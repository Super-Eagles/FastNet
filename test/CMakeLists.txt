set(FASTNET_TEST_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin)

function(add_fastnet_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE FastNet::FastNet)
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    set_target_properties(${target_name} PROPERTIES
        DEBUG_POSTFIX "d"
        RUNTIME_OUTPUT_DIRECTORY ${FASTNET_TEST_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${FASTNET_TEST_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${FASTNET_TEST_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${FASTNET_TEST_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${FASTNET_TEST_OUTPUT_DIR}
        VS_GLOBAL_VcpkgEnabled "false"
        FOLDER "tests"
    )
    add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
endfunction()

add_fastnet_test(fastnet_test_base64 test_base64.cpp)
add_fastnet_test(fastnet_test_configuration test_configuration.cpp)
add_fastnet_test(fastnet_test_connection_manager test_connection_manager.cpp)
add_fastnet_test(fastnet_test_error_model test_error_model.cpp)
add_fastnet_test(fastnet_test_http_parser test_http_parser.cpp)
add_fastnet_test(fastnet_test_http_loopback test_http_loopback.cpp)
add_fastnet_test(fastnet_test_network_core test_network_core.cpp)
add_fastnet_test(fastnet_test_runtime_components test_runtime_components.cpp)
add_fastnet_test(fastnet_test_tcp_transport test_tcp_transport.cpp)
add_fastnet_test(fastnet_test_udp_socket test_udp_socket.cpp)
add_fastnet_test(fastnet_test_websocket_loopback test_websocket_loopback.cpp)
add_fastnet_test(fastnet_test_websocket_protocol test_websocket_protocol.cpp)

if(FASTNET_ENABLE_SSL)
    add_fastnet_test(fastnet_test_tls_transport test_tls_transport.cpp)
endif()
