-- 注意：本脚本不创建/切换数据库；调用方需通过命令行指定目标 db，例如：
--   mysql -u root muduo_im < sql/init.sql
-- 这样同一脚本可用于生产 (muduo_im) 和测试 (muduo_im_test) 两套库。

CREATE TABLE IF NOT EXISTS users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(64) UNIQUE NOT NULL,
    password VARCHAR(128) NOT NULL,
    nickname VARCHAR(64),
    avatar VARCHAR(256) DEFAULT '',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friends (
    user_id BIGINT NOT NULL,
    friend_id BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, friend_id),
    INDEX idx_user (user_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS `groups` (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(128) NOT NULL,
    owner_id BIGINT NOT NULL,
    announcement TEXT DEFAULT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_groups_owner (owner_id),
    FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS group_members (
    group_id BIGINT NOT NULL,
    user_id BIGINT NOT NULL,
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, user_id),
    INDEX idx_user_groups (user_id),
    FOREIGN KEY (group_id) REFERENCES `groups`(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS private_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    from_user BIGINT NOT NULL,
    to_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    recalled TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_chat (from_user, to_user, timestamp),
    INDEX idx_inbox (to_user, timestamp)
    -- 消息不加外键：保留历史消息即使用户删除
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS group_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    group_id BIGINT NOT NULL,
    from_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    recalled TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_group_time (group_id, timestamp)
    -- 不加外键：保留历史
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friend_requests (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    from_user BIGINT NOT NULL,
    to_user BIGINT NOT NULL,
    status TINYINT DEFAULT 0 COMMENT '0=pending, 1=accepted, 2=rejected',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_to_user (to_user, status),
    UNIQUE KEY uk_pair (from_user, to_user),
    FOREIGN KEY (from_user) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (to_user) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

-- 审计日志：记录敏感操作（登录/注册/改密/注销/踢人等）
-- 不加外键：用户注销后仍保留审计痕迹
CREATE TABLE IF NOT EXISTS audit_log (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id BIGINT,
    action VARCHAR(64) NOT NULL COMMENT 'login/register/delete_account/change_password/...',
    target VARCHAR(128) COMMENT '操作目标（user_id/group_id/...）',
    ip VARCHAR(64),
    detail TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_user_time (user_id, created_at),
    INDEX idx_action_time (action, created_at)
) ENGINE=InnoDB;

-- Phase 1.2 Transactional Outbox：业务表写入 + outbox 行写入同事务，
-- OutboxRelay 后台线程拉 status='pending' 行 → produce Kafka → 改 'sent'。
-- 解决"DB 写成功但 Kafka 发失败"或反之的双写不一致。
CREATE TABLE IF NOT EXISTS outbox (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    topic VARCHAR(64) NOT NULL COMMENT 'Kafka topic，例 im.messages',
    msg_key VARCHAR(64) NOT NULL COMMENT 'Kafka partition key（conv_id / group_id 等）',
    payload MEDIUMTEXT NOT NULL COMMENT '已序列化业务消息 JSON',
    status ENUM('pending','sending','sent','failed') NOT NULL DEFAULT 'pending',
    retry_count INT NOT NULL DEFAULT 0,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    sent_at DATETIME(3) NULL,
    last_error VARCHAR(512) NULL,
    INDEX idx_status_created (status, created_at)
) ENGINE=InnoDB;

-- Phase 6.1 Saga 编排器：多步业务流的状态持久化
-- forward 步骤跟 saga_log 行 UPDATE 同事务写，保证"业务表 + saga_log"原子。
-- 进程崩溃 → 重启 SagaCoordinator::recoverIncomplete() 扫 state IN
-- ('running','compensating') 续跑或反向补偿。
CREATE TABLE IF NOT EXISTS saga_log (
    saga_id BIGINT PRIMARY KEY COMMENT 'Snowflake，全局唯一',
    saga_type VARCHAR(64) NOT NULL COMMENT '注册的 saga 名，如 group_create',
    state ENUM('running','compensating','done','failed') NOT NULL DEFAULT 'running',
    current_step INT NOT NULL DEFAULT 0 COMMENT 'forward / compensate 当前进行到第几步',
    payload MEDIUMTEXT NOT NULL COMMENT 'saga 上下文 JSON，跨步骤传递',
    error_msg TEXT NULL,
    created_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    INDEX idx_state_updated (state, updated_at)
) ENGINE=InnoDB;
