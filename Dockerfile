FROM ubuntu:22.04

# 一次性安装所有编译+测试工具（永久生效）
RUN apt update && apt install -y \
    gcc \
    make \
    bison \
    flex \
    netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# 编译（可选）
RUN make
