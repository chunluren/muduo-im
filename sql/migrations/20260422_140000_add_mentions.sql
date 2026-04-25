-- Feature: @ mention support (Phase 2.3)
-- Author: chunluren
-- Ticket: #8
-- Affects: group_messages 加 mentions JSON 字段
-- Estimated duration: < 5s（现在表里 0 行）

-- +migrate Up
ALTER TABLE group_messages
    ADD COLUMN mentions JSON NULL COMMENT '被 @ 的 userId 列表，[12345, 67890]';

-- 索引：按"是否含 mention"快速过滤（生成列 + 索引）
-- 用 JSON_LENGTH 判断是否非空数组
ALTER TABLE group_messages
    ADD COLUMN has_mentions TINYINT GENERATED ALWAYS AS
        (CASE WHEN JSON_LENGTH(mentions) > 0 THEN 1 ELSE 0 END) STORED,
    ADD INDEX idx_group_mentions (group_id, has_mentions, timestamp);

-- +migrate Down
ALTER TABLE group_messages
    DROP INDEX idx_group_mentions,
    DROP COLUMN has_mentions,
    DROP COLUMN mentions;
