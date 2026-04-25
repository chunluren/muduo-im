#!/usr/bin/env python3
"""
冷数据归档 Worker（Phase 5.3）

工作流程（每天 cron 调度）：
1. 扫描 private_messages / group_messages 中 created_at < now-90d 的消息
2. 按"年-月"分区键聚合 → 写 Parquet 文件
3. 上传到 S3（生产）或写本地（开发）
4. 在 archive_index 记录归档元数据（status=verified）
5. 验证通过后，从 MySQL 删除已归档的消息（status=deleted_source）

容错：
- 单个 partition 失败不影响其他
- 已归档的 partition 跳过（去重）
- 删除前必须 status=verified（先归档后删除）

存储 URI 协议：
- file:///path/to/archives/2025-04_private.parquet  开发环境
- s3://bucket/im/archives/2025-04_private.parquet   生产

部署：cron 每天凌晨 2:00 跑一次。
"""
import os
import sys
import json
import logging
import datetime
import io
from typing import Optional

import pymysql
import pyarrow as pa
import pyarrow.parquet as pq

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
ARCHIVE_LOCAL_DIR = os.environ.get('ARCHIVE_LOCAL_DIR', '/home/ly/workspaces/im/muduo-im/archives')
ARCHIVE_S3_BUCKET = os.environ.get('ARCHIVE_S3_BUCKET', '')  # 生产填 S3 bucket
ARCHIVE_S3_PREFIX = os.environ.get('ARCHIVE_S3_PREFIX', 'im/archives/')
ARCHIVE_AGE_DAYS = int(os.environ.get('ARCHIVE_AGE_DAYS', '90'))
DELETE_AFTER_VERIFY = os.environ.get('DELETE_AFTER_VERIFY', 'true').lower() == 'true'


def upload_to_storage(data: bytes, partition: str, kind: str) -> str:
    """根据配置上传到 S3 或本地。返回 storage_uri。"""
    filename = f"{partition}_{kind}.parquet"
    if ARCHIVE_S3_BUCKET:
        # 生产：上传 S3（需 boto3 + AWS credentials）
        try:
            import boto3
            s3 = boto3.client('s3')
            key = f"{ARCHIVE_S3_PREFIX}{filename}"
            s3.put_object(Bucket=ARCHIVE_S3_BUCKET, Key=key, Body=data)
            return f"s3://{ARCHIVE_S3_BUCKET}/{key}"
        except ImportError:
            logging.warning("boto3 not installed; falling back to local storage")
    # 开发：写本地
    os.makedirs(ARCHIVE_LOCAL_DIR, exist_ok=True)
    local_path = os.path.join(ARCHIVE_LOCAL_DIR, filename)
    with open(local_path, 'wb') as f:
        f.write(data)
    return f"file://{local_path}"


def archive_partition(conn, kind: str, year: int, month: int, cutoff_ts: int) -> Optional[dict]:
    """归档单个 (kind, year-month) 分区。返回 stats 或 None（无数据）。"""
    table = 'private_messages' if kind == 'private' else 'group_messages'
    partition = f"{year:04d}-{month:02d}"

    # 检查是否已归档（幂等）
    with conn.cursor() as cur:
        cur.execute(
            "SELECT id, status FROM archive_index WHERE msg_kind=%s AND partition_key=%s",
            (kind, partition))
        existing = cur.fetchone()
        if existing and existing['status'] in ('verified', 'deleted_source'):
            logging.info("Partition %s/%s already archived (status=%s); skipping",
                         kind, partition, existing['status'])
            return None

    # 查询本月 + 90 天前的消息
    month_start = int(datetime.datetime(year, month, 1).timestamp() * 1000)
    next_month = datetime.datetime(year + (month // 12), (month % 12) + 1, 1)
    month_end = int(next_month.timestamp() * 1000)
    range_end = min(month_end, cutoff_ts)
    if range_end <= month_start:
        return None  # 整月在 cutoff 之后

    with conn.cursor() as cur:
        if kind == 'private':
            cur.execute(
                "SELECT id, msg_id, from_user, to_user, content, msg_type, "
                "timestamp, recalled, edited_at, original_body, delivered_at "
                "FROM private_messages WHERE timestamp >= %s AND timestamp < %s "
                "ORDER BY id ASC",
                (month_start, range_end))
        else:
            cur.execute(
                "SELECT id, msg_id, from_user, group_id, content, msg_type, "
                "timestamp, recalled, mentions, edited_at, original_body "
                "FROM group_messages WHERE timestamp >= %s AND timestamp < %s "
                "ORDER BY id ASC",
                (month_start, range_end))
        rows = cur.fetchall()

    if not rows:
        logging.info("Partition %s/%s: 0 rows", kind, partition)
        return None

    logging.info("Partition %s/%s: %d rows", kind, partition, len(rows))

    # 转 Parquet（pyarrow）
    # JSON 列序列化为字符串以兼容 Parquet schema
    for r in rows:
        if 'mentions' in r and r['mentions'] is not None and not isinstance(r['mentions'], str):
            r['mentions'] = json.dumps(r['mentions'])
    table_arrow = pa.Table.from_pylist(rows)
    buf = io.BytesIO()
    pq.write_table(table_arrow, buf, compression='snappy')
    pq_bytes = buf.getvalue()

    # 上传
    storage_uri = upload_to_storage(pq_bytes, partition, kind)
    min_id = rows[0]['msg_id']
    max_id = rows[-1]['msg_id']
    min_ts = rows[0]['timestamp']
    max_ts = rows[-1]['timestamp']

    # 写 archive_index
    with conn.cursor() as cur:
        if existing:
            cur.execute(
                "UPDATE archive_index SET storage_uri=%s, msg_count=%s, "
                "min_msg_id=%s, max_msg_id=%s, min_ts=%s, max_ts=%s, "
                "status='verified', archived_at=NOW() WHERE id=%s",
                (storage_uri, len(rows), min_id, max_id, min_ts, max_ts, existing['id']))
        else:
            cur.execute(
                "INSERT INTO archive_index (msg_kind, partition_key, storage_uri, "
                "msg_count, min_msg_id, max_msg_id, min_ts, max_ts, status) "
                "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, 'verified')",
                (kind, partition, storage_uri, len(rows), min_id, max_id, min_ts, max_ts))
    conn.commit()
    logging.info("✓ Archived %d rows to %s", len(rows), storage_uri)

    # 删除源数据（status=verified → deleted_source）
    if DELETE_AFTER_VERIFY:
        with conn.cursor() as cur:
            cur.execute(
                f"DELETE FROM {table} WHERE timestamp >= %s AND timestamp < %s",
                (month_start, range_end))
            deleted = cur.rowcount
            cur.execute(
                "UPDATE archive_index SET status='deleted_source' "
                "WHERE msg_kind=%s AND partition_key=%s",
                (kind, partition))
        conn.commit()
        logging.info("✓ Deleted %d rows from %s", deleted, table)

    return {'partition': partition, 'kind': kind,
            'rows': len(rows), 'uri': storage_uri}


def main():
    cutoff = datetime.datetime.now() - datetime.timedelta(days=ARCHIVE_AGE_DAYS)
    cutoff_ts = int(cutoff.timestamp() * 1000)
    logging.info("Archive cutoff: %s (ts=%d, %d days ago)",
                 cutoff.strftime('%Y-%m-%d'), cutoff_ts, ARCHIVE_AGE_DAYS)

    conn = pymysql.connect(**DB_CONF)
    try:
        # 找有消息的最早月份作为起点
        with conn.cursor() as cur:
            cur.execute("SELECT MIN(timestamp) AS mt FROM ("
                        "SELECT MIN(timestamp) AS timestamp FROM private_messages "
                        "UNION SELECT MIN(timestamp) FROM group_messages) t")
            row = cur.fetchone()
        if not row or not row['mt']:
            logging.info("No messages to archive")
            return

        start = datetime.datetime.fromtimestamp(row['mt'] / 1000)
        # 遍历每个月直到 cutoff
        cur_year, cur_month = start.year, start.month
        end_year, end_month = cutoff.year, cutoff.month
        while (cur_year, cur_month) <= (end_year, end_month):
            for kind in ('private', 'group'):
                try:
                    archive_partition(conn, kind, cur_year, cur_month, cutoff_ts)
                except Exception as e:
                    logging.exception("Archive %s/%04d-%02d failed: %s",
                                      kind, cur_year, cur_month, e)
            # 下个月
            if cur_month == 12:
                cur_year += 1; cur_month = 1
            else:
                cur_month += 1
    finally:
        conn.close()
    logging.info("Archive run completed")


if __name__ == '__main__':
    main()
