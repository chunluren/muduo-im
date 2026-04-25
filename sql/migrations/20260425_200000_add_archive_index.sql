-- Feature: 冷数据归档索引（Phase 5.3）
-- Author: chunluren
-- Ticket: #19
-- Affects: 新表 archive_index

-- +migrate Up
CREATE TABLE IF NOT EXISTS archive_index (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    msg_kind ENUM('private','group') NOT NULL,
    partition_key VARCHAR(20) NOT NULL COMMENT '分区键 (年-月)',
    storage_uri VARCHAR(512) NOT NULL COMMENT '归档文件 URI',
    msg_count INT NOT NULL DEFAULT 0,
    min_msg_id VARCHAR(36),
    max_msg_id VARCHAR(36),
    min_ts BIGINT,
    max_ts BIGINT,
    archived_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    status ENUM('archiving','verified','active','deleted_source') NOT NULL DEFAULT 'archiving',
    UNIQUE KEY uk_kind_partition (msg_kind, partition_key),
    KEY idx_partition (partition_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='冷数据归档索引';

-- +migrate Down
DROP TABLE IF EXISTS archive_index;
