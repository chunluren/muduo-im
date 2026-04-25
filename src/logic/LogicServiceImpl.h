/**
 * @file LogicServiceImpl.h
 * @brief Phase 1.2 W1.D3: LogicService 业务层实现（最小可跑骨架）
 *
 * 只实现 HandleMessage（unary），HandleAuth + RegisterGateway 后续 day 接。
 * 当前只做日志 + 回 ack，不真正路由消息（W2 起会接现有 MessageService）。
 */
#pragma once

#include "im/logic.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <iostream>
#include <string>

class LogicServiceImpl final : public im::LogicService::Service {
public:
    grpc::Status HandleMessage(grpc::ServerContext* /*ctx*/,
                               const im::MessageRequest* req,
                               im::MessageResponse* resp) override {
        std::cerr << "[logic] HandleMessage trace=" << req->env().trace_id()
                  << " uid=" << req->who().uid()
                  << " device=" << req->who().device_id()
                  << " payload_size=" << req->payload().size() << "\n";
        resp->mutable_status()->set_code(im::StatusCode::OK);
        resp->mutable_status()->set_msg("ack");
        // server_msg_id 由后续真正业务侧雪花生成；占位用 ts
        resp->set_server_msg_id(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        return grpc::Status::OK;
    }

    grpc::Status HandleAuth(grpc::ServerContext* /*ctx*/,
                            const im::AuthRequest* /*req*/,
                            im::AuthResponse* resp) override {
        resp->mutable_status()->set_code(im::StatusCode::INTERNAL);
        resp->mutable_status()->set_msg("HandleAuth not implemented yet");
        return grpc::Status::OK;
    }

    grpc::Status RegisterGateway(
        grpc::ServerContext* /*ctx*/,
        grpc::ServerReaderWriter<im::PushCommand, im::GatewayEvent>* stream) override {
        // W2.D4-D5 才接双向流，先把流读空让客户端可优雅退出
        im::GatewayEvent ev;
        while (stream->Read(&ev)) { /* discard */ }
        return grpc::Status::OK;
    }
};
