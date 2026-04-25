#!/usr/bin/env python3
"""
ES Sync Worker（Phase 4.4）

工作流程：
1. 增量从 MySQL 拉新消息（按 id 游标）
2. 批量 (bulk) 写 ES /messages 索引
3. 维护 last_indexed_id 游标在 Redis（容器重启不丢进度）

容错：单批失败不更新游标，下批重试；MySQL/Redis/ES 任一异常 sleep 5s 重试。

部署：独立进程，与主服务解耦。需 elasticsearch + pymysql + redis。
"""
import json
import time
import logging
import os
import urllib.request
import urllib.error

import pymysql
import redis

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s [%(levelname)s] %(message)s')

DB_CONF = {
    'host': os.environ.get('MYSQL_HOST', '127.0.0.1'),
    'port': int(os.environ.get('MYSQL_PORT', '3306')),
    'user': os.environ.get('MYSQL_USER', 'root'),
    'password': os.environ.get('MYSQL_PASSWORD', ''),
    'database': os.environ.get('MYSQL_DB', 'muduo_im'),
    'charset': 'utf8mb4',
    'cursorclass': pymysql.cursors.DictCursor,
}
ES_HOST = os.environ.get('ES_HOST', 'http://localhost:9200')
ES_INDEX = os.environ.get('ES_INDEX', 'messages')
REDIS_HOST = os.environ.get('REDIS_HOST', '127.0.0.1')
REDIS_PORT = int(os.environ.get('REDIS_PORT', '6379'))
SYNC_INTERVAL = int(os.environ.get('SYNC_INTERVAL_SEC', '5'))
BATCH_SIZE = int(os.environ.get('BATCH_SIZE', '100'))

CURSOR_KEY = 'es_sync:last_indexed'


def es_bulk(docs):
    if not docs:
        return True
    body = []
    for d in docs:
        body.append(json.dumps({"index": {"_index": ES_INDEX, "_id": d['msg_id']}}))
        body.append(json.dumps(d))
    payload = '\n'.join(body) + '\n'
    req = urllib.request.Request(
        f"{ES_HOST}/_bulk",
        data=payload.encode('utf-8'),
        headers={'Content-Type': 'application/x-ndjson'},
        method='POST'
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            r = json.loads(resp.read().decode('utf-8'))
            if r.get('errors'):
                for item in r.get('items', []):
                    op = item.get('index', {})
                    if op.get('status', 200) >= 300:
                        logging.warning("ES bulk item: %s", op.get('error', {}))
                        break
            return True
    except urllib.error.URLError as e:
        logging.error("ES bulk failed: %s", e)
        return False


def fetch_new(conn, cur_priv, cur_grp):
    docs = []
    new_priv = cur_priv
    new_grp = cur_grp
    with conn.cursor() as cur:
        cur.execute(
            "SELECT id, msg_id, from_user, to_user, content, timestamp "
            "FROM private_messages WHERE id > %s AND recalled=0 "
            "ORDER BY id ASC LIMIT %s",
            (cur_priv, BATCH_SIZE))
        for row in cur.fetchall():
            docs.append({'msg_id': row['msg_id'], 'sender_id': row['from_user'],
                         'recipient_id': row['to_user'], 'body': row['content'],
                         'msg_kind': 'private', 'created_at': row['timestamp']})
            new_priv = max(new_priv, row['id'])
        cur.execute(
            "SELECT id, msg_id, from_user, group_id, content, timestamp "
            "FROM group_messages WHERE id > %s AND recalled=0 "
            "ORDER BY id ASC LIMIT %s",
            (cur_grp, BATCH_SIZE))
        for row in cur.fetchall():
            docs.append({'msg_id': row['msg_id'], 'sender_id': row['from_user'],
                         'group_id': row['group_id'], 'body': row['content'],
                         'msg_kind': 'group', 'created_at': row['timestamp']})
            new_grp = max(new_grp, row['id'])
    return docs, new_priv, new_grp


def main():
    logging.info("ES Sync Worker started | DB=%s ES=%s INDEX=%s",
                 DB_CONF['host'], ES_HOST, ES_INDEX)
    while True:
        try:
            r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
            cur_priv = int(r.hget(CURSOR_KEY, 'private') or 0)
            cur_grp = int(r.hget(CURSOR_KEY, 'group') or 0)

            conn = pymysql.connect(**DB_CONF)
            try:
                docs, new_priv, new_grp = fetch_new(conn, cur_priv, cur_grp)
            finally:
                conn.close()

            if docs and es_bulk(docs):
                r.hset(CURSOR_KEY, mapping={'private': new_priv, 'group': new_grp})
                logging.info("Indexed %d docs (priv:%d→%d grp:%d→%d)",
                             len(docs), cur_priv, new_priv, cur_grp, new_grp)
        except (pymysql.Error, redis.RedisError) as e:
            logging.error("DB error: %s", e)
        except Exception as e:
            logging.exception("Unexpected: %s", e)
        time.sleep(SYNC_INTERVAL)


if __name__ == '__main__':
    main()
