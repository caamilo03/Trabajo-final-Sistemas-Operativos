# ── Etapa de build ──────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS builder

# build-essential garantiza gcc + libc6-dev + binutils (linker funcional).
# Usar solo "gcc" con --no-install-recommends omite libc6-dev y rompe el linker.
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build build --parallel

# ── Etapa de runtime ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

# wget para el HEALTHCHECK. libc/pthread ya vienen en la imagen base (glibc 2.36
# integra libpthread). No se necesita toolchain en runtime.
RUN apt-get update && apt-get install -y --no-install-recommends \
    wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/src/server/perfanalyzer_server   ./perfanalyzer_server
COPY --from=builder /src/build/src/cli/perfctl                  ./perfctl
COPY --from=builder /src/build/src/tools/container_monitor      ./container_monitor
COPY --from=builder /src/build/src/workloads/cpu_stress         ./cpu_stress
COPY --from=builder /src/build/src/workloads/mem_stress         ./mem_stress
COPY --from=builder /src/build/src/workloads/io_stress          ./io_stress

EXPOSE 8080

HEALTHCHECK --interval=5s --timeout=3s --retries=3 \
    CMD wget -qO- http://localhost:8080/health || exit 1

CMD ["./perfanalyzer_server", "--port", "8080"]
