/**
 * @file grpc_logic_smoke.cpp
 * @brief Phase 1.2 W1.D3 smoke test：调一次 LogicService::HandleMessage
 *
 * 假设 muduo-im-logic 已在 127.0.0.1:9100 启动。
 */
#include "im/logic.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string addr = (argc > 1) ? argv[1] : "127.0.0.1:9100";
    auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    auto stub = im::LogicService::NewStub(channel);

    im::MessageRequest req;
    req.mutable_env()->set_trace_id("smoke-001");
    req.mutable_env()->set_instance("smoke-client");
    req.mutable_env()->set_routing_key("42");
    req.mutable_who()->set_uid(42);
    req.mutable_who()->set_device_id("smoke-device");
    req.mutable_who()->set_conn_id("c-1");
    req.set_payload(R"({"type":"msg","to":"99","content":"hi from grpc"})");

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    im::MessageResponse resp;
    auto st = stub->HandleMessage(&ctx, req, &resp);
    if (!st.ok()) {
        std::cerr << "RPC failed: " << st.error_code() << " " << st.error_message() << "\n";
        return 1;
    }
    std::cout << "HandleMessage OK: status=" << resp.status().code()
              << " msg=" << resp.status().msg()
              << " server_msg_id=" << resp.server_msg_id() << "\n";
    assert(resp.status().code() == im::StatusCode::OK);
    return 0;
}
