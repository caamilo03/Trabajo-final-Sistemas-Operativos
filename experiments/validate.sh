#!/usr/bin/env bash
# validate.sh — Compara métricas de perfanalyzer vs docker stats
#
# Ejecuta el servidor en un contenedor, mide la misma carga con:
#   (A) perfanalyzer /snapshot  (desde dentro del contenedor)
#   (B) container_monitor       (desde el host vía cgroup v2)
#   (C) docker stats            (referencia oficial de Docker)
# y reporta la diferencia relativa entre ellos.
#
# Uso:
#   chmod +x experiments/validate.sh
#   experiments/validate.sh [--duration 30] [--workload cpu_stress]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD="$ROOT_DIR/build-release"
RESULTS="$SCRIPT_DIR/results"
COMPOSE="$ROOT_DIR/docker/docker-compose.yml"

DURATION=30
WORKLOAD="cpu_stress"
INTENSITY=2
CONTAINER="perf_baseline"
PORT=8080

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[validate]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)  DURATION="$2";  shift 2 ;;
        --workload)  WORKLOAD="$2";  shift 2 ;;
        --intensity) INTENSITY="$2"; shift 2 ;;
        *) echo "Uso: $0 [--duration N] [--workload W] [--intensity 1|2]"; exit 1 ;;
    esac
done

mkdir -p "$RESULTS"
TS=$(date +%Y%m%d_%H%M%S)
OUT="$RESULTS/validation_${TS}.csv"

# ── 1. Levantar contenedor baseline ─────────────────────────────────────────
log "Levantando contenedor $CONTAINER..."
docker compose -f "$COMPOSE" --profile baseline up -d --build 2>/dev/null
sleep 5   # esperar healthcheck

# ── 2. Verificar que el servidor responde ────────────────────────────────────
log "Verificando servidor en contenedor..."
MAX_WAIT=20
for i in $(seq 1 $MAX_WAIT); do
    if curl -sf "http://localhost:$PORT/health" &>/dev/null; then
        log "Servidor OK ✓"
        break
    fi
    [[ $i -eq $MAX_WAIT ]] && { echo "ERROR: servidor no responde"; exit 1; }
    sleep 1
done

# ── 3. Arrancar workload en el contenedor ────────────────────────────────────
log "Arrancando workload $WORKLOAD (intensidad=$INTENSITY, duración=${DURATION}s)..."
docker exec -d "$CONTAINER" "./${WORKLOAD}" \
    --duration "$DURATION" --intensity "$INTENSITY" 2>/dev/null || true

# ── 4. Recolectar métricas en paralelo ──────────────────────────────────────
log "Recolectando métricas (${DURATION}s)..."

PERF_OUT="$RESULTS/validate_perf_${TS}.csv"
CMON_OUT="$RESULTS/validate_cmon_${TS}.csv"
DSTAT_OUT="$RESULTS/validate_dstat_${TS}.csv"

# (A) perfanalyzer /snapshot — mide el CONTENEDOR completo vía cgroup v2.
#     IMPORTANTE: usamos cg_cpu_usage_us (tiempo de CPU del cgroup entero),
#     NO cpu_pct (que es solo del proceso servidor, idle). El %CPU se calcula
#     como delta de uso de CPU entre dos snapshots sobre el tiempo de reloj.
#     Así perfanalyzer mide lo mismo que container_monitor y docker stats.
(
    echo "ts,source,cpu_pct,mem_rss_mb"
    prev_cg=""; prev_t=""
    END=$((SECONDS + DURATION))
    while [[ $SECONDS -lt $END ]]; do
        snap=$(curl -sf "http://localhost:$PORT/snapshot" 2>/dev/null || echo '{}')
        read -r cg t mem <<< "$(echo "$snap" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    print('0 0 0'); sys.exit()
cg  = d.get('cg_cpu_usage_us', 0)
t   = d.get('ts_sec', 0) + d.get('ts_nsec', 0) * 1e-9
mem = round(d.get('cg_mem_current', 0) / 1048576.0, 2)
print(cg, t, mem)
" 2>/dev/null || echo '0 0 0')"
        if [[ -n "$prev_cg" && "$prev_cg" != "0" ]]; then
            cpu=$(python3 -c "
dt = $t - $prev_t
print(round(($cg - $prev_cg) / (dt * 1e6) * 100, 2) if dt > 0 else 0)" 2>/dev/null || echo 0)
            echo "$(date +%s),perfanalyzer,$cpu,$mem"
        fi
        prev_cg="$cg"; prev_t="$t"
        sleep 1
    done
) > "$PERF_OUT" &
PID_PERF=$!

# (B) container_monitor — desde el host vía cgroup v2
if [[ -x "$BUILD/src/tools/container_monitor" ]]; then
    "$BUILD/src/tools/container_monitor" \
        --name "$CONTAINER" \
        --interval 1000 \
        --duration "$DURATION" \
        --output csv \
        --out-file "$CMON_OUT" &
    PID_CMON=$!
else
    warn "container_monitor no encontrado, omitiendo fuente B."
    PID_CMON=""
fi

# (C) docker stats — una muestra cada segundo
(
    echo "ts,source,cpu_pct,mem_mb"
    END=$((SECONDS + DURATION))
    while [[ $SECONDS -lt $END ]]; do
        line=$(docker stats --no-stream --format \
            "{{.CPUPerc}},{{.MemUsage}}" "$CONTAINER" 2>/dev/null || echo "0%,0MiB / 0MiB")
        ts=$(date +%s)
        cpu=$(echo "$line" | cut -d, -f1 | tr -d '%')
        mem=$(echo "$line" | cut -d, -f2 | awk '{print $1}' | tr -d 'MiBGiB')
        echo "$ts,docker_stats,$cpu,$mem"
        sleep 1
    done
) > "$DSTAT_OUT" &
PID_DSTAT=$!

# Esperar a que terminen
wait $PID_PERF 2>/dev/null || true
[[ -n "$PID_CMON" ]] && wait "$PID_CMON" 2>/dev/null || true
wait $PID_DSTAT 2>/dev/null || true

# ── 5. Análisis de diferencia relativa ──────────────────────────────────────
log "Analizando diferencias..."

export RESULTS_DIR="$RESULTS"
export VAL_TS="$TS"

python3 - <<'PYEOF'
import sys, csv, statistics, os

results_dir = os.environ.get('RESULTS_DIR', 'experiments/results')
ts = os.environ.get('VAL_TS', '')

def load(path, source):
    rows = []
    try:
        with open(path) as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    rows.append(float(row.get('cpu_pct', row.get('cpu_percent', 0)) or 0))
                except ValueError:
                    pass
    except FileNotFoundError:
        pass
    return rows

import glob
perf_files  = glob.glob(f'{results_dir}/validate_perf_*.csv')
cmon_files  = glob.glob(f'{results_dir}/validate_cmon_*.csv')
dstat_files = glob.glob(f'{results_dir}/validate_dstat_*.csv')

a = load(perf_files[-1],  'perfanalyzer') if perf_files  else []
b = load(cmon_files[-1],  'cmonitor')     if cmon_files  else []
c = load(dstat_files[-1], 'docker_stats') if dstat_files else []

def mean_safe(lst): return statistics.mean(lst) if lst else float('nan')

ma, mb, mc = mean_safe(a), mean_safe(b), mean_safe(c)
print(f"\n{'Fuente':<20} {'CPU% promedio':>14} {'n muestras':>12}")
print("-" * 50)
print(f"{'perfanalyzer':<20} {ma:>14.2f} {len(a):>12}")
print(f"{'container_monitor':<20} {mb:>14.2f} {len(b):>12}")
print(f"{'docker stats':<20} {mc:>14.2f} {len(c):>12}")

ref = mc if mc == mc and mc > 0 else ma
if ref > 0:
    print("\nDiferencia relativa respecto a docker stats:")
    for label, val in [('perfanalyzer', ma), ('container_monitor', mb)]:
        if val == val:
            diff = abs(val - ref) / ref * 100
            ok = "✓ (<5%)" if diff < 5 else "✗ (>5%)"
            print(f"  {label:<20}: {diff:.1f}% {ok}")
PYEOF

# ── 6. Detener contenedor ────────────────────────────────────────────────────
log "Deteniendo contenedor..."
docker compose -f "$COMPOSE" --profile baseline down 2>/dev/null || true

log "Archivos de validación:"
log "  $PERF_OUT"
log "  $CMON_OUT"
log "  $DSTAT_OUT"
