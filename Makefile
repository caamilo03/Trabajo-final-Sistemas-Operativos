.PHONY: all setup debug release test clean docker-baseline docker-cpu docker-mem \
        experiment validate overhead

all: debug

# ── Setup: instala Unity, Python deps, verifica cgroup v2 ──────────────────
setup:
	@chmod +x scripts/setup_deps.sh
	@./scripts/setup_deps.sh

# ── Builds ─────────────────────────────────────────────────────────────────
debug:
	cmake -B build -DCMAKE_BUILD_TYPE=Debug . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	cmake --build build --parallel

release:
	cmake -B build-release -DCMAKE_BUILD_TYPE=Release .
	cmake --build build-release --parallel

test: debug
	cd build && ctest --output-on-failure -j$$(nproc)

clean:
	rm -rf build build-release

# ── Docker ─────────────────────────────────────────────────────────────────
docker-baseline:
	docker compose -f docker/docker-compose.yml --profile baseline up --build

docker-cpu:
	docker compose -f docker/docker-compose.yml --profile cpu-limit up --build

docker-mem:
	docker compose -f docker/docker-compose.yml --profile mem-limit up --build

# ── Experimentos ───────────────────────────────────────────────────────────
experiment: release
	@chmod +x experiments/run_experiment.sh
	@experiments/run_experiment.sh

validate: release
	@chmod +x experiments/validate.sh
	@experiments/validate.sh

overhead: release
	@chmod +x experiments/measure_overhead.sh
	@experiments/measure_overhead.sh
