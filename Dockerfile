# MiniVectorDB 评测镜像（多阶段构建）
# 构建阶段：从源码编译
FROM ubuntu:24.04 AS build

RUN apt-get update && \
    apt-get install -y --no-install-recommends g++ make cmake && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j2

# 运行阶段：仅保留可执行文件
FROM ubuntu:24.04

WORKDIR /app
COPY --from=build /app/build/minivectordb /app/minivectordb

RUN mkdir -p /data
VOLUME ["/data"]

ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
