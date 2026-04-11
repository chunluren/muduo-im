# Phase 4: muduo-im 前端 + 联调 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 构建单文件 Web 前端（登录/注册 + 聊天界面），集成到 ChatServer 静态文件服务，完成端到端联调。

**Architecture:** 纯 HTML + CSS + JS 单文件 `index.html`，使用原生 `fetch` 调 REST API，原生 `WebSocket` 做实时通信。ChatServer 用 `httpServer_.serveStatic("/", "web/")` serve 前端。无框架无构建工具。

**Tech Stack:** HTML5, CSS3, Vanilla JS, WebSocket API, fetch API, localStorage

---

## Task 1: 前端 index.html

**Files:**
- Create: `web/index.html`

### 完整实现

单文件包含三个页面状态：登录/注册、聊天主界面。

功能清单：
- 登录/注册表单（切换）
- 好友列表 + 群组列表（侧边栏）
- 添加好友/创建群/加入群（弹窗或内联表单）
- 聊天消息窗口 + 输入框
- WebSocket 实时消息接收
- 在线/离线状态显示
- 历史消息加载
- JWT 存储在 localStorage

关键 JS 逻辑：
- `API_BASE = window.location.origin`（HTTP API 同源）
- `WS_URL = 'ws://' + window.location.hostname + ':9090/ws?token=' + token`
- `fetch(url, { headers: { 'Authorization': 'Bearer ' + token } })`
- `new WebSocket(WS_URL)` → onopen/onmessage/onclose
- 消息类型分发: msg, group_msg, ack, online, offline, error

### Commit

```bash
git add web/index.html
git commit -m "feat: add web frontend (login/chat/friends/groups)"
```

---

## Task 2: ChatServer 集成静态文件

**Files:**
- Modify: `src/server/ChatServer.h`

在 `setupHttpRoutes()` 最后加一行:

```cpp
httpServer_.serveStatic("/", "../web");
```

这样访问 `http://localhost:8080/index.html` 即可打开前端。

**注意:** 路径是相对于运行目录的。如果从 `build/` 运行则 `../web` 指向正确位置。

### Commit

```bash
git add src/server/ChatServer.h
git commit -m "feat: serve web frontend via HttpServer static files"
```

---

## Task 3: 最终编译 + 文档更新

### Step 1: 编译

```bash
cd build && cmake .. && make muduo-im -j$(nproc)
```

### Step 2: 更新 CLAUDE.md

将阶段 4 标记完成。

### Step 3: Commit

```bash
git add -A
git commit -m "docs: mark Phase 4 as completed"
```

---

## 联调指南（手动测试）

### 前提
1. MySQL 运行中，执行 `mysql -u root < sql/init.sql`
2. Redis 运行中（默认端口 6379）

### 启动
```bash
cd build && ./muduo-im
# HTTP: http://localhost:8080
# WS: ws://localhost:9090
```

### 测试流程
1. 浏览器访问 `http://localhost:8080/index.html`
2. 注册两个用户 (user1, user2)
3. 登录 user1 → 添加 user2 为好友
4. 打开第二个浏览器标签 → 登录 user2
5. user1 发消息给 user2 → user2 实时收到
6. 创建群组 → user2 加入 → 群聊消息广播
7. 查看历史消息

---

## 总结

| Task | 文件 | 描述 |
|------|------|------|
| 1 | `web/index.html` | 完整前端（登录+聊天+好友+群组） |
| 2 | `ChatServer.h` | 静态文件服务集成 |
| 3 | 编译 + 文档 | 最终验证 |
