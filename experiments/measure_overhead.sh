#!/usr/bin/env bash
# measure_overhead.sh — Cuantifica el overhead de libperfanalyzer
#
# Ejecuta cada workload N réplicas en dos modos:
#   - with_sampler:   PERF_PROBE activo + sampler de fondo
#   - no_sampler:     mismo binario compilado con -DPERF_DISABLE_SAMPLING
#                     (la macro PERF_PROBE se vuelve no-op)
# Compara el throughput entre ambos para reportar el % de overhead.
#
# Uso:
#   chmod +x experiments/measure_overhead.sh
#   experiments/measure_overhead.sh [--replicas N] [--duration S]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD="$ROOT_DIR/build-release"
RESULTS="$SCRIPT_DIR/results"

REPLICAS=30
DURATION=10
WARMUP=2          # corridas de calentamiento descartadas por (wl,inten)
TS=$(date +%Y%m%d_%H%M%S)
OUT="$RESULTS/overhead_${TS}.csv"

# Fijar afinidad a un núcleo reduce el ruido por migración/escalado de
# frecuencia. Se aplica idéntico a ambos modos, así que el delta de overhead
# permanece válido y con menor varianza.
TASKSET=""
if command -v taskset &>/dev/null; then
    TASKSET="taskset -c 0"
fi

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
log() { echo -e "${GREEN}[overhead]${NC} $*"; }
die() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --replicas) REPLICAS="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        *) echo "Uso: $0 [--replicas N] [--duration S]"; exit 1 ;;
    esac
done

mkdir -p "$RESULTS"
echo "workload,intensity,mode,replica,throughput,cpu_pct_avg" > "$OUT"

parse_output() {
    local file="$1"
    local throughput cpu_avg
    throughput=$(grep -E '^(matmul_iters|alloc_iters|iters),' "$file" \
                 | head -1 | cut -d, -f2 || echo 0)
    cpu_avg=$(grep    '^cpu_pct_avg,' "$file" | cut -d, -f2 || echo 0)
    echo "${throughput},${cpu_avg}"
}

# Verificar binarios
for wl in cpu_stress mem_stress io_stress; do
    [[ -x "$BUILD/src/workloads/${wl}" ]]            || die "Falta ${wl}. Ejecuta 'make release'."
    [[ -x "$BUILD/src/workloads/${wl}_nosampler" ]]  || die "Falta ${wl}_nosampler. Ejecuta 'make release'."
done

# Ejecuta una corrida de un modo y registra (si record=1).
run_one() {
    local wl="$1" inten="$2" mode="$3" rep="$4" record="$5"
    local bin
    if [[ "$mode" == "with_sampler" ]]; then
        bin="$BUILD/src/workloads/$wl"
    else
        bin="$BUILD/src/workloads/${wl}_nosampler"
    fi
    local tmpf; tmpf=$(mktemp)
    $TASKSET "$bin" --duration "$DURATION" --intensity "$inten" \
        > "$tmpf" 2>/dev/null
    if [[ "$record" == "1" ]]; then
        local vals; vals=$(parse_output "$tmpf")
        echo "$wl,$inten,$mode,$rep,$vals" >> "$OUT"
    fi
    rm -f "$tmpf"
}

for wl in cpu_stress mem_stress io_stress; do
    for inten in 1 2; do
        log "── $wl inten=$inten (warmup=$WARMUP, n=$REPLICAS, orden aleatorizado) ──"

        # Calentamiento (no registrado): estabiliza caché y frecuencia de CPU
        for ((w=0; w<WARMUP; w++)); do
            run_one "$wl" "$inten" with_sampler 0 0
            run_one "$wl" "$inten" no_sampler   0 0
        done

        # Réplicas intercaladas con orden aleatorio por réplica: ambos modos
        # sufren por igual cualquier deriva térmica, eliminando el sesgo
        # sistemático que daba "overhead negativo".
        for rep in $(seq 1 "$REPLICAS"); do
            if (( RANDOM % 2 )); then
                run_one "$wl" "$inten" with_sampler "$rep" 1
                run_one "$wl" "$inten" no_sampler   "$rep" 1
            else
                run_one "$wl" "$inten" no_sampler   "$rep" 1
                run_one "$wl" "$inten" with_sampler "$rep" 1
            fi
            echo -n "."
        done
        echo ""
    done
done

log "Overhead medido. Resultados en: $OUT"
log "Analizando..."

OUT="$OUT" python3 - <<'PYEOF'
import csv, statistics, os, sys

path = os.environ['OUT']
rows = list(csv.DictReader(open(path)))

# Agrupar por (workload, intensidad, mode) y promediar throughput
groups = {}
for r in rows:
    key = (r['workload'], r['intensity'], r['mode'])
    try:
        t = float(r['throughput'])
    except ValueError:
        continue
    groups.setdefault(key, []).append(t)

print(f"\n{'Workload':<14} {'Inten':>5} {'Modo':<14} "
      f"{'Throughput media':>18} {'std':>10}")
print("-" * 72)
for k in sorted(groups):
    vals = groups[k]
    m = statistics.mean(vals)
    s = statistics.stdev(vals) if len(vals) > 1 else 0.0
    print(f"{k[0]:<14} {k[1]:>5} {k[2]:<14} {m:>18.1f} {s:>10.2f}")

# Overhead % = (no_sampler - with_sampler) / no_sampler * 100, con IC 95%.
# SE de la diferencia de medias (muestras independientes): sqrt(s1^2/n1 + s2^2/n2).
import math
print(f"\n{'Workload':<14} {'Inten':>5} {'Overhead %':>12} {'IC95 %':>20} {'¿signif?':>10}")
print("-" * 72)
workloads = sorted({(r['workload'], r['intensity']) for r in rows})
for wl, inten in workloads:
    base = groups.get((wl, inten, 'no_sampler'),    [])
    inst = groups.get((wl, inten, 'with_sampler'),  [])
    if len(base) < 2 or len(inst) < 2:
        continue
    mb, mi = statistics.mean(base), statistics.mean(inst)
    sb, si = statistics.stdev(base), statistics.stdev(inst)
    if mb <= 0:
        continue
    overhead = (mb - mi) / mb * 100
    # SE de (mb - mi), propagada a porcentaje dividiendo por mb
    se_diff = math.sqrt(sb*sb/len(base) + si*si/len(inst))
    se_pct  = se_diff / mb * 100
    lo, hi  = overhead - 1.96*se_pct, overhead + 1.96*se_pct
    signif  = "sí" if (lo > 0 or hi < 0) else "no (~0)"
    print(f"{wl:<14} {inten:>5} {overhead:>11.2f}% "
          f"[{lo:>7.2f}, {hi:>7.2f}] {signif:>10}")

print("\nNota: 'no (~0)' significa que el IC 95% incluye cero, es decir el")
print("overhead del sampler no es estadísticamente distinguible de cero.")
PYEOF
