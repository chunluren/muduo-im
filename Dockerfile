# Multi-stage build for muduo-im
# Stage 1: build the server binary in a full toolchain image
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential cmake git \
    libssl-dev zlib1g-dev \
    libmysqlclient-dev libhiredis-dev \
    libargon2-dev \
    libprotobuf-dev protobuf-compiler \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make muduo-im -j$(nproc)

# Stage 2: slim runtime image with only shared libs
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libssl3 zlib1g \
    libmysqlclient21 libhiredis0.14 \
    libargon2-1 \
    libprotobuf23 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /build/build/muduo-im /app/
COPY --from=builder /build/web /app/web
COPY --from=builder /build/sql /app/sql
COPY docker-config.ini /app/config.ini

# Create logs directory for log_file output
RUN mkdir -p /app/logs

EXPOSE 8080 9090

# Healthcheck uses the /health endpoint exposed by HttpServer
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

# ChatServer code uses relative paths like "../web" which resolve against the
# process working directory. Place the binary in /app/build so ../web -> /app/web.
RUN mkdir -p /app/build && cp /app/muduo-im /app/build/muduo-im

WORKDIR /app/build
CMD ["./muduo-im", "/app/config.ini"]
