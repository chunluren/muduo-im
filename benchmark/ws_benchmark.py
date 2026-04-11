#!/usr/bin/env python3
"""muduo-im WebSocket 压力测试"""
import asyncio
import json
import time
import sys
import socket
import hashlib
import base64
import os
import struct

# Config
HOST = "127.0.0.1"
HTTP_PORT = 8080
WS_PORT = 9090
NUM_CLIENTS = int(sys.argv[1]) if len(sys.argv) > 1 else 10
MESSAGES_PER_CLIENT = int(sys.argv[2]) if len(sys.argv) > 2 else 100
API = f"http://{HOST}:{HTTP_PORT}"

stats = {"connected": 0, "sent": 0, "received": 0, "errors": 0, "latencies": []}

def http_post(path, data, token=None):
    """Simple HTTP POST using raw sockets"""
    body = json.dumps(data)
    headers = f"POST {path} HTTP/1.1\r\nHost: {HOST}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: close\r\n"
    if token:
        headers += f"Authorization: Bearer {token}\r\n"
    headers += "\r\n"

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((HOST, HTTP_PORT))
    s.sendall((headers + body).encode())

    resp = b""
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk: break
            resp += chunk
        except: break
    s.close()

    # Extract body after \r\n\r\n
    parts = resp.split(b"\r\n\r\n", 1)
    if len(parts) > 1:
        return json.loads(parts[1])
    return {}

def register_and_login(username, password):
    """Register user and login, return token + userId"""
    http_post("/api/register", {"username": username, "password": password, "nickname": username})
    result = http_post("/api/login", {"username": username, "password": password})
    return result.get("token", ""), result.get("userId", 0)

async def ws_client(client_id, token, target_id):
    """Single WebSocket client coroutine using raw TCP"""
    try:
        reader, writer = await asyncio.open_connection(HOST, WS_PORT)

        # WebSocket handshake
        key = base64.b64encode(os.urandom(16)).decode()
        handshake = (
            f"GET /ws?token={token} HTTP/1.1\r\n"
            f"Host: {HOST}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        )
        writer.write(handshake.encode())
        await writer.drain()

        # Read handshake response
        response = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=5)
        if b"101" not in response:
            stats["errors"] += 1
            return

        stats["connected"] += 1

        # Send messages
        for i in range(MESSAGES_PER_CLIENT):
            msg = json.dumps({
                "type": "msg",
                "to": str(target_id),
                "content": f"bench-{client_id}-{i}",
                "msgId": f"{client_id}-{i}-{time.time()}"
            })

            # Encode as WebSocket frame (masked)
            payload = msg.encode()
            mask_key = os.urandom(4)
            masked = bytes(b ^ mask_key[j % 4] for j, b in enumerate(payload))

            frame = bytearray()
            frame.append(0x81)  # FIN + TEXT
            length = len(payload)
            if length <= 125:
                frame.append(0x80 | length)
            elif length <= 65535:
                frame.append(0x80 | 126)
                frame.extend(struct.pack(">H", length))
            frame.extend(mask_key)
            frame.extend(masked)

            t0 = time.time()
            writer.write(bytes(frame))
            await writer.drain()
            stats["sent"] += 1

            # Try to read ACK (non-blocking, with short timeout)
            try:
                data = await asyncio.wait_for(reader.read(4096), timeout=1)
                if data:
                    stats["received"] += 1
                    latency = (time.time() - t0) * 1000
                    stats["latencies"].append(latency)
            except asyncio.TimeoutError:
                pass

        # Close
        writer.close()

    except Exception as e:
        stats["errors"] += 1

async def main():
    print(f"=== muduo-im WebSocket 压力测试 ===")
    print(f"客户端数: {NUM_CLIENTS}")
    print(f"每客户端消息数: {MESSAGES_PER_CLIENT}")
    print()

    # Register test users
    print("注册测试用户...")
    tokens = []
    user_ids = []
    for i in range(NUM_CLIENTS):
        token, uid = register_and_login(f"bench_user_{i}_{int(time.time())}", "benchpass")
        tokens.append(token)
        user_ids.append(uid)

    if not all(tokens):
        print("ERROR: 部分用户注册/登录失败")
        return

    # Add all as friends of user 0
    for i in range(1, NUM_CLIENTS):
        http_post("/api/friends/add", {"friendId": user_ids[i]}, tokens[0])

    print(f"已注册 {NUM_CLIENTS} 个用户")
    print("开始压测...")

    t0 = time.time()

    # Launch all clients
    tasks = []
    for i in range(NUM_CLIENTS):
        target = user_ids[(i + 1) % NUM_CLIENTS]
        tasks.append(ws_client(i, tokens[i], target))

    await asyncio.gather(*tasks)

    elapsed = time.time() - t0

    print()
    print(f"======== 结果 ========")
    print(f"连接成功: {stats['connected']}/{NUM_CLIENTS}")
    print(f"消息发送: {stats['sent']}")
    print(f"ACK 收到: {stats['received']}")
    print(f"错误: {stats['errors']}")
    print(f"耗时: {elapsed:.2f}s")
    print(f"发送 QPS: {stats['sent']/elapsed:.1f}")
    if stats['latencies']:
        lats = sorted(stats['latencies'])
        print(f"延迟 Avg: {sum(lats)/len(lats):.2f}ms")
        print(f"延迟 P50: {lats[len(lats)//2]:.2f}ms")
        print(f"延迟 P99: {lats[int(len(lats)*0.99)]:.2f}ms")

if __name__ == "__main__":
    asyncio.run(main())
