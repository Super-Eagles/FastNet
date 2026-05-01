set(FASTNET_EXAMPLE_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin)

function(add_fastnet_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE FastNet::FastNet)
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    set_target_properties(${target_name} PROPERTIES
        DEBUG_POSTFIX "d"
        RUNTIME_OUTPUT_DIRECTORY ${FASTNET_EXAMPLE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${FASTNET_EXAMPLE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${FASTNET_EXAMPLE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${FASTNET_EXAMPLE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${FASTNET_EXAMPLE_OUTPUT_DIR}
        VS_GLOBAL_VcpkgEnabled "false"
        FOLDER "examples"
    )
endfunction()

add_fastnet_example(fastnet_tcp_echo_server tcp_echo_server.cpp)
add_fastnet_example(fastnet_tcp_loopback_benchmark tcp_loopback_benchmark.cpp)
add_fastnet_example(fastnet_tcp_connect_burst_benchmark tcp_connect_burst_benchmark.cpp)
add_fastnet_example(fastnet_udp_loopback_benchmark udp_loopback_benchmark.cpp)
add_fastnet_example(fastnet_benchmark_matrix_runner benchmark_matrix_runner.cpp)
add_fastnet_example(fastnet_http_connect_burst_benchmark http_connect_burst_benchmark.cpp)
add_fastnet_example(fastnet_websocket_loopback_benchmark websocket_loopback_benchmark.cpp)
add_fastnet_example(fastnet_websocket_connect_burst_benchmark websocket_connect_burst_benchmark.cpp)
add_fastnet_example(fastnet_http_loopback_benchmark http_loopback_benchmark.cpp)
add_fastnet_example(fastnet_http_get_client http_get_client.cpp)
add_fastnet_example(fastnet_http_static_server http_static_server.cpp)
add_fastnet_example(fastnet_websocket_echo_server websocket_echo_server.cpp)
