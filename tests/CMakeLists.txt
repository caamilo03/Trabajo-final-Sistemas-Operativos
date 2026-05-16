enable_testing()

# Framework de tests: Unity (un solo .c, embebido)
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)

# Helper: convierte target de test en test de CTest
function(add_perf_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE perfanalyzer_core unity pthread)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/vendor
        ${CMAKE_CURRENT_SOURCE_DIR}/unity
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_perf_test(test_sampler_cpu  test_sampler_cpu.c)
add_perf_test(test_sampler_mem  test_sampler_mem.c)
add_perf_test(test_sampler_io   test_sampler_io.c)
add_perf_test(test_profiler     test_profiler.c)
add_perf_test(test_ring_buffer  test_ring_buffer.c)
add_perf_test(test_exporter     test_exporter.c)
