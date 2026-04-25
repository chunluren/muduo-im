-- Feature: 消息 Reactions（Phase 4.5）
-- Author: chunluren
-- Ticket: #16
-- Affects: 新表 message_reactions
-- Estimated duration: < 1s

-- +migrate Up
CREATE TABLE IF NOT EXISTS message_reactions (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    msg_id VARCHAR(36) NOT NULL COMMENT '被反应的消息 ID',
    uid BIGINT NOT NULL COMMENT '反应者 userId',
    emoji VARCHAR(16) NOT NULL COMMENT 'emoji 字符（utf8mb4 4 字节）',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_msg_uid_emoji (msg_id, uid, emoji),
    KEY idx_msg (msg_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='消息表情反应';

-- +migrate Down
DROP TABLE IF EXISTS message_reactions;
