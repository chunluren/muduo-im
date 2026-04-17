#include "test_helper.h"
#include "server/UserService.h"
#include "server/MessageService.h"
#include "common/Protocol.h"
#include <iostream>
#include <chrono>

void testSaveAndRetrievePrivateMessage() {
    std::cout << "=== testSaveAndRetrievePrivateMessage ===" << std::endl;
    cleanTestDb();
    UserService usvc(getTestDb(), "secret");
    int64_t alice = usvc.registerUser("alice_m", "pass1234", "")["userId"].get<int64_t>();
    int64_t bob = usvc.registerUser("bob_m", "pass1234", "")["userId"].get<int64_t>();
    MessageService msvc(getTestDb());

    int64_t ts = Protocol::nowMs();
    bool saved = msvc.savePrivateMessage("msg-1", alice, bob, "Hello", ts);
    ASSERT_TRUE(saved);

    auto history = msvc.getPrivateHistory(alice, bob, 50, 0);
    ASSERT_EQ(history.size(), (size_t)1);

    std::cout << "PASS" << std::endl;
}

void testRecallMessage() {
    std::cout << "=== testRecallMessage ===" << std::endl;
    cleanTestDb();
    UserService usvc(getTestDb(), "secret");
    int64_t alice = usvc.registerUser("alice_r", "pass1234", "")["userId"].get<int64_t>();
    int64_t bob = usvc.registerUser("bob_r", "pass1234", "")["userId"].get<int64_t>();
    MessageService msvc(getTestDb());

    int64_t ts = Protocol::nowMs();
    msvc.savePrivateMessage("msg-r", alice, bob, "to recall", ts);

    bool recalled = msvc.recallMessage("msg-r", alice);
    ASSERT_TRUE(recalled);

    // Recalled message should not appear in history
    auto history = msvc.getPrivateHistory(alice, bob, 50, 0);
    ASSERT_EQ(history.size(), (size_t)0);

    std::cout << "PASS" << std::endl;
}

void testRecallByOtherUser() {
    std::cout << "=== testRecallByOtherUser ===" << std::endl;
    cleanTestDb();
    UserService usvc(getTestDb(), "secret");
    int64_t alice = usvc.registerUser("alice_x", "pass1234", "")["userId"].get<int64_t>();
    int64_t bob = usvc.registerUser("bob_x", "pass1234", "")["userId"].get<int64_t>();
    MessageService msvc(getTestDb());

    msvc.savePrivateMessage("msg-x", alice, bob, "from alice", Protocol::nowMs());

    // Bob tries to recall Alice's message
    bool recalled = msvc.recallMessage("msg-x", bob);
    ASSERT_TRUE(!recalled);

    std::cout << "PASS" << std::endl;
}

void testSearchMessages() {
    std::cout << "=== testSearchMessages ===" << std::endl;
    cleanTestDb();
    UserService usvc(getTestDb(), "secret");
    int64_t alice = usvc.registerUser("alice_s", "pass1234", "")["userId"].get<int64_t>();
    int64_t bob = usvc.registerUser("bob_s", "pass1234", "")["userId"].get<int64_t>();
    MessageService msvc(getTestDb());

    int64_t ts = Protocol::nowMs();
    msvc.savePrivateMessage("m1", alice, bob, "hello world", ts);
    msvc.savePrivateMessage("m2", alice, bob, "goodbye", ts + 1);
    msvc.savePrivateMessage("m3", alice, bob, "hello again", ts + 2);

    auto search = msvc.searchMessages(alice, "hello", 50);
    ASSERT_TRUE(search["success"].get<bool>());
    auto messages = search["messages"];
    ASSERT_EQ(messages.size(), (size_t)2);

    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "Starting MessageService tests..." << std::endl;
    testSaveAndRetrievePrivateMessage();
    testRecallMessage();
    testRecallByOtherUser();
    testSearchMessages();
    std::cout << "All MessageService tests passed!" << std::endl;
    return 0;
}
