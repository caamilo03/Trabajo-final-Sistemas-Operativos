foreach(wl cpu_stress mem_stress io_stress)
    add_executable(${wl} ${wl}.c)
    target_link_libraries(${wl} PRIVATE perfanalyzer_core pthread m)
    target_include_directories(${wl} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
    )
endforeach()
