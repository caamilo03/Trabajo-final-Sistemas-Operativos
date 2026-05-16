add_library(perfanalyzer_core STATIC
    perf.c
    sampler_cpu.c
    sampler_mem.c
    sampler_io.c
    sampler_cgroup.c
    profiler.c
    ring_buffer.c
    exporter_json.c
)

target_include_directories(perfanalyzer_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/vendor
)

target_compile_options(perfanalyzer_core PRIVATE
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
)

target_link_libraries(perfanalyzer_core PRIVATE pthread)
