add_executable(perfctl
    perfctl.c
)

target_include_directories(perfctl PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/vendor
)

target_link_libraries(perfctl PRIVATE
    perfanalyzer_core
    pthread
)
