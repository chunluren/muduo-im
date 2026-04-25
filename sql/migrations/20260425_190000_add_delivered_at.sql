-- Feature: 客户端 ACK 双向回执（Phase 2.1）
-- Author: chunluren
-- Ticket: #6
-- Affects: private_messages 加 delivered_at；group_messages 加 delivered_count
-- Estimated duration: < 5s

-- +migrate Up
ALTER TABLE private_messages
    ADD COLUMN delivered_at BIGINT NULL COMMENT '对端 ACK 时间戳（毫秒），NULL=尚未送达';

ALTER TABLE group_messages
    ADD COLUMN delivered_count INT DEFAULT 0 COMMENT '群聊已送达成员数（每个 ACK +1）';

-- 索引：扫描"未送达"消息（用于离线上线后批量 deliver）
ALTER TABLE private_messages
    ADD INDEX idx_undelivered (to_user, delivered_at, timestamp);

-- +migrate Down
ALTER TABLE private_messages DROP INDEX idx_undelivered, DROP COLUMN delivered_at;
ALTER TABLE group_messages DROP COLUMN delivered_count;
