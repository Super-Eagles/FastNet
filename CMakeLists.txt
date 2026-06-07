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
option(FASTNET_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(FASTNET_INSTALL_CMAKE_PACKAGE "Install FastNet CMake package files" ON)
option(FASTNET_ENABLE_IOURING "Enable Linux io_uring backend (requires liburing)" OFF)
option(FASTNET_NATIVE_ARCH "Enable -march=native CPU-specific optimizations" ON)
option(FASTNET_ENABLE_LTO "Enable Link-Time Optimization (IPO)" ON)

set(FASTNET_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(FASTNET_CMAKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

    foreach(output_config ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${output_config} output_config_upper)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${output_config_upper} ${CMAKE_SOURCE_DIR}/lib)
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${output_config_upper} ${CMAKE_SOURCE_DIR}/lib)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${output_config_upper} ${CMAKE_SOURCE_DIR}/bin)
    endforeach()
endif()

find_package(Threads REQUIRED)

if(FASTNET_ENABLE_SSL)
    find_package(OpenSSL REQUIRED)
endif()

# ── io_uring backend ───────────────────────────────────────────────────────────
if(FASTNET_ENABLE_IOURING)
    find_library(LIBURING_LIB uring REQUIRED)
    find_path(LIBURING_INCLUDE_DIR liburing.h REQUIRED)
    message(STATUS "io_uring backend enabled (liburing: ${LIBURING_LIB})")
endif()

# ── Link-Time Optimization ────────────────────────────────────────────────────
if(FASTNET_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    if(_ipo_ok)
        message(STATUS "LTO (IPO) enabled")
    else()
        message(STATUS "LTO not supported: ${_ipo_msg}")
        set(FASTNET_ENABLE_LTO OFF)
    endif()
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

if(FASTNET_ENABLE_IOURING)
    list(APPEND FASTNET_SOURCES src/IoUringPoller.cpp)
endif()

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

# ── Performance: native arch + visibility + inlining ────────────────────────
# Flags applied to ALL targets (must be ABI-compatible with libFastNet.so):
set(FASTNET_ARCH_FLAGS "")
# Flags applied ONLY to the library (internal symbols can be hidden):
set(FASTNET_LIB_PRIV_FLAGS "")
if(NOT MSVC)
    if(FASTNET_NATIVE_ARCH)
        list(APPEND FASTNET_ARCH_FLAGS -march=native -mtune=native)
    endif()
    # Hidden visibility: only for the library's internal (non-API) symbols.
    # The public API uses FASTNET_API which must remain visible.
    list(APPEND FASTNET_LIB_PRIV_FLAGS -fvisibility=hidden -fvisibility-inlines-hidden)
    # Aggressive inlining and aliasing — safe only within one translation unit.
    list(APPEND FASTNET_LIB_PRIV_FLAGS -finline-functions -fstrict-aliasing)
endif()

target_compile_options(FastNet PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:${FASTNET_MSVC_WARNINGS}>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:${FASTNET_OTHER_WARNINGS}>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:${FASTNET_LIB_PRIV_FLAGS}>
)

target_compile_options(FastNet
    PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

# Apply -march=native to ALL targets (examples, tests) for consistent codegen.
# Visibility flags are NOT propagated here — they are FastNet-internal only.
if(FASTNET_ARCH_FLAGS)
    foreach(_flag IN LISTS FASTNET_ARCH_FLAGS)
        add_compile_options($<$<NOT:$<CXX_COMPILER_ID:MSVC>>:${_flag}>)
    endforeach()
endif()

# ── LTO ──────────────────────────────────────────────────────────────────────
if(FASTNET_ENABLE_LTO AND _ipo_ok)
    set_target_properties(FastNet PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_link_libraries(FastNet
    PUBLIC
        Threads::Threads
)

if(WIN32)
    target_link_libraries(FastNet PRIVATE ws2_32 mswsock)
endif()

# ── io_uring ─────────────────────────────────────────────────────────────────
if(FASTNET_ENABLE_IOURING)
    target_compile_definitions(FastNet PUBLIC FASTNET_ENABLE_IOURING)
    target_include_directories(FastNet PRIVATE ${LIBURING_INCLUDE_DIR})
    target_link_libraries(FastNet PRIVATE ${LIBURING_LIB})
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
