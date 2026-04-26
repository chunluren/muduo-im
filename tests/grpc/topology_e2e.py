#!/usr/bin/env python3
"""Phase 1.2 W3.D3-D4：多实例 e2e + 简单延迟统计。

模型：
  - N 对用户（sender_i, receiver_i），sender 连 gateway-A:9091，receiver 连 gateway-B:9092
  - 每对发 M 条消息，记录 ack 延迟 & receiver 到达延迟
  - 跨 gateway 投递依赖 logic 的 RegisterGateway 反向流（receiver 在 gw-B，sender 走
    任一 logic，logic 通过其与 gw-B 的 stream 把消息推过去）
"""
from __future__ import annotations
import json, os, statistics, sys, threading, time, uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from websocket import create_connection

GW_A = os.environ.get("GW_A", "ws://127.0.0.1:9091/ws")
GW_B = os.environ.get("GW_B", "ws://127.0.0.1:9092/ws")
N_PAIRS = int(os.environ.get("N_PAIRS", "20"))
M_MSGS  = int(os.environ.get("M_MSGS", "10"))
TIMEOUT = int(os.environ.get("TIMEOUT", "5"))

ack_lat_ms  = []
recv_lat_ms = []
errors      = []
lock = threading.Lock()


def run_pair(idx: int):
    sender_uid   = 10000 + idx        # gw-A
    receiver_uid = 20000 + idx        # gw-B
    try:
        ws_r = create_connection(f"{GW_B}?uid={receiver_uid}&device=R", timeout=TIMEOUT)
        ws_r.settimeout(TIMEOUT)
        ws_s = create_connection(f"{GW_A}?uid={sender_uid}&device=S", timeout=TIMEOUT)
        ws_s.settimeout(TIMEOUT)
        # 让 logic 的 conn_open 都到位
        time.sleep(0.05)

        local_ack = []
        local_recv = []
        for k in range(M_MSGS):
            cmid = f"{idx}-{k}-{uuid.uuid4().hex[:6]}"
            send_ts = time.time()
            ws_s.send(json.dumps({
                "type": "msg", "to": str(receiver_uid),
                "content": f"hi {idx}-{k}", "clientMsgId": cmid
            }))
            ack = json.loads(ws_s.recv())
            ack_ts = time.time()
            assert ack.get("type") == "ack" and ack.get("clientMsgId") == cmid, ack
            recv = json.loads(ws_r.recv())
            recv_ts = time.time()
            assert recv.get("type") == "msg" and recv.get("from") == str(sender_uid), recv
            local_ack.append((ack_ts - send_ts) * 1000.0)
            local_recv.append((recv_ts - send_ts) * 1000.0)

        with lock:
            ack_lat_ms.extend(local_ack)
            recv_lat_ms.extend(local_recv)

        ws_s.close(); ws_r.close()
    except Exception as e:
        with lock:
            errors.append(f"pair {idx}: {e!r}")


def main():
    print(f"[e2e] N_PAIRS={N_PAIRS} M_MSGS={M_MSGS} (total={N_PAIRS*M_MSGS} messages)")
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=N_PAIRS) as pool:
        list(pool.map(run_pair, range(N_PAIRS)))
    dt = time.time() - t0

    if errors:
        print(f"[e2e] FAILED ({len(errors)} errors):")
        for e in errors[:10]:
            print("  -", e)
        return 1

    n = len(ack_lat_ms)
    print(f"[e2e] OK: {n} messages in {dt:.2f}s ({n/dt:.0f} msg/s aggregate)")

    def pct(arr, p):
        return statistics.quantiles(arr, n=100, method="inclusive")[p-1]
    def stats(name, arr):
        arr = sorted(arr)
        print(f"[e2e] {name}: n={len(arr)} avg={statistics.mean(arr):.2f}ms "
              f"p50={pct(arr,50):.2f} p95={pct(arr,95):.2f} p99={pct(arr,99):.2f} "
              f"max={arr[-1]:.2f}")
    stats("ack-latency  (ws→gateway→logic→ack→ws)", ack_lat_ms)
    stats("recv-latency (ws→gateway→logic→stream→other-gw→ws)", recv_lat_ms)
    return 0


if __name__ == "__main__":
    sys.exit(main())
