/**
 * @file LogicClient.h
 * @brief gateway → logic 的 gRPC 客户端封装（Phase 1.2 W2.D1-D3 骨架）
 *
 * 当前只做 unary HandleMessage。W2.D4 起加 RegisterGateway 双向流（接 logic 反向推送）。
 */
#pragma once

#include "im/logic.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <memory>
#include <string>

class LogicClient {
public:
    explicit LogicClient(const std::string& addr) {
        channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        stub_ = im::LogicService::NewStub(channel_);
    }

    /**
     * 同步 unary HandleMessage。
     *
     * @param uid       客户端 uid（来自 ws path）
     * @param deviceId  客户端 device_id
     * @param payload   原始 ws JSON
     * @param[out] reply  logic 返回的 inline_reply（gateway 直接 ws.send 给客户端）
     * @return grpc Status
     */
    grpc::Status handleMessage(int64_t uid, const std::string& deviceId,
                               const std::string& connId, const std::string& payload,
                               std::string* reply) {
        im::MessageRequest req;
        req.mutable_env()->set_trace_id(connId + "-" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        req.mutable_env()->set_instance("gateway");
        req.mutable_env()->set_routing_key(std::to_string(uid));
        req.mutable_who()->set_uid(uid);
        req.mutable_who()->set_device_id(deviceId);
        req.mutable_who()->set_conn_id(connId);
        req.set_payload(payload);

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        im::MessageResponse resp;
        auto st = stub_->HandleMessage(&ctx, req, &resp);
        if (st.ok() && reply) *reply = resp.inline_reply();
        return st;
    }

private:
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<im::LogicService::Stub> stub_;
};
