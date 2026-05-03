#!/usr/bin/env bash
# Phase 5b.6: 起整套 observability —— Prometheus + 4 个 exporter
#
# 端口：
#   prometheus               9090
#   node_exporter            9100
#   redis_exporter           9121
#   mysqld_exporter          9104
#   kafka_exporter           9308
#   haproxy /metrics         8404 (haproxy 自身已起)
#   muduo-im /metrics        8080 (chat server 已起)
#   etcd /metrics            2379
#
# 启动：bash deploy/observability/up.sh
# 停止：bash deploy/observability/down.sh
# 看：  http://127.0.0.1:9090
set -uo pipefail
cd "$(dirname "$0")"

CONF=$(pwd)/configs/prometheus.yml
MYCNF=$(pwd)/configs/.my.cnf
DATA_ROOT=/tmp/observability
mkdir -p "$DATA_ROOT"

# 系统服务起着的话先停（避免端口冲突）
for svc in prometheus prometheus-node-exporter prometheus-redis-exporter prometheus-mysqld-exporter; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
        echo "stopping system $svc"
        sudo systemctl stop "$svc"
    fi
done

start_proc() {
    local name=$1
    local pidfile="$DATA_ROOT/$name.pid"
    local logfile="$DATA_ROOT/$name.log"
    shift
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name already running pid=$(cat "$pidfile")"
        return
    fi
    echo "start $name -> $logfile"
    nohup "$@" > "$logfile" 2>&1 &
    echo $! > "$pidfile"
    sleep 0.3
}

# 1. node_exporter (机器层指标)
start_proc node-exporter prometheus-node-exporter \
    --web.listen-address=:9100

# 2. redis_exporter (默认连 :6379；sentinel 模式自动发现)
start_proc redis-exporter prometheus-redis-exporter \
    -redis.addr=redis://127.0.0.1:6379 \
    -web.listen-address=:9121

# 3. mysqld_exporter
start_proc mysqld-exporter env DATA_SOURCE_NAME="root:@(127.0.0.1:3306)/" \
    prometheus-mysqld-exporter \
    --config.my-cnf="$MYCNF" \
    --web.listen-address=:9104 \
    --collect.global_status \
    --collect.global_variables \
    --collect.slave_status \
    --collect.info_schema.processlist

# 4. kafka_exporter
start_proc kafka-exporter kafka_exporter \
    --kafka.server=127.0.0.1:9092 \
    --kafka.server=127.0.0.1:9192 \
    --kafka.server=127.0.0.1:9292 \
    --web.listen-address=:9308

# 5a. Alertmanager（在 prometheus 前起，让 prometheus 启动时能连上）
ALERT_CFG=$(pwd)/configs/alertmanager.yml
start_proc alertmanager alertmanager \
    --config.file="$ALERT_CFG" \
    --storage.path="$DATA_ROOT/alertmanager" \
    --web.listen-address=:9093 \
    --cluster.listen-address=

# 5b. Prometheus
start_proc prometheus prometheus \
    --config.file="$CONF" \
    --storage.tsdb.path="$DATA_ROOT/prom-data" \
    --web.listen-address=:9090 \
    --storage.tsdb.retention.time=7d \
    --web.enable-lifecycle

# 6. Grafana —— 用本仓库的 provisioning 目录覆盖系统默认（不污染 /etc/grafana）
GRAFANA_HOME=$(pwd)/grafana
GF_RUNTIME=$DATA_ROOT/grafana
mkdir -p "$GF_RUNTIME/data" "$GF_RUNTIME/logs" "$GF_RUNTIME/plugins"
mkdir -p "$GF_RUNTIME/provisioning/datasources" \
         "$GF_RUNTIME/provisioning/dashboards" \
         "$GF_RUNTIME/provisioning/plugins" \
         "$GF_RUNTIME/provisioning/notifiers" \
         "$GF_RUNTIME/provisioning/alerting" \
         "$GF_RUNTIME/provisioning/access-control"
# 复制（而非软链）provisioning 文件，确保 grafana 用户可读
cp "$GRAFANA_HOME/provisioning/datasources/prometheus.yml" \
   "$GF_RUNTIME/provisioning/datasources/"
# dashboards.yml 用绝对路径（grafana 不支持 env 替换 path）
cat > "$GF_RUNTIME/provisioning/dashboards/default.yml" <<EOF
apiVersion: 1
providers:
  - name: muduo-im-dashboards
    folder: muduo-im
    type: file
    disableDeletion: false
    updateIntervalSeconds: 30
    allowUiUpdates: true
    options:
      path: $GRAFANA_HOME/dashboards
      foldersFromFilesStructure: false
EOF
chmod -R a+rX "$GF_RUNTIME" "$GRAFANA_HOME"

# 我们不读 /etc/grafana/grafana.ini（grafana 用户 only），全靠环境变量配置
GF_CONFIG="$GF_RUNTIME/grafana.ini"
cat > "$GF_CONFIG" <<EOF
[paths]
data = $GF_RUNTIME/data
logs = $GF_RUNTIME/logs
plugins = $GF_RUNTIME/plugins
provisioning = $GF_RUNTIME/provisioning

[server]
http_port = 3000

[security]
admin_user = admin
admin_password = admin

[auth.anonymous]
enabled = true
org_role = Viewer

[analytics]
reporting_enabled = false
check_for_updates = false
EOF

start_proc grafana grafana server \
    --homepath /usr/share/grafana \
    --config "$GF_CONFIG"

sleep 2
echo
echo "=== process state ==="
for name in node-exporter redis-exporter mysqld-exporter kafka-exporter alertmanager prometheus grafana; do
    pidfile="$DATA_ROOT/$name.pid"
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "  ✓ $name pid=$(cat "$pidfile")"
    else
        echo "  ✗ $name DOWN — see $DATA_ROOT/$name.log"
    fi
done

echo
echo "=== ports ==="
ss -ltn 2>/dev/null | grep -E ':(9090|9100|9104|9121|9308|3000)\s' | head -10

echo
echo "✓ Observability stack up."
echo "  Prometheus UI : http://127.0.0.1:9090"
echo "  Targets       : http://127.0.0.1:9090/targets"
echo "  Alerts        : http://127.0.0.1:9090/alerts"
echo "  Alertmanager  : http://127.0.0.1:9093"
echo "  Grafana UI    : http://127.0.0.1:3000  (匿名只读 / admin:admin 写)"
echo "  Dashboards    : http://127.0.0.1:3000/dashboards (folder: muduo-im)"
echo "  Stop          : bash deploy/observability/down.sh"
