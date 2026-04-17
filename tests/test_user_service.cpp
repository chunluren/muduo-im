#include "test_helper.h"
#include "server/UserService.h"
#include <iostream>

void testRegisterAndLogin() {
    std::cout << "=== testRegisterAndLogin ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    auto reg = svc.registerUser("alice", "pass1234", "Alice");
    ASSERT_TRUE(reg["success"].get<bool>());

    auto login = svc.login("alice", "pass1234");
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

    svc.registerUser("bobby", "pass1234", "");
    auto dup = svc.registerUser("bobby", "pass1234", "");
    ASSERT_TRUE(!dup["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

void testLoginWrongPassword() {
    std::cout << "=== testLoginWrongPassword ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    svc.registerUser("charlie", "right123", "");
    auto login = svc.login("charlie", "wrong123");
    ASSERT_TRUE(!login["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

void testProfileUpdate() {
    std::cout << "=== testProfileUpdate ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    auto reg = svc.registerUser("dave123", "pass1234", "Dave");
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

    svc.registerUser("alice123", "pass1234", "");
    svc.registerUser("alice456", "pass1234", "");
    svc.registerUser("bobby99", "pass1234", "");

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

    auto reg = svc.registerUser("evelyn", "oldpass1", "");
    int64_t userId = reg["userId"].get<int64_t>();

    auto change = svc.changePassword(userId, "oldpass1", "newpass1");
    ASSERT_TRUE(change["success"].get<bool>());

    // Old password should fail
    auto loginOld = svc.login("evelyn", "oldpass1");
    ASSERT_TRUE(!loginOld["success"].get<bool>());

    // New password should work
    auto loginNew = svc.login("evelyn", "newpass1");
    ASSERT_TRUE(loginNew["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

void testWeakPasswordRejected() {
    std::cout << "=== testWeakPasswordRejected ===" << std::endl;
    cleanTestDb();
    UserService svc(getTestDb(), "test-secret");

    // Too short
    auto tooShort = svc.registerUser("user1", "pass1", "");
    ASSERT_TRUE(!tooShort["success"].get<bool>());

    // No digit
    auto noDigit = svc.registerUser("user2", "onlyletters", "");
    ASSERT_TRUE(!noDigit["success"].get<bool>());

    // No letter
    auto noAlpha = svc.registerUser("user3", "12345678", "");
    ASSERT_TRUE(!noAlpha["success"].get<bool>());

    // OK
    auto ok = svc.registerUser("user4", "good1234", "");
    ASSERT_TRUE(ok["success"].get<bool>());

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
    testWeakPasswordRejected();
    std::cout << "All UserService tests passed!" << std::endl;
    return 0;
}
