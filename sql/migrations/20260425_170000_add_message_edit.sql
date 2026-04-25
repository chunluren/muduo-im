-- Feature: 消息编辑（Phase 4.1）
-- Author: chunluren
-- Ticket: #12
-- Affects: private_messages / group_messages 加 edited_at + original_body 字段；新表 message_edits
-- Estimated duration: < 5s（在线表 0 行）

-- +migrate Up
ALTER TABLE private_messages
    ADD COLUMN edited_at BIGINT NULL COMMENT '最后编辑时间戳（毫秒）',
    ADD COLUMN original_body TEXT NULL COMMENT '首次编辑时保存的原始内容';

ALTER TABLE group_messages
    ADD COLUMN edited_at BIGINT NULL COMMENT '最后编辑时间戳（毫秒）',
    ADD COLUMN original_body TEXT NULL COMMENT '首次编辑时保存的原始内容';

-- 编辑历史审计表（每次编辑一条记录，按 msg_id + edited_at 排序得到完整时间线）
CREATE TABLE IF NOT EXISTS message_edits (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    msg_id VARCHAR(36) NOT NULL COMMENT '被编辑的消息 ID',
    editor_id BIGINT NOT NULL COMMENT '编辑者 userId（必须是消息发送者）',
    msg_kind TINYINT NOT NULL COMMENT '消息类型：0=私聊 1=群聊',
    old_body TEXT COMMENT '编辑前内容',
    new_body TEXT COMMENT '编辑后内容',
    edited_at BIGINT NOT NULL COMMENT '编辑时间戳（毫秒）',
    KEY idx_msg (msg_id, edited_at),
    KEY idx_editor (editor_id, edited_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='消息编辑历史审计';

-- +migrate Down
DROP TABLE IF EXISTS message_edits;
ALTER TABLE group_messages DROP COLUMN original_body, DROP COLUMN edited_at;
ALTER TABLE private_messages DROP COLUMN original_body, DROP COLUMN edited_at;
