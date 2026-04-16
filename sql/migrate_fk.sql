-- 清理悬挂引用后添加外键（用于已有数据库迁移）
USE muduo_im;

-- 清理悬挂引用
DELETE FROM friends WHERE user_id NOT IN (SELECT id FROM users);
DELETE FROM friends WHERE friend_id NOT IN (SELECT id FROM users);
DELETE FROM `groups` WHERE owner_id NOT IN (SELECT id FROM users);
DELETE FROM group_members WHERE user_id NOT IN (SELECT id FROM users);
DELETE FROM group_members WHERE group_id NOT IN (SELECT id FROM `groups`);
DELETE FROM friend_requests WHERE from_user NOT IN (SELECT id FROM users);
DELETE FROM friend_requests WHERE to_user NOT IN (SELECT id FROM users);

-- 应用外键约束（若已存在会报错，需要先 DROP）
ALTER TABLE friends ADD CONSTRAINT fk_friends_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE friends ADD CONSTRAINT fk_friends_friend FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE `groups` ADD CONSTRAINT fk_groups_owner FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE group_members ADD CONSTRAINT fk_gm_group FOREIGN KEY (group_id) REFERENCES `groups`(id) ON DELETE CASCADE;
ALTER TABLE group_members ADD CONSTRAINT fk_gm_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE friend_requests ADD CONSTRAINT fk_fr_from FOREIGN KEY (from_user) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE friend_requests ADD CONSTRAINT fk_fr_to FOREIGN KEY (to_user) REFERENCES users(id) ON DELETE CASCADE;
