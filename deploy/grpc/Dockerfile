# Multi-stage build for the Phase 1.2 gRPC topology
# Produces three binaries (registry, logic, gateway) all installed at /app/.
# The compose file picks which one to run via the `command:` field.

FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential cmake git pkg-config \
    libssl-dev zlib1g-dev \
    libmysqlclient-dev libhiredis-dev \
    libargon2-dev \
    libprotobuf-dev protobuf-compiler \
    libgrpc++-dev libgrpc-dev protobuf-compiler-grpc \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# 默认 main 不带 -DWITH_GRPC（保留 monolith muduo-im 二进制可用）；
# 这里追加新的目标。
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DWITH_GRPC=ON && \
    make -j$(nproc) muduo-im muduo-im-registry muduo-im-logic muduo-im-gateway

# ---------- runtime ----------
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libssl3 zlib1g \
    libmysqlclient21 libhiredis0.14 \
    libargon2-1 \
    libprotobuf23 libgrpc++1 libgrpc10 \
    ca-certificates curl tini \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /build/build/muduo-im          /app/
COPY --from=builder /build/build/muduo-im-registry /app/
COPY --from=builder /build/build/muduo-im-logic    /app/
COPY --from=builder /build/build/muduo-im-gateway  /app/
COPY --from=builder /build/sql                     /app/sql
COPY --from=builder /build/web                     /app/web
COPY docker-config.ini                             /app/config.ini

EXPOSE 8080 9090 9091 9092 9100 9101 9200

ENTRYPOINT ["/usr/bin/tini", "--"]
# 默认起 monolith；compose 会用 command: 覆盖
CMD ["/app/muduo-im", "/app/config.ini"]
