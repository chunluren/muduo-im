#include "test_helper.h"
#include "server/UserService.h"
#include <iostream>

void testRegisterAndLogin() {
    std::cout << "=== testRegisterAndLogin ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    auto reg = svc.registerUser("alice", "pass123", "Alice");
    ASSERT_TRUE(reg["success"].get<bool>());

    auto login = svc.login("alice", "pass123");
    ASSERT_TRUE(login["success"].get<bool>());
    ASSERT_TRUE(!login["token"].get<std::string>().empty());

    int64_t userId = svc.verifyToken(login["token"].get<std::string>());
    ASSERT_TRUE(userId > 0);

    std::cout << "PASS" << std::endl;
}

void testRegisterDuplicate() {
    std::cout << "=== testRegisterDuplicate ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    svc.registerUser("bob", "pass", "");
    auto dup = svc.registerUser("bob", "pass", "");
    ASSERT_TRUE(!dup["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

void testLoginWrongPassword() {
    std::cout << "=== testLoginWrongPassword ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    svc.registerUser("charlie", "right", "");
    auto login = svc.login("charlie", "wrong");
    ASSERT_TRUE(!login["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

void testProfileUpdate() {
    std::cout << "=== testProfileUpdate ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    auto reg = svc.registerUser("dave", "pass", "Dave");
    int64_t userId = reg["userId"].get<int64_t>();

    svc.updateProfile(userId, "NewDave", "https://example.com/avatar.png");
    auto profile = svc.getProfile(userId);
    ASSERT_EQ(profile["nickname"].get<std::string>(), std::string("NewDave"));

    std::cout << "PASS" << std::endl;
}

void testSearchUsers() {
    std::cout << "=== testSearchUsers ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    svc.registerUser("alice123", "p", "");
    svc.registerUser("alice456", "p", "");
    svc.registerUser("bob", "p", "");

    auto search = svc.searchUsers("alice");
    ASSERT_TRUE(search["success"].get<bool>());
    auto users = search["users"];
    ASSERT_EQ(users.size(), (size_t)2);

    std::cout << "PASS" << std::endl;
}

void testChangePassword() {
    std::cout << "=== testChangePassword ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    auto reg = svc.registerUser("eve", "oldpass", "");
    int64_t userId = reg["userId"].get<int64_t>();

    auto change = svc.changePassword(userId, "oldpass", "newpass");
    ASSERT_TRUE(change["success"].get<bool>());

    // Old password should fail
    auto loginOld = svc.login("eve", "oldpass");
    ASSERT_TRUE(!loginOld["success"].get<bool>());

    // New password should work
    auto loginNew = svc.login("eve", "newpass");
    ASSERT_TRUE(loginNew["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "Starting UserService tests..." << std::endl;
    testRegisterAndLogin();
    testRegisterDuplicate();
    testLoginWrongPassword();
    testProfileUpdate();
    testSearchUsers();
    testChangePassword();
    std::cout << "All UserService tests passed!" << std::endl;
    return 0;
}
