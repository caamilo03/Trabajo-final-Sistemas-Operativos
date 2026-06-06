# Construimos cada workload en dos variantes:
#   - <wl>           : instrumentado con PERF_PROBE
#   - <wl>_nosampler : misma fuente con -DPERF_DISABLE_SAMPLING
#                       (PERF_PROBE se vuelve no-op, para medir overhead)

foreach(wl cpu_stress mem_stress io_stress)
    # Variante instrumentada
    add_executable(${wl} ${wl}.c)
    target_link_libraries(${wl} PRIVATE perfanalyzer_core pthread m)
    target_include_directories(${wl} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
    )

    # Variante sin sampler (baseline de overhead)
    add_executable(${wl}_nosampler ${wl}.c)
    target_compile_definitions(${wl}_nosampler PRIVATE PERF_DISABLE_SAMPLING)
    target_link_libraries(${wl}_nosampler PRIVATE perfanalyzer_core pthread m)
    target_include_directories(${wl}_nosampler PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
    )
endforeach()
