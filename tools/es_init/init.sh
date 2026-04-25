#!/bin/bash
# ElasticSearch 索引初始化（Phase 4.4）
# 用法：./tools/es_init/init.sh [http://localhost:9200]

set -e
ES="${1:-http://localhost:9200}"
INDEX="messages"

echo "=== 等待 ES 集群就绪 ==="
for i in {1..30}; do
    if curl -sf "$ES/_cluster/health?wait_for_status=yellow&timeout=5s" > /dev/null; then
        echo "ES ready"
        break
    fi
    echo "  retry $i/30..."
    sleep 2
done

curl -s "$ES/_cluster/health" | python3 -m json.tool

echo "=== 创建索引 $INDEX ==="
curl -X DELETE "$ES/$INDEX" -s -o /dev/null || true

curl -X PUT "$ES/$INDEX" -H 'Content-Type: application/json' -d '{
  "settings": {
    "number_of_shards": 10,
    "number_of_replicas": 1,
    "refresh_interval": "5s"
  },
  "mappings": {
    "properties": {
      "msg_id":       { "type": "keyword" },
      "body":         { "type": "text", "analyzer": "ik_max_word", "search_analyzer": "ik_smart" },
      "sender_id":    { "type": "long" },
      "sender_name":  { "type": "keyword" },
      "recipient_id": { "type": "long" },
      "group_id":     { "type": "long" },
      "msg_kind":     { "type": "keyword" },
      "created_at":   { "type": "date", "format": "epoch_millis" }
    }
  }
}'
echo ""
echo "=== 索引创建成功 ==="

# 注：IK 分词器需要在 ES 镜像里预装，未装时把 analyzer 改为 standard
# 安装方法：进入容器执行
#   bin/elasticsearch-plugin install https://github.com/medcl/elasticsearch-analysis-ik/releases/download/v8.11.0/elasticsearch-analysis-ik-8.11.0.zip
