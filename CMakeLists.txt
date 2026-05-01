# @file CMakeLists.txt
# @brief FastNet top-level build configuration

cmake_minimum_required(VERSION 3.15)
project(FastNet VERSION 1.4.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FASTNET_BUILD_EXAMPLES "Build FastNet example programs" ON)
option(FASTNET_BUILD_TESTS "Build FastNet regression tests" ON)
option(FASTNET_ENABLE_SSL "Enable TLS support through OpenSSL" OFF)
option(FASTNET_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(FASTNET_INSTALL_CMAKE_PACKAGE "Install FastNet CMake package files" ON)

set(FASTNET_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(FASTNET_CMAKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

foreach(output_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${output_config} output_config_upper)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${output_config_upper} ${CMAKE_SOURCE_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${output_config_upper} ${CMAKE_SOURCE_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${output_config_upper} ${CMAKE_SOURCE_DIR}/bin)
endforeach()

find_package(Threads REQUIRED)

if(FASTNET_ENABLE_SSL)
    find_package(OpenSSL REQUIRED)
endif()

set(FASTNET_SOURCES
    src/Configuration.cpp
    src/ConnectionManager.cpp
    src/Error.cpp
    src/EventPoller.cpp
    src/FastNet.cpp
    src/HttpClient.cpp
    src/HttpParser.cpp
    src/HttpServer.cpp
    src/IoService.cpp
    src/Logger.cpp
    src/PerformanceMonitor.cpp
    src/SocketWrapper.cpp
    src/SSLContext.cpp
    src/TcpClient.cpp
    src/TcpConnectionPool.cpp
    src/TcpServer.cpp
    src/Timer.cpp
    src/UdpSocket.cpp
    src/WindowsIocpTransport.cpp
    src/WebSocketClient.cpp
    src/WebSocketProtocol.cpp
    src/WebSocketServer.cpp
    src/base64.cpp
    src/BenchmarkUtils.cpp
)

add_library(FastNet SHARED ${FASTNET_SOURCES})
add_library(FastNet::FastNet ALIAS FastNet)

set_target_properties(FastNet PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "FastNet"
    DEBUG_POSTFIX "d"
    PUBLIC_HEADER "${FASTNET_INCLUDE_DIR}/FastNet/FastNet.h"
    VS_GLOBAL_VcpkgEnabled "false"
)

target_compile_features(FastNet PUBLIC cxx_std_17)

target_include_directories(FastNet
    PUBLIC
        $<BUILD_INTERFACE:${FASTNET_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${FASTNET_INCLUDE_DIR}/FastNet
)

target_compile_definitions(FastNet
    PUBLIC
        FASTNET_VERSION_STRING="\"${PROJECT_VERSION}\""
)

if(WIN32)
    target_compile_definitions(FastNet PRIVATE FASTNET_EXPORTS)
endif()

set(FASTNET_MSVC_WARNINGS
    /W4
    /utf-8
    /wd4251
    /wd4275
    /wd4324
    /wd4244
    /wd4717
    /wd4702
    /wd4828
)
if(FASTNET_WARNINGS_AS_ERRORS)
    list(APPEND FASTNET_MSVC_WARNINGS /WX)
endif()

set(FASTNET_OTHER_WARNINGS
    -Wall
    -Wextra
    -pedantic
)
if(FASTNET_WARNINGS_AS_ERRORS)
    list(APPEND FASTNET_OTHER_WARNINGS -Werror)
endif()

target_compile_options(FastNet PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:${FASTNET_MSVC_WARNINGS}>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:${FASTNET_OTHER_WARNINGS}>
)

target_compile_options(FastNet
    PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(FastNet
    PUBLIC
        Threads::Threads
)

if(WIN32)
    target_link_libraries(FastNet PRIVATE ws2_32 mswsock)
endif()

if(FASTNET_ENABLE_SSL)
    target_compile_definitions(FastNet PUBLIC FASTNET_ENABLE_SSL)
    target_link_libraries(FastNet PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(FASTNET_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt)
    add_subdirectory(examples)
endif()

if(FASTNET_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
    include(CTest)
    add_subdirectory(test)
endif()

install(TARGETS FastNet
    EXPORT FastNetTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/FastNet
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/FastNet/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/FastNet
    FILES_MATCHING PATTERN "*.h"
)

install(EXPORT FastNetTargets
    FILE FastNetTargets.cmake
    NAMESPACE FastNet::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FastNet
)

if(FASTNET_INSTALL_CMAKE_PACKAGE)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/FastNetConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    configure_package_config_file(
        ${FASTNET_CMAKE_DIR}/FastNetConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/FastNetConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FastNet
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/FastNetConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FastNetConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FastNet
    )
endif()
