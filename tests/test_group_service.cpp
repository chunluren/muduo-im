#include "test_helper.h"
#include "server/UserService.h"
#include "server/GroupService.h"
#include <iostream>

void testCreateAndJoinGroup() {
    std::cout << "=== testCreateAndJoinGroup ===" << std::endl;
    cleanTestDb();
    UserService usvc(getTestDb(), "secret");
    int64_t alice = usvc.registerUser("alice_gs", "p", "")["userId"].get<int64_t>();
    int64_t bob = usvc.registerUser("bob_gs", "p", "")["userId"].get<int64_t>();
    GroupService gsvc(getTestDb());

    auto create = gsvc.createGroup(alice, "Test Group");
    ASSERT_TRUE(create["success"].get<bool>());
    int64_t groupId = create["groupId"].get<int64_t>();

    auto join = gsvc.joinGroup(bob, groupId);
    ASSERT_TRUE(join["success"].get<bool>());

    auto members = gsvc.getMembers(groupId);
    ASSERT_EQ(members.size(), (size_t)2);

    std::cout << "PASS" << std::endl;
}

void testOwnerCannotLeave() {
    std::cout << "=== testOwnerCannotLeave ===" << std::endl;
    cleanTestDb();
    UserService usvc(getTestDb(), "secret");
    int64_t alice = usvc.registerUser("alice_o", "p", "")["userId"].get<int64_t>();
    GroupService gsvc(getTestDb());

    int64_t groupId = gsvc.createGroup(alice, "G")["groupId"].get<int64_t>();
    auto leave = gsvc.leaveGroup(alice, groupId);
    ASSERT_TRUE(!leave["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

void testOnlyOwnerCanDelete() {
    std::cout << "=== testOnlyOwnerCanDelete ===" << std::endl;
    cleanTestDb();
    UserService usvc(getTestDb(), "secret");
    int64_t alice = usvc.registerUser("alice_d", "p", "")["userId"].get<int64_t>();
    int64_t bob = usvc.registerUser("bob_d", "p", "")["userId"].get<int64_t>();
    GroupService gsvc(getTestDb());

    int64_t groupId = gsvc.createGroup(alice, "G")["groupId"].get<int64_t>();
    gsvc.joinGroup(bob, groupId);

    // Bob (not owner) tries to delete
    auto del = gsvc.deleteGroup(bob, groupId);
    ASSERT_TRUE(!del["success"].get<bool>());

    // Alice (owner) deletes
    auto delByOwner = gsvc.deleteGroup(alice, groupId);
    ASSERT_TRUE(delByOwner["success"].get<bool>());

    std::cout << "PASS" << std::endl;
}

void testKickMember() {
    std::cout << "=== testKickMember ===" << std::endl;
    cleanTestDb();
    UserService usvc(getTestDb(), "secret");
    int64_t alice = usvc.registerUser("alice_k", "p", "")["userId"].get<int64_t>();
    int64_t bob = usvc.registerUser("bob_k", "p", "")["userId"].get<int64_t>();
    GroupService gsvc(getTestDb());

    int64_t groupId = gsvc.createGroup(alice, "G")["groupId"].get<int64_t>();
    gsvc.joinGroup(bob, groupId);
    ASSERT_EQ(gsvc.getMembers(groupId).size(), (size_t)2);

    auto kick = gsvc.kickMember(alice, groupId, bob);
    ASSERT_TRUE(kick["success"].get<bool>());
    ASSERT_EQ(gsvc.getMembers(groupId).size(), (size_t)1);

    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "Starting GroupService tests..." << std::endl;
    testCreateAndJoinGroup();
    testOwnerCannotLeave();
    testOnlyOwnerCanDelete();
    testKickMember();
    std::cout << "All GroupService tests passed!" << std::endl;
    return 0;
}
