CREATE DATABASE IF NOT EXISTS muduo_im DEFAULT CHARACTER SET utf8mb4;
USE muduo_im;

CREATE TABLE IF NOT EXISTS users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(64) UNIQUE NOT NULL,
    password VARCHAR(128) NOT NULL,
    nickname VARCHAR(64),
    avatar VARCHAR(256) DEFAULT '',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS friends (
    user_id BIGINT NOT NULL,
    friend_id BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, friend_id),
    INDEX idx_user (user_id)
);

CREATE TABLE IF NOT EXISTS `groups` (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(128) NOT NULL,
    owner_id BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS group_members (
    group_id BIGINT NOT NULL,
    user_id BIGINT NOT NULL,
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, user_id),
    INDEX idx_user_groups (user_id)
);

CREATE TABLE IF NOT EXISTS private_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    from_user BIGINT NOT NULL,
    to_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_chat (from_user, to_user, timestamp),
    INDEX idx_inbox (to_user, timestamp)
);

CREATE TABLE IF NOT EXISTS group_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    group_id BIGINT NOT NULL,
    from_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_group_time (group_id, timestamp)
);
