add_executable(perfanalyzer_server
    main.c
    http.c
    routes.c
    dashboard.c
)

target_include_directories(perfanalyzer_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/vendor
)

target_link_libraries(perfanalyzer_server PRIVATE
    perfanalyzer_core
    pthread
)

target_compile_options(perfanalyzer_server PRIVATE
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
)
