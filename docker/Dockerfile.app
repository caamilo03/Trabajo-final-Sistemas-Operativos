# ── Etapa de build ──────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    cmake \
    make \
    libpthread-stubs0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build build --parallel $(nproc)

# ── Etapa de runtime ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpthread-stubs0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/src/server/perfanalyzer_server ./perfanalyzer_server
COPY --from=builder /src/build/src/cli/perfctl                ./perfctl
COPY --from=builder /src/build/src/workloads/cpu_stress       ./cpu_stress
COPY --from=builder /src/build/src/workloads/mem_stress       ./mem_stress
COPY --from=builder /src/build/src/workloads/io_stress        ./io_stress

EXPOSE 8080

HEALTHCHECK --interval=5s --timeout=3s --retries=3 \
    CMD wget -qO- http://localhost:8080/health || exit 1

CMD ["./perfanalyzer_server", "--port", "8080"]
