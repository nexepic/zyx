FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONPATH=/app

WORKDIR /app

RUN python -m pip install --no-cache-dir --upgrade pip \
    && python -m pip install --no-cache-dir neo4j kuzu PyYAML pytest

COPY benchmarks/graphdb-comparison/dataset ./dataset
COPY benchmarks/graphdb-comparison/runner ./runner
COPY benchmarks/graphdb-comparison/workloads ./workloads

VOLUME ["/results"]

CMD ["python", "-m", "runner.run", "--database", "fake", "--database", "kuzu", "--scale", "smoke", "--output-root", "/results", "--warmup", "1", "--iterations", "1"]
