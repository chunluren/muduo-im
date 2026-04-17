CREATE DATABASE IF NOT EXISTS muduo_im DEFAULT CHARACTER SET utf8mb4;
USE muduo_im;

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
