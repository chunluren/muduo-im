# Redis 持久化配置

## 为什么需要 AOF

muduo-im 使用 Redis 作为消息队列缓冲（`msg_queue`），每 2 秒批量刷到 MySQL。
若 Redis 崩溃且只用默认 RDB 持久化，最多可能丢失 **最近 15 分钟的消息**。

启用 AOF 可将丢失窗口缩短到 **最多 1 秒**。

## 配置方法

### 方法 1：修改 `/etc/redis/redis.conf`

```conf
appendonly yes
appendfsync everysec
appendfilename "appendonly.aof"
dir /var/lib/redis

# 自动重写（避免 AOF 文件过大）
auto-aof-rewrite-percentage 100
auto-aof-rewrite-min-size 64mb
```

重启 Redis：
```bash
sudo systemctl restart redis-server
redis-cli config get appendonly  # 确认: 1) "appendonly"  2) "yes"
```

### 方法 2：运行时启用（无需重启）

```bash
redis-cli CONFIG SET appendonly yes
redis-cli CONFIG SET appendfsync everysec
redis-cli CONFIG REWRITE  # 持久化配置到磁盘
```

## 验证

```bash
ls -la /var/lib/redis/appendonly.aof  # 应该存在且持续增长
redis-cli LPUSH test_key "hello"
cat /var/lib/redis/appendonly.aof | tail -5  # 应能看到 LPUSH 命令
```

## fsync 策略选择

| 策略 | 丢失窗口 | 性能影响 | 适用场景 |
|------|---------|---------|---------|
| `always` | 0（每条写都 fsync） | 严重（QPS 降 10x） | 金融类 |
| `everysec` | 1 秒 | 轻微 | **muduo-im 推荐** |
| `no` | OS 控制（~30s） | 无 | 非关键数据 |

## 恢复流程

1. Redis 崩溃重启时自动重放 AOF
2. 重放过程中 muduo-im 的写入会失败，`CircuitBreaker` 熔断保护
3. Redis 恢复后 muduo-im 的 CircuitBreaker 自动进入 HalfOpen 探测

## 监控

建议监控以下 Redis 指标：
- `aof_current_size` — AOF 文件大小
- `aof_pending_rewrite` — 是否有 pending 重写
- `rdb_last_bgsave_status` — 最后一次 RDB 快照状态
