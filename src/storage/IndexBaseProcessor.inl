/* Copyright (c) 2019 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "base/Base.h"
#include "storage/IndexBaseProcessor.h"


DECLARE_int32(bulk_number_per_index_creation);

namespace nebula {
namespace storage {

template<typename RESP>
void IndexBaseProcessor<RESP>::finishProcess(cpp2::ResultCode thriftResult) {
    bool finished = false;
    {
        std::lock_guard<std::mutex> lg(this->lock_);
        if (thriftResult.code != cpp2::ErrorCode::SUCCEEDED) {
            this->codes_.emplace_back(thriftResult);
        }
        this->callingNum_--;
        if (this->callingNum_ == 0) {
            this->result_.set_failed_codes(std::move(this->codes_));
            finished = true;
        }
    }
    if (finished) {
        this->kvstore_->deleteSnapshot(spaceId_);
        this->onFinished();
    }
}

template<typename RESP>
void IndexBaseProcessor<RESP>::doIndexCreate(PartitionID partId) {
    std::string key, val;
    StatusOr<std::string> indexKey;
    std::vector<kvstore::KV> data;

    int32_t batchPutNum = 0;
    cpp2::ResultCode thriftResult;
    thriftResult.set_code(cpp2::ErrorCode::SUCCEEDED);
    thriftResult.set_part_id(partId);

    auto prefix = NebulaKeyUtils::partPrefix(partId);
    std::unique_ptr<kvstore::KVIterator> iter;
    auto ret = this->kvstore_->prefixSnapshot(spaceId_, partId, prefix, &iter);

    if (ret != kvstore::ResultCode::SUCCEEDED || !iter) {
        thriftResult.set_code(this->to(ret));
        finishProcess(thriftResult);
        return;
    }

    while (iter->valid()) {
        key = iter->key().str();
        val = iter->val().str();
        iter->next();
        indexKey = (indexType_ == nebula::cpp2::IndexType::EDGE) ?
                   assembleEdgeIndexKey(spaceId_, partId, key, std::move(val)) :
                   assembleVertexIndexKey(spaceId_, partId, key, std::move(val));

        if (!indexKey.ok()) {
            continue;
        }
        data.emplace_back(std::move(indexKey.value()), std::move(key));
        batchPutNum++;
        if (batchPutNum == FLAGS_bulk_number_per_index_creation) {
            auto code = doBatchPut(spaceId_, partId, std::move(data));
            if (code == cpp2::ErrorCode::E_LEADER_CHANGED) {
                nebula::cpp2::HostAddr leader;
                auto addrRet = this->kvstore_->partLeader(spaceId_, partId);
                CHECK(ok(addrRet));
                auto addr = value(std::move(addrRet));
                leader.set_ip(addr.first);
                leader.set_port(addr.second);
                thriftResult.set_leader(leader);
            } else if (code != cpp2::ErrorCode::SUCCEEDED) {
                thriftResult.set_code(code);
                finishProcess(thriftResult);
                return;
            }
            batchPutNum = 0;
        }
    }

    if (!data.empty()) {
        auto code = doBatchPut(spaceId_, partId, std::move(data));
        if (code == cpp2::ErrorCode::E_LEADER_CHANGED) {
            nebula::cpp2::HostAddr leader;
            auto addrRet = this->kvstore_->partLeader(spaceId_, partId);
            CHECK(ok(addrRet));
            auto addr = value(std::move(addrRet));
            leader.set_ip(addr.first);
            leader.set_port(addr.second);
            thriftResult.set_leader(leader);
        } else if (code != cpp2::ErrorCode::SUCCEEDED) {
            thriftResult.set_code(code);
            finishProcess(thriftResult);
            return;
        }
    }

    // TODO : send index status 'CONSTRUCTING' via meta client,
    //        should block all actions of insert|delete| update.
    //        until index create done of each parts.

    // TODO : Assemble new data changes during index creation.

    finishProcess(thriftResult);
}

template<typename RESP>
cpp2::ErrorCode IndexBaseProcessor<RESP>::doBatchPut(GraphSpaceID spaceId, PartitionID partId,
                                          std::vector<kvstore::KV> data) {
    cpp2::ErrorCode ret = cpp2::ErrorCode::SUCCEEDED;
    folly::Baton<true, std::atomic> baton;
    this->kvstore_->asyncMultiPut(spaceId, partId, std::move(data),
                            [&] (kvstore::ResultCode code) {
                                if (code != kvstore::ResultCode::SUCCEEDED) {
                                    ret = this->to(code);
                                }
                                baton.post();
                            });
    baton.wait();
    return ret;
}

template<typename RESP>
StatusOr<std::string> IndexBaseProcessor<RESP>::assembleEdgeIndexKey(GraphSpaceID spaceId,
        PartitionID partId, std::string key, std::string val) {
    if (folly::to<int32_t>(key.size()) != NebulaKeyUtils::getEdgeLen()) {
        return Status::Error("Skip this row");
    }
    auto edgeType = NebulaKeyUtils::parseEdgeType(key);
    auto reader = RowReader::getEdgePropReader(this->schemaMan_, val, spaceId, edgeType);
    for (const auto &prop : props_) {
        auto ret = this->schemaMan_->getNewestEdgeSchemaVer(spaceId, edgeType);
        if (!ret.ok()) {
            return Status::Error("Space %d edge %d invalid", spaceId, edgeType);
        }
        if (edgeType == prop.first && ret.value() == reader->schemaVer()) {
            auto propVal = this->collectColsVal(reader.get(), prop.second);
            if (!propVal.ok()) {
                return Status::Error("Get edge Prop failing");
            }
            auto ver = NebulaKeyUtils::parseEdgeVersion(key);
            auto raw = NebulaKeyUtils::edgeIndexkey(partId, indexId_,
                                                    edgeType, ver, propVal.value());
            return raw;
        }
    }
    return Status::Error("Not is newly version by edge");
}

template<typename RESP>
StatusOr<std::string> IndexBaseProcessor<RESP>::assembleVertexIndexKey(GraphSpaceID spaceId,
        PartitionID partId, std::string key, std::string val) {
    if (folly::to<int32_t>(key.size()) != NebulaKeyUtils::getVertexLen()) {
        return Status::Error("Skip this row");
    }

    auto tagId = NebulaKeyUtils::parseTagId(key);
    auto reader = RowReader::getTagPropReader(this->schemaMan_, val, spaceId, tagId);
    for (const auto &prop : props_) {
        auto ret = this->schemaMan_->getNewestTagSchemaVer(spaceId, tagId);
        if (!ret.ok()) {
            return Status::Error("Space %d tag %d invalid", spaceId, tagId);
        }
        if (tagId == prop.first && ret.value() == reader->schemaVer()) {
            auto propVal = this->collectColsVal(reader.get(), prop.second);
            if (!propVal.ok()) {
                return Status::Error("Get tag Prop failing");
            }
            auto ver = NebulaKeyUtils::parseTagVersion(key);
            auto vId = NebulaKeyUtils::parseVertexId(key);
            auto raw = NebulaKeyUtils::tagIndexkey(partId, indexId_, vId, ver, propVal.value());
            return raw;
        }
    }
    return Status::Error("Not is newly version by vertex");
}

}  // namespace storage
}  // namespace nebula