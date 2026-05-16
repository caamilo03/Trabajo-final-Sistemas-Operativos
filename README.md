# perfanalyzer

API de análisis de rendimiento para contenedores Docker.  
Proyecto final — Sistemas Operativos, Universidad de Antioquia.

## Descripción

`perfanalyzer` es una librería en C (`libperfanalyzer`) con un servidor HTTP REST que expone métricas de CPU, memoria y E/S de un proceso, tanto desde dentro de un contenedor (vía `/proc`) como desde el host (vía cgroup v2). Incluye un sistema de *profiling* por función instrumentado con macros.

## Requisitos del sistema

| Dependencia      | Versión mínima | Notas                                  |
|------------------|----------------|----------------------------------------|
| Linux            | 5.10+          | cgroup v2 requerido                    |
| gcc              | 11+            |                                        |
| cmake            | 3.20+          |                                        |
| Docker           | 24+            | con cgroup v2 habilitado               |
| Python           | 3.10+          | solo para análisis de experimentos     |
| numpy/scipy/pandas | últimas      | `pip install -r experiments/requirements.txt` |

Verificar cgroup v2:
```bash
stat -fc %T /sys/fs/cgroup   # debe imprimir: cgroup2fs
```

## Compilar

```bash
# Debug (con sanitizers Address + UB)
cmake -B build -DCMAKE_BUILD_TYPE=Debug .
cmake --build build --parallel

# Release
cmake -B build-release -DCMAKE_BUILD_TYPE=Release .
cmake --build build-release --parallel
```

## Ejecutar pruebas

```bash
cd build && ctest --output-on-failure
```

## Ejecutar el servidor

```bash
./build/src/server/perfanalyzer_server --port 8080
```

Endpoints disponibles:

| Método | Ruta                | Descripción                         |
|--------|---------------------|-------------------------------------|
| GET    | `/health`           | Liveness check                      |
| GET    | `/snapshot`         | Muestra puntual CPU/Mem/IO          |
| POST   | `/recording/start`  | Inicia muestreo periódico           |
| POST   | `/recording/stop`   | Detiene y devuelve serie            |
| POST   | `/probe/begin`      | Inicia profiling de función         |
| POST   | `/probe/end`        | Finaliza profiling                  |
| GET    | `/probes`           | Estadísticas agregadas de funciones |

## Docker

```bash
# Baseline (sin límites)
docker compose --profile baseline up --build

# Con límite de CPU
docker compose --profile cpu-limit up --build

# Con límite de memoria
docker compose --profile mem-limit up --build
```

## Reproducir experimentos

```bash
# Ejecuta la matriz completa (host + 3 perfiles Docker, 30 réplicas c/u)
chmod +x experiments/run_experiment.sh
experiments/run_experiment.sh

# Análisis estadístico y figuras
pip install -r experiments/requirements.txt
python experiments/analyze.py
# Abre experiments/results/report.html
```

> **Nota:** los scripts de experimentos requieren permisos para ejecutar
> contenedores Docker y leer `/sys/fs/cgroup`. Ejecutar como usuario en el
> grupo `docker`, no como root.

## Estructura del repositorio

```
src/
  core/        librería libperfanalyzer (samplers, profiler, exportador)
  server/      daemon HTTP REST
  cli/         cliente de línea de comandos perfctl
  workloads/   benchmarks instrumentados (cpu_stress, mem_stress, io_stress)
  vendor/      dependencias embebidas (cJSON, Unity)
docker/        Dockerfile y docker-compose
experiments/   scripts de experimentación y análisis estadístico
tests/         tests unitarios y de integración
docs/informe/  reporte IEEE en LaTeX
```

## Licencia

MIT — ver [LICENSE](LICENSE).
