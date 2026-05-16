.PHONY: all debug release test clean docker-baseline docker-cpu docker-mem

all: debug

debug:
	cmake -B build -DCMAKE_BUILD_TYPE=Debug . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	cmake --build build --parallel

release:
	cmake -B build-release -DCMAKE_BUILD_TYPE=Release .
	cmake --build build-release --parallel

test: debug
	cd build && ctest --output-on-failure -j$(nproc)

clean:
	rm -rf build build-release

docker-baseline:
	docker compose -f docker/docker-compose.yml --profile baseline up --build

docker-cpu:
	docker compose -f docker/docker-compose.yml --profile cpu-limit up --build

docker-mem:
	docker compose -f docker/docker-compose.yml --profile mem-limit up --build
