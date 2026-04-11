#include <iostream>
#include "common/JWT.h"
#include "common/Protocol.h"
#include "server/UserService.h"
#include "server/OnlineManager.h"
#include "server/FriendService.h"
#include "server/GroupService.h"
#include "server/MessageService.h"

int main() {
    // Quick JWT test
    JWT jwt("test-secret");
    auto token = jwt.generate(1);
    auto userId = jwt.verify(token);
    std::cout << "JWT test: userId=" << userId << " (expected 1)" << std::endl;

    // Protocol test
    std::cout << "MsgId: " << Protocol::generateMsgId() << std::endl;
    std::cout << "Hash: " << Protocol::hashPassword("test123") << std::endl;

    std::cout << "All modules compiled successfully!" << std::endl;
    return 0;
}
