# perfanalyzer

**API en C para el análisis de rendimiento y la caracterización de recursos de contenedores Docker.**
Proyecto final — Sistemas Operativos, Universidad de Antioquia.

Autores: **Jose Camilo Loaiza Hoyos** ([camilo.loaiza1@udea.edu.co](mailto:camilo.loaiza1@udea.edu.co)) · **Miguel Angel Serna Montoya** ([mangel.serna@udea.edu.co](mailto:mangel.serna@udea.edu.co)) — Departamento de Ingeniería de Sistemas.

---

## Descripción

`perfanalyzer` mide el uso de **CPU, memoria y E/S** de procesos y contenedores Docker, empleando únicamente las interfaces del kernel de Linux (`/proc` y *control groups* v2), sin dependencias de alto nivel. Lo distintivo frente a herramientas como `docker stats`:

- **Medición dual:** desde **dentro** del contenedor (autoinspección vía `/proc`) y desde el **host** (lectura directa del cgroup, sin instrumentar el contenedor).
- **Profiling por función:** instrumentación con la macro `PERF_PROBE` que reporta latencia media y percentiles (p50/p95/p99) por función.
- **Servidor REST + tablero en vivo** y un cliente CLI.

**Caso de uso principal:** caracterizar el perfil de recursos de una aplicación *antes* de desplegarla, para dimensionar correctamente los límites del contenedor (`--cpus`, `--memory`).

---

## Requisitos

| Dependencia | Versión mínima | Notas |
|---|---|---|
| Linux | 5.10+ | **cgroup v2 requerido** |
| gcc | 11+ | |
| cmake | 3.20+ | |
| make | — | |
| Docker | 24+ | para las pruebas en contenedor |
| Python | 3.10+ | solo para el análisis de experimentos |

> **Importante:** `perfanalyzer` es 100% Linux (usa `/proc`, cgroups y pthreads). No compila nativo en Windows ni macOS; en esos sistemas se usa dentro de contenedores Docker.

Verificar cgroup v2:
```bash
stat -fc %T /sys/fs/cgroup   # debe imprimir: cgroup2fs
```

---

## Inicio rápido

```bash
make setup      # descarga Unity (tests), verifica cgroup v2, instala deps de Python
make release    # compila la versión optimizada
make test       # compila debug (ASan/UBSan) y corre las pruebas unitarias
```

Los binarios quedan en `build-release/`:

```
build-release/src/server/perfanalyzer_server
build-release/src/cli/perfctl
build-release/src/tools/container_monitor
build-release/src/workloads/{cpu,mem,io}_stress
```

---

## Componentes

| Componente | Descripción |
|---|---|
| `libperfanalyzer` | Librería estática: samplers (`/proc` y cgroup), profiler, ring buffer y exportador JSON. |
| `perfanalyzer_server` | Daemon HTTP/REST que expone las métricas y un tablero web en vivo. |
| `perfctl` | Cliente de línea de comandos para el servidor (o muestreo directo). |
| `container_monitor` | Monitorea un contenedor desde el host vía cgroup v2, sin tocarlo. |
| `cpu_stress` / `mem_stress` / `io_stress` | Cargas de trabajo instrumentadas para pruebas y experimentos. |

---

## Servidor REST y tablero

```bash
./build-release/src/server/perfanalyzer_server --port 8080
# Opciones: --port <n> · --period-ms <n> (muestreo, def. 100) · --source proc|cgroup|auto
```

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/health` | Verificación de vida |
| GET | `/snapshot` | Muestra puntual (CPU/Mem/E/S + cgroup) en JSON |
| POST | `/recording/start` | Inicia muestreo periódico en segundo plano |
| POST | `/recording/stop` | Detiene y devuelve la serie temporal |
| GET | `/probes` | Estadísticas de profiling por función |
| POST | `/probe/begin` | Inicia una sonda manual — cuerpo: `{"name":"..."}` |
| POST | `/probe/end` | Finaliza la sonda manual |
| POST | `/probe/reset` | Reinicia los contadores de sondas |
| GET | `/dashboard` | **Tablero HTML en vivo** (abrir en el navegador) |

**Tablero en vivo:** abre `http://localhost:8080/dashboard` — tarjetas de CPU/memoria/E/S y tabla de profiling que se actualizan cada segundo.

Ejemplos:
```bash
curl http://localhost:8080/health
curl http://localhost:8080/snapshot
curl -X POST http://localhost:8080/recording/start
curl -X POST http://localhost:8080/recording/stop
curl http://localhost:8080/probes
```

---

## Cliente CLI (`perfctl`)

```bash
./perfctl health
./perfctl snapshot
./perfctl record start
./perfctl record stop
./perfctl probes
./perfctl probe reset
./perfctl local snapshot          # muestra directa, sin servidor
./perfctl --host 127.0.0.1 --port 8081 snapshot
```

---

## Monitorear un contenedor desde el host (`container_monitor`)

Monitorea cualquier contenedor Docker por su nombre, leyendo su cgroup directamente, **sin instalar nada dentro de él** y sin importar su lenguaje. Debe ejecutarse en la **misma máquina Linux** donde corre el contenedor (el cgroup es local al kernel).

```bash
docker ps   # obtener el nombre del contenedor

./build-release/src/tools/container_monitor --name <contenedor> --interval 500

# Guardar a CSV durante 30 s:
./build-release/src/tools/container_monitor \
    --name <contenedor> --interval 500 --duration 30 \
    --output csv --out-file metrics.csv

# Opciones: --name (req.) · --interval <ms> · --duration <s> (-1=infinito)
#           --output human|csv|json · --out-file <ruta> · --cgroup-root <ruta>
```

---

## Cargas de trabajo

```bash
./build-release/src/workloads/cpu_stress --duration 10 --intensity 2
./build-release/src/workloads/mem_stress --duration 8  --intensity 1
./build-release/src/workloads/io_stress  --duration 8  --intensity 1

# Opciones: --duration <s> · --intensity 1|2 · --no-sampler (desactiva el muestreo)
```

El flag `--no-sampler` desactiva la instrumentación en tiempo de ejecución; se usa para medir su sobrecosto comparando el mismo binario consigo mismo.

---

## Usar la librería en tu propio código (C)

```c
#include "perf.h"

perf_config_t cfg = PERF_CONFIG_DEFAULT;
perf_ctx_t *ctx = perf_init(&cfg);
perf_start_recording(ctx);

PERF_PROBE(ctx, "mi_funcion") {
    mi_funcion();              // se cronometra automáticamente
}

perf_series_t serie;
perf_stop_recording(ctx, &serie);
perf_series_free(&serie);
perf_shutdown(ctx);
```

La API pública está documentada en [`src/core/perf.h`](src/core/perf.h).

---

## Docker

```bash
# Perfiles disponibles (distintos límites de recursos):
docker compose -f docker/docker-compose.yml --profile baseline  up --build   # sin límites
docker compose -f docker/docker-compose.yml --profile cpu-limit up --build    # --cpus=1
docker compose -f docker/docker-compose.yml --profile mem-limit up --build    # --memory=512m

# Lanzar una carga dentro del contenedor:
docker exec perf_baseline ./cpu_stress --duration 20 --intensity 2
```

En Windows, el script [`scripts/demo_windows.ps1`](scripts/demo_windows.ps1) automatiza la demo completa con Docker Desktop.

---

## Experimentos y análisis estadístico

```bash
# 1. Experimento principal: host vs. 3 perfiles Docker × 3 cargas × 2 intensidades, n=30
experiments/run_experiment.sh                 # ~2-3 h, genera results/raw_results.csv

# 2. Sobrecosto del sampler (mismo binario con/sin --no-sampler)
experiments/measure_overhead.sh               # ~30 min

# 3. Validación cruzada: perfanalyzer vs container_monitor vs docker stats
experiments/validate.sh --duration 30

# Análisis estadístico y figuras
pip install -r experiments/requirements.txt
python experiments/analyze.py                 # genera results/report.html
```

> Los scripts requieren permisos para ejecutar contenedores Docker y leer `/sys/fs/cgroup`. Ejecutar como usuario del grupo `docker`, no como root.

### Resultados principales (campaña con n=30, 720 corridas)

- **Contenerizar sin límites ≈ host** (CPU, memoria y throughput indistinguibles, *p* > 0.9).
- **Límite de CPU** (`--cpus=1`): reduce el throughput hasta **17%** y eleva la latencia p99 **37%** (throttling del CFS).
- **Límite de memoria**: dispara la latencia p99 de E/S un **261%** (menor caché de páginas).
- **Validación:** las dos vías de medición de la API coinciden al **0.07%**.
- **Sobrecosto del sampler:** ~7% en CPU, despreciable en memoria, ~33% en E/S de grano fino.

---

## Estructura del repositorio

```
src/
  core/        libperfanalyzer: samplers, profiler, ring buffer, exportador JSON
  server/      daemon HTTP/REST + tablero (dashboard.c)
  cli/         cliente perfctl
  tools/       container_monitor (monitoreo desde el host)
  workloads/   cargas instrumentadas (cpu/mem/io_stress)
docker/        Dockerfile multi-stage y docker-compose (3 perfiles)
experiments/   scripts de experimentación, validación y análisis (Python)
tests/         pruebas unitarias (framework Unity)
scripts/       setup de dependencias y demo para Windows
docs/informe/  reporte IEEE en LaTeX
```

---

## Calidad

- Compilación con banderas estrictas: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (0 advertencias).
- Análisis dinámico con *sanitizers* de direcciones y comportamiento indefinido (ASan/UBSan) en modo debug.
- Pruebas unitarias automatizadas con CTest (6/6).
- Sin estado global mutable en la librería; concurrencia protegida con *mutex*.

---

## Licencia

Proyecto académico de uso educativo — Universidad de Antioquia.
