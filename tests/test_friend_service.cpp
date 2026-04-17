#include "test_helper.h"
#include "server/UserService.h"
#include "server/FriendService.h"
#include <iostream>

struct UserPair {
    int64_t alice;
    int64_t bob;
};

UserPair createUsers() {
    UserService usvc(getTestDb(), "secret");
    auto a = usvc.registerUser("alice_fs", "p", "");
    auto b = usvc.registerUser("bob_fs", "p", "");
    return {a["userId"].get<int64_t>(), b["userId"].get<int64_t>()};
}

void testFriendRequestAcceptFlow() {
    std::cout << "=== testFriendRequestAcceptFlow ===" << std::endl;
    cleanTestDb();
    auto [alice, bob] = createUsers();
    FriendService fsvc(getTestDb());

    auto req = fsvc.sendRequest(alice, bob);
    ASSERT_TRUE(req["success"].get<bool>());

    auto requests = fsvc.getRequests(bob);
    ASSERT_EQ(requests.size(), (size_t)1);
    int64_t reqId = requests[0]["requestId"].get<int64_t>();

    auto handle = fsvc.handleRequest(bob, reqId, true);
    ASSERT_TRUE(handle["success"].get<bool>());

    // Both sides should see each other as friends now
    auto aliceFriends = fsvc.getFriends(alice);
    auto bobFriends = fsvc.getFriends(bob);
    ASSERT_EQ(aliceFriends.size(), (size_t)1);
    ASSERT_EQ(bobFriends.size(), (size_t)1);

    std::cout << "PASS" << std::endl;
}

void testFriendRequestReject() {
    std::cout << "=== testFriendRequestReject ===" << std::endl;
    cleanTestDb();
    auto [alice, bob] = createUsers();
    FriendService fsvc(getTestDb());

    fsvc.sendRequest(alice, bob);
    auto requests = fsvc.getRequests(bob);
    int64_t reqId = requests[0]["requestId"].get<int64_t>();

    fsvc.handleRequest(bob, reqId, false);

    auto aliceFriends = fsvc.getFriends(alice);
    ASSERT_EQ(aliceFriends.size(), (size_t)0);

    std::cout << "PASS" << std::endl;
}

void testCannotAddSelf() {
    std::cout << "=== testCannotAddSelf ===" << std::endl;
    cleanTestDb();
    auto [alice, _] = createUsers();
    FriendService fsvc(getTestDb());

    auto req = fsvc.sendRequest(alice, alice);
    ASSERT_TRUE(!req["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

void testDeleteFriend() {
    std::cout << "=== testDeleteFriend ===" << std::endl;
    cleanTestDb();
    auto [alice, bob] = createUsers();
    FriendService fsvc(getTestDb());

    fsvc.addFriend(alice, bob);
    ASSERT_EQ(fsvc.getFriends(alice).size(), (size_t)1);

    fsvc.deleteFriend(alice, bob);
    ASSERT_EQ(fsvc.getFriends(alice).size(), (size_t)0);
    ASSERT_EQ(fsvc.getFriends(bob).size(), (size_t)0);

    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "Starting FriendService tests..." << std::endl;
    testFriendRequestAcceptFlow();
    testFriendRequestReject();
    testCannotAddSelf();
    testDeleteFriend();
    std::cout << "All FriendService tests passed!" << std::endl;
    return 0;
}
