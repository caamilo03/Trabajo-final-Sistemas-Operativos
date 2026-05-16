cmake_minimum_required(VERSION 3.20)
project(perfanalyzer VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Flags de compilación ────────────────────────────────────────────────────
set(PERF_WARN_FLAGS
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes
)

set(PERF_HARDEN_FLAGS
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
)

add_compile_options(${PERF_WARN_FLAGS})

# Debug: sanitizers + info de depuración
set(CMAKE_C_FLAGS_DEBUG   "-Og -g3 -fsanitize=address,undefined -fno-omit-frame-pointer")
# Release: optimizado
set(CMAKE_C_FLAGS_RELEASE "-O2 -flto -DNDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ── Directorio de includes ──────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/src/core)
include_directories(${CMAKE_SOURCE_DIR}/src/vendor)

# ── Subdirectorios ──────────────────────────────────────────────────────────
add_subdirectory(src/core)
add_subdirectory(src/server)
add_subdirectory(src/cli)
add_subdirectory(src/workloads)
add_subdirectory(tests)

# ── Instalación ─────────────────────────────────────────────────────────────
install(TARGETS perfanalyzer_server perfctl RUNTIME DESTINATION bin)
install(TARGETS perfanalyzer_core    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES src/core/perf.h        DESTINATION include)
