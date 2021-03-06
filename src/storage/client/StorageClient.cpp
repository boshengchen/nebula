/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "base/Base.h"
#include "storage/client/StorageClient.h"

#define ID_HASH(id, numShards) \
    ((static_cast<uint64_t>(id)) % numShards + 1)

namespace nebula {
namespace storage {

StorageClient::StorageClient(std::shared_ptr<folly::IOThreadPoolExecutor> threadPool,
                             meta::MetaClient *client)
        : ioThreadPool_(threadPool)
        , client_(client) {
    clientsMan_
        = std::make_unique<thrift::ThriftClientManager<storage::cpp2::StorageServiceAsyncClient>>();
}


StorageClient::~StorageClient() {
    VLOG(3) << "~StorageClient";
    if (nullptr != client_) {
        client_ = nullptr;
    }
}


folly::SemiFuture<StorageRpcResponse<cpp2::ExecResponse>> StorageClient::addVertices(
        GraphSpaceID space,
        std::vector<cpp2::Vertex> vertices,
        bool overwritable,
        folly::EventBase* evb) {
    auto clusters = clusterIdsToHosts(
        space,
        vertices,
        [] (const cpp2::Vertex& v) {
            return v.get_id();
        });

    std::unordered_map<HostAddr, cpp2::AddVerticesRequest> requests;
    for (auto& c : clusters) {
        auto& host = c.first;
        auto& req = requests[host];
        req.set_space_id(space);
        req.set_overwritable(overwritable);
        req.set_parts(std::move(c.second));
    }

    VLOG(3) << "requests size " << requests.size();
    return collectResponse(
        evb, std::move(requests),
        [](cpp2::StorageServiceAsyncClient* client,
           const cpp2::AddVerticesRequest& r) {
            return client->future_addVertices(r);
        });
}


folly::SemiFuture<StorageRpcResponse<cpp2::ExecResponse>> StorageClient::addEdges(
        GraphSpaceID space,
        std::vector<storage::cpp2::Edge> edges,
        bool overwritable,
        folly::EventBase* evb) {
    auto clusters = clusterIdsToHosts(
        space,
        edges,
        [] (const cpp2::Edge& e) {
            return e.get_key().get_src();
        });

    std::unordered_map<HostAddr, cpp2::AddEdgesRequest> requests;
    for (auto& c : clusters) {
        auto& host = c.first;
        auto& req = requests[host];
        req.set_space_id(space);
        req.set_overwritable(overwritable);
        req.set_parts(std::move(c.second));
    }

    return collectResponse(
        evb, std::move(requests),
        [](cpp2::StorageServiceAsyncClient* client,
           const cpp2::AddEdgesRequest& r) {
            return client->future_addEdges(r);
        });
}


folly::SemiFuture<StorageRpcResponse<cpp2::QueryResponse>> StorageClient::getNeighbors(
        GraphSpaceID space,
        const std::vector<VertexID> &vertices,
        const std::vector<EdgeType> &edgeTypes,
        std::string filter,
        std::vector<cpp2::PropDef> returnCols,
        folly::EventBase* evb) {
    auto clusters = clusterIdsToHosts(
        space,
        vertices,
        [] (const VertexID& v) {
            return v;
        });

    std::unordered_map<HostAddr, cpp2::GetNeighborsRequest> requests;
    for (auto& c : clusters) {
        auto& host = c.first;
        auto& req = requests[host];
        req.set_space_id(space);
        req.set_parts(std::move(c.second));
        req.set_edge_types(edgeTypes);
        req.set_filter(filter);
        req.set_return_columns(returnCols);
    }

    return collectResponse(
        evb, std::move(requests),
        [](cpp2::StorageServiceAsyncClient* client, const cpp2::GetNeighborsRequest& r) {
            return client->future_getBound(r);
        });
}


folly::SemiFuture<StorageRpcResponse<cpp2::QueryStatsResponse>> StorageClient::neighborStats(
        GraphSpaceID space,
        std::vector<VertexID> vertices,
        std::vector<EdgeType> edgeTypes,
        std::string filter,
        std::vector<cpp2::PropDef> returnCols,
        folly::EventBase* evb) {
    auto clusters = clusterIdsToHosts(
        space,
        vertices,
        [] (const VertexID& v) {
            return v;
        });

    std::unordered_map<HostAddr, cpp2::GetNeighborsRequest> requests;
    for (auto& c : clusters) {
        auto& host = c.first;
        auto& req = requests[host];
        req.set_space_id(space);
        req.set_parts(std::move(c.second));
        // Make edge type a negative number when query in-bound
        req.set_edge_types(edgeTypes);
        req.set_filter(filter);
        req.set_return_columns(returnCols);
    }

    return collectResponse(
        evb, std::move(requests),
        [](cpp2::StorageServiceAsyncClient* client, const cpp2::GetNeighborsRequest& r) {
            return client->future_boundStats(r);
        });
}


folly::SemiFuture<StorageRpcResponse<cpp2::QueryResponse>> StorageClient::getVertexProps(
        GraphSpaceID space,
        std::vector<VertexID> vertices,
        std::vector<cpp2::PropDef> returnCols,
        folly::EventBase* evb) {
    auto clusters = clusterIdsToHosts(
        space,
        vertices,
        [] (const VertexID& v) {
            return v;
        });

    std::unordered_map<HostAddr, cpp2::VertexPropRequest> requests;
    for (auto& c : clusters) {
        auto& host = c.first;
        auto& req = requests[host];
        req.set_space_id(space);
        req.set_parts(std::move(c.second));
        req.set_return_columns(returnCols);
    }

    return collectResponse(
        evb, std::move(requests),
        [](cpp2::StorageServiceAsyncClient* client,
           const cpp2::VertexPropRequest& r) {
            return client->future_getProps(r);
        });
}


folly::SemiFuture<StorageRpcResponse<cpp2::EdgePropResponse>> StorageClient::getEdgeProps(
        GraphSpaceID space,
        std::vector<cpp2::EdgeKey> edges,
        std::vector<cpp2::PropDef> returnCols,
        folly::EventBase* evb) {
    auto clusters = clusterIdsToHosts(
        space,
        edges,
        [] (const cpp2::EdgeKey& v) {
            return v.get_src();
        });

    std::unordered_map<HostAddr, cpp2::EdgePropRequest> requests;
    for (auto& c : clusters) {
        auto& host = c.first;
        auto& req = requests[host];
        req.set_space_id(space);
        for (auto& p : c.second) {
            req.set_edge_type((p.second[0].edge_type));
            break;
        }
        req.set_parts(std::move(c.second));
        req.set_return_columns(returnCols);
    }

    return collectResponse(
        evb, std::move(requests),
        [](cpp2::StorageServiceAsyncClient* client,
           const cpp2::EdgePropRequest& r) {
            return client->future_getEdgeProps(r);
        });
}


folly::Future<StatusOr<cpp2::EdgeKeyResponse>> StorageClient::getEdgeKeys(
    GraphSpaceID space,
    VertexID vid,
    folly::EventBase* evb) {
    std::pair<HostAddr, cpp2::EdgeKeyRequest> request;
    PartitionID part = partId(space, vid);
    auto partMeta = getPartMeta(space, part);
    CHECK_GT(partMeta.peers_.size(), 0U);
    const auto& leader = this->leader(partMeta);
    request.first = leader;

    cpp2::EdgeKeyRequest req;
    req.set_space_id(space);
    req.set_part_id(part);
    req.set_vid(vid);
    request.second = std::move(req);

    return getResponse(
        evb,
        std::move(request),
        [] (cpp2::StorageServiceAsyncClient* client,
            const cpp2::EdgeKeyRequest& r) {
            return client->future_getEdgeKeys(r);
    });
}


folly::SemiFuture<StorageRpcResponse<cpp2::ExecResponse>> StorageClient::deleteEdges(
    GraphSpaceID space,
    std::vector<storage::cpp2::EdgeKey> edges,
    folly::EventBase* evb) {
    auto clusters = clusterIdsToHosts(
        space,
        edges,
        [] (const cpp2::EdgeKey& v) {
            return v.get_src();
        });

    std::unordered_map<HostAddr, cpp2::DeleteEdgesRequest> requests;
    for (auto& c : clusters) {
        auto& host = c.first;
        auto& req = requests[host];
        req.set_space_id(space);
        req.set_parts(std::move(c.second));
    }

    return collectResponse(
        evb, std::move(requests),
        [](cpp2::StorageServiceAsyncClient* client,
           const cpp2::DeleteEdgesRequest& r) {
            return client->future_deleteEdges(r);
        });
}


folly::Future<StatusOr<cpp2::ExecResponse>> StorageClient::deleteVertex(
    GraphSpaceID space,
    VertexID vid,
    folly::EventBase* evb) {
    std::pair<HostAddr, cpp2::DeleteVertexRequest> request;
    PartitionID part = partId(space, vid);
    auto partMeta = getPartMeta(space, part);
    CHECK_GT(partMeta.peers_.size(), 0U);
    const auto& leader = this->leader(partMeta);
    request.first = leader;

    cpp2::DeleteVertexRequest req;
    req.set_space_id(space);
    req.set_part_id(part);
    req.set_vid(vid);
    request.second = std::move(req);

    return getResponse(
        evb,
        std::move(request),
        [] (cpp2::StorageServiceAsyncClient* client,
            const cpp2::DeleteVertexRequest& r) {
            return client->future_deleteVertex(r);
    });
}


PartitionID StorageClient::partId(GraphSpaceID spaceId, int64_t id) const {
    auto parts = partsNum(spaceId);
    auto s = ID_HASH(id, parts);
    CHECK_GE(s, 0U);
    return s;
}
}   // namespace storage
}   // namespace nebula
