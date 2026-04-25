#!/usr/bin/env python3
"""
归档查询服务器（Phase 5.3）

提供 HTTP API 让主服务查归档消息：
    GET /query?kind=private&conv_id=A_B&before_ts=N&limit=50
    GET /query?kind=group&conv_id=GROUP_ID&before_ts=N&limit=50

工作机制：
- 启动时从 archive_index 读已归档分区
- 按需加载 Parquet（缓存最近 N 个 partition）
- 在内存里过滤 + 排序

在生产可换 Athena / Trino / Spark SQL 直接查 S3 上的 Parquet（更可扩展）；
本实现适合开发 / 中小规模归档查询。
"""
import json
import logging
import os
import sys
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

import pymysql
import pyarrow.parquet as pq

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s [%(levelname)s] %(message)s')

DB = dict(host=os.environ.get('MYSQL_HOST', '127.0.0.1'),
          port=int(os.environ.get('MYSQL_PORT', '3306')),
          user=os.environ.get('MYSQL_USER', 'root'),
          password=os.environ.get('MYSQL_PASSWORD', ''),
          database=os.environ.get('MYSQL_DB', 'muduo_im'),
          charset='utf8mb4',
          cursorclass=pymysql.cursors.DictCursor)
PORT = int(os.environ.get('ARCHIVE_QUERY_PORT', '9300'))
CACHE_LIMIT = 8  # 缓存最近 8 个 partition

# LRU partition cache: {(kind, partition): [rows]}
_cache = OrderedDict()


def load_partition(kind: str, partition: str):
    """加载 Parquet（带 LRU 缓存）"""
    key = (kind, partition)
    if key in _cache:
        _cache.move_to_end(key)
        return _cache[key]

    # 查 archive_index
    conn = pymysql.connect(**DB)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT storage_uri FROM archive_index "
                "WHERE msg_kind=%s AND partition_key=%s "
                "AND status IN ('verified', 'deleted_source')",
                (kind, partition))
            row = cur.fetchone()
    finally:
        conn.close()
    if not row:
        return None
    uri = row['storage_uri']
    if not uri.startswith('file://'):
        # 简化：暂只支持 file:// (生产可加 s3:// 处理)
        logging.warning("Unsupported storage_uri: %s", uri)
        return None
    path = uri[len('file://'):]
    if not os.path.exists(path):
        logging.warning("Parquet not found: %s", path)
        return None
    table = pq.read_table(path)
    rows = table.to_pylist()
    if len(_cache) >= CACHE_LIMIT:
        _cache.popitem(last=False)
    _cache[key] = rows
    logging.info("Loaded partition %s/%s: %d rows", kind, partition, len(rows))
    return rows


def find_partitions(kind: str, before_ts: int):
    """查 archive_index 找 max_ts < before_ts 的 partition，倒序返回"""
    conn = pymysql.connect(**DB)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT partition_key FROM archive_index "
                "WHERE msg_kind=%s AND max_ts < %s "
                "AND status IN ('verified','deleted_source') "
                "ORDER BY max_ts DESC",
                (kind, before_ts))
            return [r['partition_key'] for r in cur.fetchall()]
    finally:
        conn.close()


def query_history(kind: str, user_id: int, peer_id: int, group_id: int,
                  before_ts: int, limit: int):
    """查归档历史。用户视角：私聊查 (user, peer)；群聊查 group_id"""
    parts = find_partitions(kind, before_ts)
    results = []
    for partition in parts:
        rows = load_partition(kind, partition)
        if not rows:
            continue
        # 倒序遍历（按 timestamp）
        rows = sorted(rows, key=lambda r: r['timestamp'], reverse=True)
        for r in rows:
            if r['timestamp'] >= before_ts:
                continue
            if r.get('recalled'):
                continue
            if kind == 'private':
                # A→B 或 B→A
                if not ((r['from_user'] == user_id and r['to_user'] == peer_id) or
                        (r['from_user'] == peer_id and r['to_user'] == user_id)):
                    continue
                out = {
                    'msgId': r['msg_id'],
                    'from': r['from_user'],
                    'to': r['to_user'],
                    'content': r.get('content'),
                    'timestamp': r['timestamp']
                }
            else:
                if r['group_id'] != group_id:
                    continue
                out = {
                    'msgId': r['msg_id'],
                    'from': r['from_user'],
                    'groupId': r['group_id'],
                    'content': r.get('content'),
                    'timestamp': r['timestamp']
                }
            results.append(out)
            if len(results) >= limit:
                return results
    return results


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # 静默 access log（用 logging）
        logging.info("%s %s", self.command, self.path)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path != '/query':
            self.send_error(404, "use /query")
            return
        qs = parse_qs(u.query)
        kind = qs.get('kind', [''])[0]
        if kind not in ('private', 'group'):
            self.send_error(400, "kind must be private/group")
            return
        try:
            limit = int(qs.get('limit', ['50'])[0])
            before_ts = int(qs.get('before_ts', ['9999999999999'])[0])
            if kind == 'private':
                user_id = int(qs.get('user_id', ['0'])[0])
                peer_id = int(qs.get('peer_id', ['0'])[0])
                group_id = 0
            else:
                user_id = peer_id = 0
                group_id = int(qs.get('group_id', ['0'])[0])
        except (ValueError, KeyError) as e:
            self.send_error(400, f"bad params: {e}")
            return

        try:
            results = query_history(kind, user_id, peer_id, group_id, before_ts, limit)
        except Exception as e:
            logging.exception("query failed: %s", e)
            self.send_error(500, "query error")
            return

        body = json.dumps({'success': True, 'count': len(results),
                           'messages': results}).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    server = HTTPServer(('127.0.0.1', PORT), Handler)
    logging.info("Archive query server listening on :%d", PORT)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
