/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#ifndef GRAPH_EXECUTOR_H_
#define GRAPH_EXECUTOR_H_

#include "base/Base.h"
#include "base/Status.h"
#include "cpp/helpers.h"
#include "graph/ExecutionContext.h"
#include "gen-cpp2/common_types.h"
#include "graph/UserAccessControl.h"


/**
 * Executor is the interface of kinds of specific executors that do the actual execution.
 */

namespace nebula {
namespace graph {

#define ACL_CHECK()                                                      \
    do {                                                                 \
        auto spaceId = ectx()->rctx()->session()->space();               \
        if (spaceId == -1) {                                             \
            spaceId = ectx()->getMetaClient()->                          \
                    getMetaDefaultSpaceIdInCache();                      \
        }                                                                \
        auto aclStatus = checkACL(spaceId,                               \
                                  ectx()->rctx()->session()->user(),     \
                                  sentence_->kind());                    \
        if (!aclStatus.ok()) {                                           \
            return aclStatus;                                            \
        }                                                                \
    } while (false)

#define ACL_CHECK_SPACE(spaceId)                                         \
    do {                                                                 \
        auto aclStatus = checkACL(spaceId,                               \
                                  ectx()->rctx()->session()->user(),     \
                                  sentence_->kind());                    \
        if (!aclStatus.ok()) {                                           \
            return aclStatus;                                            \
        }                                                                \
    } while (false)

#define ACL_CHECK_IS_GOD()                                               \
    do {                                                                 \
        const auto& userName = ectx()->rctx()->session()->user();        \
        auto isGod = ectx()->getMetaClient()->                           \
                     checkIsGodUserInCache(userName);                    \
        if (FLAGS_security_authorization_enable && !isGod) {             \
        return Status::Error("God role requested");                      \
        }                                                                \
    } while (false)

class Executor : public cpp::NonCopyable, public cpp::NonMovable {
public:
    explicit Executor(ExecutionContext *ectx) {
        ectx_ = ectx;
    }

    virtual ~Executor() {}

    /**
     * Do some preparatory works, such as sanitize checking, dependency setup, etc.
     *
     * `prepare' succeeds only if all its sub-executors are prepared.
     * `prepare' works in a synchronous way, once the executor is prepared, it will
     * be executed.
     */
    virtual Status MUST_USE_RESULT prepare() = 0;

    virtual void execute() = 0;

    virtual const char* name() const = 0;

    /**
     * Set callback to be invoked when this executor is finished(normally).
     */
    void setOnFinish(std::function<void()> onFinish) {
        onFinish_ = onFinish;
    }
    /**
     * When some error happens during an executor's execution, it should invoke its
     * `onError_' with a Status that indicates the reason.
     *
     * An executor terminates its execution via invoking either `onFinish_' or `onError_',
     * but should never call them both.
     */
    void setOnError(std::function<void(Status)> onError) {
        onError_ = onError;
    }
    /**
     * Upon finished successfully, `setupResponse' would be invoked on the last executor.
     * Any Executor implementation, which wants to send its meaningful result to the client,
     * should override this method.
     */
    virtual void setupResponse(cpp2::ExecutionResponse &resp) {
        resp.set_error_code(cpp2::ErrorCode::SUCCEEDED);
    }

    ExecutionContext* ectx() const {
        return ectx_;
    }

protected:
    std::unique_ptr<Executor> makeExecutor(Sentence *sentence);

    std::string valueTypeToString(nebula::cpp2::ValueType type);

    nebula::cpp2::SupportedType columnTypeToSupportedType(ColumnType type);

    Status checkIfGraphSpaceChosen() const {
        if (ectx()->rctx()->session()->space() == -1) {
            return Status::Error("Please choose a graph space with `USE spaceName' firstly");
        }
        return Status::OK();
    }

    Status checkACL(GraphSpaceID spaceId, const std::string& user, Sentence::Kind op) {
        if (!FLAGS_security_authorization_enable) {
            return Status::OK();
        }
        auto userRet = ectx()->getMetaClient()->getUserIdByNameFromCache(user);
        if (!userRet.ok()) {
            return userRet.status();
        }
        auto ret = UserAccessControl::checkPerms(spaceId,
                                                 userRet.value(),
                                                 op,
                                                 ectx()->getMetaClient());
        if (!ret.ok()) {
            return ret;
        }
        return Status::OK();
    }

    meta::cpp2::RoleType toRole(RoleTypeClause::RoleType type) {
        switch (type) {
            case RoleTypeClause::RoleType::GOD:
                return meta::cpp2::RoleType::GOD;
            case RoleTypeClause::RoleType::ADMIN:
                return meta::cpp2::RoleType::ADMIN;
            case RoleTypeClause::RoleType::USER:
                return meta::cpp2::RoleType::USER;
            case RoleTypeClause::RoleType::GUEST:
                return meta::cpp2::RoleType::GUEST;
        }
        return meta::cpp2::RoleType::UNKNOWN;
    }

protected:
    ExecutionContext                            *ectx_;
    std::function<void()>                       onFinish_;
    std::function<void(Status)>                 onError_;
};

}   // namespace graph
}   // namespace nebula

#endif  // GRAPH_EXECUTOR_H_
