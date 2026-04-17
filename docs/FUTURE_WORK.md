# 未来工作路线图

本文档记录 muduo-im 作为 IM 系统距离真正大规模生产部署的完整清单。
当前项目已具备中小规模生产能力（ACID、CircuitBreaker、/health、CI、Docker），
但面向百万级用户/多地部署需要以下扩展。

## 已完成（参考）

- 安全：Argon2id 密码哈希、登录失败限流、JWT 环境变量、审计日志
- 可靠性：事务 + 外键 CASCADE、Redis AOF、CircuitBreaker、优雅关闭
- 可观测性：/health 探针、Prometheus /metrics、结构化 JSON 日志
- 质量：18 项单元测试、23 项 E2E、GitHub Actions CI、Docker 镜像

## 待做

### Tier 3 — 规模扩展

#### 跨实例消息路由
**问题**：多实例部署时，user A 连 instance-1，user B 连 instance-2。
A 给 B 发消息时，instance-1 的 OnlineManager 查不到 B，消息丢失。

**方案**：Redis Pub/Sub 做实例间消息总线
```
instance-1 收到 msg to B →
  查 Redis 得知 B 在 instance-2 →
  Redis PUBLISH "msg:instance-2" {payload} →
  instance-2 订阅收到 → 本地 session 转发
```

**工作量**：~200 行，需要 Redis Cluster + 多实例部署环境验证。

#### 数据库读写分离
**问题**：所有流量打主库，写放大 10x（消息表），读放大更严重。

**方案**：
- 主从复制（MySQL 内置）
- MySQLPool 区分 masterPool_ + slavePool_
- 读操作走 slave，写操作走 master

**工作量**：~100 行代码 + 主从配置。

#### Redis Cluster
**问题**：单 Redis 容量/QPS 有限（几十万连接的 online:{uid} 键）。

**方案**：Redis Cluster 分片（按 userId hash slot）。

**工作量**：hiredis 改用 hiredis-cluster，~50 行。

### Tier 4 — 业务完善

#### 端到端加密（E2EE）
**问题**：服务端能看到所有消息内容。

**方案**：Signal Protocol（X3DH + Double Ratchet）。
- 客户端存私钥，服务端只存公钥
- 消息加密后再发往服务端
- 撤回/搜索功能会失效（密文无法在服务端搜索）

**工作量**：需要完整实现 Signal Protocol 客户端和密钥协商流程，~数千行。

#### 离线推送（APNs/FCM）
**问题**：用户关闭 app 后收不到消息通知。

**方案**：
- 离线时将消息转发到 APNs（iOS）或 FCM（Android）
- 需要 Apple Developer / Firebase 账号
- 需要客户端注册 device token

**工作量**：~300 行代码 + 外部服务账号。

#### 语音/视频通话
**问题**：仅支持文本。

**方案**：WebRTC + 信令服务器
- muduo-im 做信令中转（offer/answer/ICE）
- STUN/TURN 服务器（可用 coturn）

**工作量**：信令 ~500 行 + WebRTC 前端集成。

#### GDPR 合规
- 数据导出（用户请求所有个人数据的 ZIP）
- 彻底删除（跨表清理 + 日志脱敏）
- Cookie 同意
- 隐私政策页面

**工作量**：合规工作，非纯代码。

### 运维增强

#### 分布式追踪（OpenTelemetry）
- 每个 HTTP 请求生成 trace_id
- 在 log、metrics、消息转发中传播
- 集成 Jaeger / Tempo 做可视化

**工作量**：~150 行 + 部署 Jaeger。

#### 配置热更新
**问题**：改 config.ini 要重启服务。

**方案**：
- inotify 监听 config.ini 变化
- 某些配置（如日志级别、限流阈值）运行时生效

**工作量**：~80 行。

#### 自动化压力测试
- 每个 release 前自动跑 wrk + ws_bench.cpp
- 性能回归告警

**工作量**：~100 行脚本。

## 不在路线图

以下项目作者认为 **超出 IM 核心范畴**，短期不会做：

- 文件秒传（需要 CDN + 去重存储）
- 图床（需要云存储）
- 机器人 API（Webhook/SDK）
- 多租户（SaaS 场景）
