#!/usr/bin/env bash
# setup_deps.sh — Descarga e instala dependencias del proyecto
#
# Ejecutar una sola vez antes de compilar:
#   chmod +x scripts/setup_deps.sh
#   scripts/setup_deps.sh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TESTS_DIR="$ROOT_DIR/tests/unity"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
log() { echo -e "${GREEN}[setup]${NC} $*"; }
die() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

# ── 1. Verificar herramientas del sistema ────────────────────────────────────
log "Verificando herramientas del sistema..."

check_tool() {
    command -v "$1" &>/dev/null || die "$1 no encontrado. Instala con: sudo apt install $2"
}
check_tool gcc    "gcc"
check_tool cmake  "cmake"
check_tool make   "make"
check_tool git    "git"
check_tool curl   "curl"
check_tool docker "docker.io"
check_tool python3 "python3"

# ── 2. Verificar cgroup v2 ───────────────────────────────────────────────────
log "Verificando cgroup v2..."
CGROUP_TYPE=$(stat -fc %T /sys/fs/cgroup 2>/dev/null || echo "unknown")
if [[ "$CGROUP_TYPE" == "cgroup2fs" ]]; then
    log "cgroup v2 disponible ✓"
else
    echo -e "${RED}[WARN]${NC} cgroup v2 NO disponible (tipo: $CGROUP_TYPE)."
    echo "  Las métricas de cgroup estarán vacías."
    echo "  Para habilitarlo en Ubuntu: edita /etc/default/grub y agrega:"
    echo "    GRUB_CMDLINE_LINUX=\"systemd.unified_cgroup_hierarchy=1\""
    echo "  Luego: sudo update-grub && sudo reboot"
fi

# ── 3. Unity test framework ──────────────────────────────────────────────────
log "Instalando Unity test framework..."
mkdir -p "$TESTS_DIR"

UNITY_VERSION="2.5.2"
UNITY_BASE="https://raw.githubusercontent.com/ThrowTheSwitch/Unity/v${UNITY_VERSION}/src"

if [[ ! -f "$TESTS_DIR/unity.h" ]]; then
    curl -fsSL "$UNITY_BASE/unity.h" -o "$TESTS_DIR/unity.h"
    curl -fsSL "$UNITY_BASE/unity.c" -o "$TESTS_DIR/unity.c"
    curl -fsSL "$UNITY_BASE/unity_internals.h" -o "$TESTS_DIR/unity_internals.h"
    log "Unity $UNITY_VERSION descargado ✓"
else
    log "Unity ya instalado ✓"
fi

# ── 4. Dependencias Python ───────────────────────────────────────────────────
log "Instalando dependencias Python para análisis..."
if command -v pip3 &>/dev/null; then
    pip3 install -q -r "$ROOT_DIR/experiments/requirements.txt"
    log "Dependencias Python instaladas ✓"
else
    echo "[WARN] pip3 no encontrado. Instala manualmente:"
    echo "  pip3 install -r experiments/requirements.txt"
fi

# ── 5. Resumen ───────────────────────────────────────────────────────────────
log "─────────────────────────────────────────"
log "Setup completo. Ahora puedes compilar con:"
echo "  make debug     # debug + sanitizers"
echo "  make release   # release optimizado"
echo "  make test      # tests unitarios"
