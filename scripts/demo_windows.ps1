# demo_windows.ps1 — Demo de perfanalyzer en Docker desde Windows
#
# Construye la imagen, levanta el servidor con el dashboard, y opcionalmente
# corre los tres workloads mostrando sus resultados.
#
# Uso (desde la raíz del repo, en PowerShell):
#   .\scripts\demo_windows.ps1                 # build + servidor + workloads
#   .\scripts\demo_windows.ps1 -SkipBuild      # reusa la imagen ya construida
#   .\scripts\demo_windows.ps1 -ServerOnly     # solo levanta el servidor
#
# Requisitos: Docker Desktop corriendo.

param(
    [switch]$SkipBuild,
    [switch]$ServerOnly,
    [int]$Port = 8080
)

$ErrorActionPreference = "Stop"
$IMAGE     = "perfanalyzer:demo"
$CONTAINER = "perf_demo"

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

# ── 1. Verificar Docker ──────────────────────────────────────────────────────
Section "Verificando Docker"
docker ps > $null 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Docker no responde. Abre Docker Desktop y reintenta." -ForegroundColor Red
    exit 1
}
Write-Host "Docker OK"

# ── 2. Construir imagen ──────────────────────────────────────────────────────
if (-not $SkipBuild) {
    Section "Construyendo imagen $IMAGE"
    docker build -t $IMAGE -f docker/Dockerfile.app .
    if ($LASTEXITCODE -ne 0) { Write-Host "Build falló" -ForegroundColor Red; exit 1 }
}

# ── 3. Arrancar contenedor ───────────────────────────────────────────────────
Section "Arrancando servidor (contenedor $CONTAINER)"
docker rm -f $CONTAINER 2>&1 | Out-Null
docker run -d --name $CONTAINER -p "${Port}:8080" $IMAGE | Out-Null

# Esperar healthcheck
$ok = $false
for ($i = 0; $i -lt 30; $i++) {
    try {
        $r = Invoke-WebRequest "http://localhost:$Port/health" -UseBasicParsing -TimeoutSec 1
        if ($r.StatusCode -eq 200) { $ok = $true; break }
    } catch {}
    Start-Sleep -Seconds 1
}
if (-not $ok) {
    Write-Host "El servidor no respondió. Revisa: docker logs $CONTAINER" -ForegroundColor Red
    exit 1
}
Write-Host "Servidor arriba en http://localhost:$Port" -ForegroundColor Green
Write-Host "Dashboard en vivo:  http://localhost:$Port/dashboard" -ForegroundColor Yellow

if ($ServerOnly) {
    Write-Host "`nModo --ServerOnly. Abre el dashboard y corre workloads con:"
    Write-Host "  docker exec $CONTAINER ./cpu_stress --duration 20 --intensity 2"
    Write-Host "`nPara detener:  docker rm -f $CONTAINER"
    exit 0
}

# ── 4. Snapshot inicial ──────────────────────────────────────────────────────
Section "Snapshot inicial (/snapshot)"
(Invoke-WebRequest "http://localhost:$Port/snapshot" -UseBasicParsing).Content

# ── 5. Ejecutar los tres workloads ───────────────────────────────────────────
foreach ($wl in @(
    @{name="cpu_stress"; args="--duration 10 --intensity 2"},
    @{name="mem_stress"; args="--duration 8  --intensity 2"},
    @{name="io_stress";  args="--duration 8  --intensity 1"}
)) {
    Section "Workload: $($wl.name) $($wl.args)"
    docker exec $CONTAINER ./$($wl.name) $($wl.args.Split(" "))
}

# ── 6. Resumen ───────────────────────────────────────────────────────────────
Section "Demo completa"
Write-Host "El servidor sigue corriendo. Abre el dashboard en tu navegador:" -ForegroundColor Green
Write-Host "  http://localhost:$Port/dashboard" -ForegroundColor Yellow
Write-Host "`nMientras lo miras, lanza carga y observa las tarjetas cgroup reaccionar:"
Write-Host "  docker exec $CONTAINER ./cpu_stress --duration 30 --intensity 2"
Write-Host "`nPara detener el servidor:"
Write-Host "  docker rm -f $CONTAINER"
