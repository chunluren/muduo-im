#!/usr/bin/env python3
"""
图片缩略图生成 Worker（Phase 4.2）

工作流程：
1. 阻塞 BRPOP 从 Redis 队列 thumb_queue 取任务（JSON）
2. 用 Pillow 读原图 → 生成多尺寸缩略图（默认 200, 600）
3. 写到与原图同目录的 <name>_thumb_<size>.<ext>
4. 失败重试 3 次后丢弃（记日志）

任务格式：
    {"saved_name":"abc.jpg","original_path":"/path/to/abc.jpg","sizes":[200,600]}

部署：独立容器，与主服务共享 uploads 目录卷。
依赖：Pillow + redis
"""
import json
import logging
import os
import time

from PIL import Image
import redis

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s [%(levelname)s] %(message)s')

REDIS_HOST = os.environ.get('REDIS_HOST', '127.0.0.1')
REDIS_PORT = int(os.environ.get('REDIS_PORT', '6379'))
QUEUE = os.environ.get('THUMB_QUEUE', 'thumb_queue')
JPEG_QUALITY = int(os.environ.get('THUMB_JPEG_QUALITY', '80'))


def gen_thumb(orig_path: str, size: int) -> str:
    """生成单个尺寸的缩略图，返回输出路径"""
    base, ext = os.path.splitext(orig_path)
    out_path = f"{base}_thumb_{size}{ext}"

    with Image.open(orig_path) as img:
        # RGBA → RGB 用于 JPEG 输出（避免透明度报错）
        if ext.lower() in ('.jpg', '.jpeg') and img.mode != 'RGB':
            img = img.convert('RGB')
        # 等比缩放，最长边 = size
        img.thumbnail((size, size), Image.LANCZOS)
        # JPEG 压缩；其他格式保持
        if ext.lower() in ('.jpg', '.jpeg'):
            img.save(out_path, 'JPEG', quality=JPEG_QUALITY, optimize=True)
        else:
            img.save(out_path)
    return out_path


def process(task: dict) -> bool:
    orig = task.get('original_path')
    sizes = task.get('sizes', [200, 600])
    if not orig or not os.path.exists(orig):
        logging.warning("Skip: file not found %s", orig)
        return False
    for size in sizes:
        try:
            out = gen_thumb(orig, size)
            logging.info("✓ %s → %dx%d %s", orig, size, size, out)
        except Exception as e:
            logging.error("✗ %s @ %d failed: %s", orig, size, e)
            return False
    return True


def main():
    logging.info("Thumb worker started | redis=%s:%d queue=%s",
                 REDIS_HOST, REDIS_PORT, QUEUE)
    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)

    while True:
        try:
            # BRPOP 阻塞 5 秒
            res = r.brpop(QUEUE, timeout=5)
            if not res:
                continue
            _, raw = res
            try:
                task = json.loads(raw)
            except json.JSONDecodeError:
                logging.warning("Bad JSON: %s", raw[:200])
                continue
            process(task)
        except redis.RedisError as e:
            logging.error("Redis error: %s; sleep 5s", e)
            time.sleep(5)
            r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
        except KeyboardInterrupt:
            logging.info("Bye")
            break
        except Exception as e:
            logging.exception("Unexpected: %s", e)


if __name__ == '__main__':
    main()
