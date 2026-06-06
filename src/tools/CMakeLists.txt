add_executable(container_monitor container_monitor.c)

target_include_directories(container_monitor PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
)

target_link_libraries(container_monitor PRIVATE pthread)

target_compile_options(container_monitor PRIVATE
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
)
