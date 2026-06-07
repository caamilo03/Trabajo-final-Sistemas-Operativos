# Cada workload se compila una sola vez. El sampling se activa/desactiva en
# runtime con --no-sampler (cfg.disable_sampling), de modo que medir el
# overhead compara el MISMO binario consigo mismo, sin artefactos de
# compilación (alineación de loops, vectorización, code layout).

foreach(wl cpu_stress mem_stress io_stress)
    add_executable(${wl} ${wl}.c)
    target_link_libraries(${wl} PRIVATE perfanalyzer_core pthread m)
    target_include_directories(${wl} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
    )
endforeach()
