#!/usr/bin/env bash
# run_experiment.sh — Orquestador de experimentos para perfanalyzer
#
# Ejecuta la matriz: entorno × workload × intensidad × N réplicas
# y guarda cada corrida como una fila CSV en experiments/results/.
#
# Uso:
#   chmod +x experiments/run_experiment.sh
#   experiments/run_experiment.sh [--replicas N] [--duration S]
#
# Requisitos: Docker con cgroup v2, build del proyecto en ./build-release/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="$SCRIPT_DIR/results"
BUILD_DIR="$ROOT_DIR/build-release"

REPLICAS=30
DURATION=10      # segundos por corrida
WARMUP_REPS=2    # corridas de calentamiento descartadas

DOCKER_COMPOSE="$ROOT_DIR/docker/docker-compose.yml"
SERVER_PORT=8080

# Colores para logs
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[EXP]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
die()  { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

# ── Parseo de argumentos ────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --replicas) REPLICAS="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        *) die "Argumento desconocido: $1" ;;
    esac
done

# ── Verificaciones previas ──────────────────────────────────────────────────
[[ -d "$BUILD_DIR" ]] || die "Build release no encontrado: $BUILD_DIR. Ejecuta 'make release' primero."
[[ -f "$BUILD_DIR/src/workloads/cpu_stress" ]] || die "Binarios no encontrados en $BUILD_DIR."

CGROUP_TYPE=$(stat -fc %T /sys/fs/cgroup 2>/dev/null || echo "unknown")
if [[ "$CGROUP_TYPE" != "cgroup2fs" ]]; then
    warn "cgroup v2 no detectado (tipo: $CGROUP_TYPE). Las métricas de cgroup estarán vacías."
fi

mkdir -p "$RESULTS_DIR"

# ── CSV header ──────────────────────────────────────────────────────────────
CSV="$RESULTS_DIR/raw_results.csv"
if [[ ! -f "$CSV" ]]; then
    echo "run_id,env,profile,workload,intensity,replica,\
cpu_pct_avg,mem_rss_avg_kb,io_read_bytes,io_write_bytes,\
probe_p99_us,throughput,duration_s,timestamp" > "$CSV"
fi

RUN_ID=1
[[ -f "$RESULTS_DIR/.last_run_id" ]] && RUN_ID=$(( $(cat "$RESULTS_DIR/.last_run_id") + 1 ))

# ── Función: correr workload en el host ─────────────────────────────────────
run_host() {
    local workload="$1"
    local intensity="$2"
    local replica="$3"

    local bin="$BUILD_DIR/src/workloads/${workload}"
    [[ -x "$bin" ]] || die "Binario no ejecutable: $bin"

    local tmpout
    tmpout=$(mktemp)
    "$bin" --duration "$DURATION" --intensity "$intensity" > "$tmpout" 2>/dev/null
    parse_and_record "$tmpout" "host" "none" "$workload" "$intensity" "$replica"
    rm -f "$tmpout"
}

# ── Función: correr workload en Docker ──────────────────────────────────────
run_docker() {
    local workload="$1"
    local intensity="$2"
    local replica="$3"
    local profile="$4"   # baseline | cpu-limit | mem-limit

    # Asegurarse de que el contenedor del perfil esté corriendo
    local container_name="perf_${profile//-/_}"
    if ! docker ps --format '{{.Names}}' | grep -q "^${container_name}$"; then
        docker compose -f "$DOCKER_COMPOSE" --profile "$profile" up -d --build \
            2>/dev/null
        sleep 3  # esperar healthcheck
    fi

    local tmpout
    tmpout=$(mktemp)
    docker exec "$container_name" \
        "./${workload}" --duration "$DURATION" --intensity "$intensity" \
        > "$tmpout" 2>/dev/null
    parse_and_record "$tmpout" "docker" "$profile" "$workload" "$intensity" "$replica"
    rm -f "$tmpout"
}

# ── Función: parsear salida CSV del workload y agregar al CSV maestro ────────
parse_and_record() {
    local file="$1"
    local env="$2"
    local profile="$3"
    local workload="$4"
    local intensity="$5"
    local replica="$6"

    local cpu_avg mem_avg io_rb io_wb p99 throughput
    cpu_avg=$(grep   '^cpu_pct_avg,'       "$file" | cut -d, -f2 || echo "0")
    mem_avg=$(grep   '^mem_rss_avg_kb,'    "$file" | cut -d, -f2 || echo "0")
    io_rb=$(grep     '^io_read_bytes'      "$file" | cut -d, -f2 || echo "0")
    io_wb=$(grep     '^io_write_bytes'     "$file" | cut -d, -f2 || echo "0")
    p99=$(grep       '_p99_us,'            "$file" | head -1 | cut -d, -f2 || echo "0")
    throughput=$(grep '^matmul_iters\|^alloc_iters\|^iters,' "$file" \
                    | head -1 | cut -d, -f2 || echo "0")

    echo "$RUN_ID,$env,$profile,$workload,$intensity,$replica,\
$cpu_avg,$mem_avg,$io_rb,$io_wb,$p99,$throughput,$DURATION,$(date -u +%s)" >> "$CSV"
    RUN_ID=$(( RUN_ID + 1 ))
}

# ── Matriz experimental ──────────────────────────────────────────────────────
WORKLOADS=("cpu_stress" "mem_stress" "io_stress")
INTENSITIES=(1 2)           # 1=low, 2=high
DOCKER_PROFILES=("baseline" "cpu-limit" "mem-limit")

total_runs=$(( (REPLICAS + WARMUP_REPS) * ${#WORKLOADS[@]} * ${#INTENSITIES[@]} * (1 + ${#DOCKER_PROFILES[@]}) ))
log "Iniciando experimento: $total_runs corridas totales (${WARMUP_REPS} warm-up + ${REPLICAS} réplicas por celda)"
log "Resultados en: $CSV"

for wl in "${WORKLOADS[@]}"; do
    for inten in "${INTENSITIES[@]}"; do

        log "─── HOST | $wl | intensidad=$inten ───"
        for rep in $(seq 1 $(( REPLICAS + WARMUP_REPS ))); do
            if [[ $rep -le $WARMUP_REPS ]]; then
                # Warm-up: ejecutar pero no registrar
                "$BUILD_DIR/src/workloads/${wl}" \
                    --duration "$DURATION" --intensity "$inten" \
                    > /dev/null 2>&1 || true
            else
                actual_rep=$(( rep - WARMUP_REPS ))
                run_host "$wl" "$inten" "$actual_rep"
                echo -n "."
            fi
        done
        echo ""

        for profile in "${DOCKER_PROFILES[@]}"; do
            log "─── DOCKER:$profile | $wl | intensidad=$inten ───"
            for rep in $(seq 1 $(( REPLICAS + WARMUP_REPS ))); do
                if [[ $rep -le $WARMUP_REPS ]]; then
                    docker compose -f "$DOCKER_COMPOSE" --profile "$profile" up -d --build \
                        2>/dev/null || true
                    sleep 2
                else
                    actual_rep=$(( rep - WARMUP_REPS ))
                    run_docker "$wl" "$inten" "$actual_rep" "$profile"
                    echo -n "."
                fi
            done
            echo ""
        done

    done
done

# Apagar contenedores
log "Apagando contenedores Docker..."
for profile in "${DOCKER_PROFILES[@]}"; do
    docker compose -f "$DOCKER_COMPOSE" --profile "$profile" down 2>/dev/null || true
done

echo "$RUN_ID" > "$RESULTS_DIR/.last_run_id"
log "Experimento completo. CSV: $CSV"
log "Ejecuta: python experiments/analyze.py"
