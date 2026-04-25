#!/usr/bin/env python3
"""
APNs/FCM 推送 Worker（Phase 5.1）

当前实现：**mock 模式** —— 仅打印日志 + 标记成功，不真实调用第三方 API。
真实接入步骤：
1. APNs：HTTP/2 + p8 JWT，配置 APNS_P8_PATH / APNS_KEY_ID / APNS_TEAM_ID / APNS_TOPIC
2. FCM ：REST + service account JSON，配置 FCM_PROJECT_ID / FCM_SA_JSON

任务格式见 PushService.h 注释。

部署：独立容器，与 thumb_worker / es_sync_worker 同样模式。
"""
import json
import logging
import os
import time

import redis

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s [%(levelname)s] %(message)s')

QUEUE = os.environ.get('PUSH_QUEUE', 'push_queue')
DRY_RUN = os.environ.get('DRY_RUN', 'true').lower() == 'true'  # 默认 mock


def send_apns(token: str, title: str, body: str, data: dict) -> bool:
    """真实 APNs 实现略：用 hyper-h2 / aioapns / pyapns 库 → HTTP/2 POST"""
    if DRY_RUN:
        logging.info("[APNs MOCK] token=%s... %s: %s", token[:16], title, body)
        return True
    # ... 真实实现
    return True


def send_fcm(token: str, title: str, body: str, data: dict) -> bool:
    """真实 FCM 实现略：用 google-auth + requests → REST POST"""
    if DRY_RUN:
        logging.info("[FCM MOCK] token=%s... %s: %s", token[:16], title, body)
        return True
    return True


def process(task: dict) -> int:
    title = task.get('title', '')
    body  = task.get('body', '')
    data  = task.get('data', {})
    apns  = task.get('apns_token', '')
    fcm   = task.get('fcm_token', '')
    sent = 0
    if apns:
        if send_apns(apns, title, body, data): sent += 1
    if fcm:
        if send_fcm(fcm, title, body, data): sent += 1
    return sent


def main():
    rh = os.environ.get('REDIS_HOST', '127.0.0.1')
    rp = int(os.environ.get('REDIS_PORT', '6379'))
    logging.info("Push worker started | redis=%s:%d queue=%s DRY_RUN=%s",
                 rh, rp, QUEUE, DRY_RUN)
    r = redis.Redis(host=rh, port=rp, decode_responses=True)
    while True:
        try:
            res = r.brpop(QUEUE, timeout=5)
            if not res: continue
            _, raw = res
            try:
                task = json.loads(raw)
            except json.JSONDecodeError:
                logging.warning("Bad JSON: %s", raw[:200]); continue
            sent = process(task)
            logging.info("✓ uid=%s device=%s sent=%d",
                         task.get('uid'), task.get('device_id'), sent)
        except redis.RedisError as e:
            logging.error("Redis error: %s; reconnect in 5s", e); time.sleep(5)
            r = redis.Redis(host=rh, port=rp, decode_responses=True)
        except KeyboardInterrupt:
            break
        except Exception as e:
            logging.exception("Unexpected: %s", e)


if __name__ == '__main__':
    main()
