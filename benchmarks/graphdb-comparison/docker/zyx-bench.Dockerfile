FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive \
    CONAN_HOME=/root/.conan2

WORKDIR /src

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        ninja-build \
        python3 \
        python3-pip \
        python3-venv \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/conan \
    && /opt/conan/bin/pip install --no-cache-dir --upgrade pip conan
ENV PATH=/opt/conan/bin:$PATH

COPY . .

RUN conan profile detect --force \
    && conan install . --output-folder=build-docker --build=missing -s build_type=Release -o '&:with_tests=False' \
    && cmake -S . -B build-docker -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build-docker/conan_toolchain.cmake -DZYX_BUILD_TESTS=OFF \
    && cmake --build build-docker --target zyx_compare_bench

FROM python:3.12-slim AS runtime

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONPATH=/app \
    ZYX_COMPARE_BENCH=/usr/local/bin/zyx-compare-bench

WORKDIR /app

RUN python -m pip install --no-cache-dir --upgrade pip \
    && python -m pip install --no-cache-dir neo4j kuzu PyYAML pytest

COPY --from=build /src/build-docker/zyx-compare-bench /usr/local/bin/zyx-compare-bench
COPY benchmarks/graphdb-comparison/dataset ./dataset
COPY benchmarks/graphdb-comparison/runner ./runner
COPY benchmarks/graphdb-comparison/workloads ./workloads

VOLUME ["/results"]

CMD ["python", "-m", "runner.run", "--scale", "smoke", "--output-root", "/results", "--warmup", "1", "--iterations", "1"]
