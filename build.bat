@echo off
setlocal enabledelayedexpansion

set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "BUILD_DIR=%PROJECT_DIR%\build"
set "CMAKE_GENERATOR="
set "FASTNET_ENABLE_SSL=OFF"
set "FASTNET_BUILD_EXAMPLES=ON"
set "FASTNET_BUILD_TESTS=ON"
set "BUILD_RELEASE=1"
set "BUILD_DEBUG=1"
set "CLEAN_FIRST=0"
set "EXTRA_CMAKE_ARGS="

:parse_args
if "%~1"=="" goto after_parse
if /I "%~1"=="--help" goto show_help
if /I "%~1"=="--clean" (
    set "CLEAN_FIRST=1"
    shift
    goto parse_args
)
if /I "%~1"=="--ssl" (
    set "FASTNET_ENABLE_SSL=ON"
    shift
    goto parse_args
)
if /I "%~1"=="--no-examples" (
    set "FASTNET_BUILD_EXAMPLES=OFF"
    shift
    goto parse_args
)
if /I "%~1"=="--no-tests" (
    set "FASTNET_BUILD_TESTS=OFF"
    shift
    goto parse_args
)
if /I "%~1"=="--release-only" (
    set "BUILD_RELEASE=1"
    set "BUILD_DEBUG=0"
    shift
    goto parse_args
)
if /I "%~1"=="--debug-only" (
    set "BUILD_RELEASE=0"
    set "BUILD_DEBUG=1"
    shift
    goto parse_args
)
 if /I "%~1"=="--build-dir" (
     shift
     if "%~1"=="" goto invalid_args
     set "BUILD_DIR=%~1"
     if "!BUILD_DIR:~-1!"=="\" set "BUILD_DIR=!BUILD_DIR:~0,-1!"
     shift
     goto parse_args
 )
if /I "%~1"=="--generator" (
    shift
    if "%~1"=="" goto invalid_args
    set "CMAKE_GENERATOR=%~1"
    shift
    goto parse_args
)

set "EXTRA_CMAKE_ARGS=!EXTRA_CMAKE_ARGS! %~1"
shift
goto parse_args

:after_parse
echo ========================================
echo   FastNet Network Library Build Script
echo ========================================
echo Project dir : %PROJECT_DIR%
echo Build dir   : %BUILD_DIR%
echo SSL         : %FASTNET_ENABLE_SSL%
echo Examples    : %FASTNET_BUILD_EXAMPLES%
echo Tests       : %FASTNET_BUILD_TESTS%

if not defined CMAKE_GENERATOR (
    call :detect_visual_studio
    if errorlevel 1 exit /b 1
)

if "%CLEAN_FIRST%"=="1" if exist "%BUILD_DIR%" (
    echo [1/5] Removing existing build directory...
    rmdir /s /q "%BUILD_DIR%"
)

if not exist "%BUILD_DIR%" (
    echo [2/5] Creating build directory...
    mkdir "%BUILD_DIR%"
)

echo [3/5] Configuring CMake...
cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G "%CMAKE_GENERATOR%" -A x64 -DFASTNET_ENABLE_SSL=%FASTNET_ENABLE_SSL% -DFASTNET_BUILD_EXAMPLES=%FASTNET_BUILD_EXAMPLES% -DFASTNET_BUILD_TESTS=%FASTNET_BUILD_TESTS% %EXTRA_CMAKE_ARGS%
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

if "%BUILD_RELEASE%"=="1" (
    echo [4/5] Building Release...
    cmake --build "%BUILD_DIR%" --config Release
    if errorlevel 1 (
        echo [ERROR] Release build failed.
        exit /b 1
    )
)

if "%BUILD_DEBUG%"=="1" (
    echo [5/5] Building Debug...
    cmake --build "%BUILD_DIR%" --config Debug
    if errorlevel 1 (
        echo [ERROR] Debug build failed.
        exit /b 1
    )
)

echo ========================================
echo   Build completed successfully
echo ========================================
echo Generator   : %CMAKE_GENERATOR%
echo Build dir   : %BUILD_DIR%
echo Library dir : %PROJECT_DIR%\lib
echo Runtime dir : %PROJECT_DIR%\bin
exit /b 0

:detect_visual_studio
echo [0/5] Detecting Visual Studio toolchain...

call :try_vs "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" "Visual Studio 17 2022"
if defined CMAKE_GENERATOR exit /b 0
call :try_vs "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" "Visual Studio 17 2022"
if defined CMAKE_GENERATOR exit /b 0
call :try_vs "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" "Visual Studio 17 2022"
if defined CMAKE_GENERATOR exit /b 0
call :try_vs "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" "Visual Studio 17 2022"
if defined CMAKE_GENERATOR exit /b 0

call :try_vs "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" "Visual Studio 16 2019"
if defined CMAKE_GENERATOR exit /b 0
call :try_vs "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" "Visual Studio 16 2019"
if defined CMAKE_GENERATOR exit /b 0
call :try_vs "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvarsall.bat" "Visual Studio 16 2019"
if defined CMAKE_GENERATOR exit /b 0
call :try_vs "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" "Visual Studio 16 2019"
if defined CMAKE_GENERATOR exit /b 0

echo [ERROR] Visual Studio 2019/2022 toolchain not found.
echo         You can also pass --generator "Visual Studio 17 2022" manually.
exit /b 1

:try_vs
set "VS_DEV_CMD=%~1"
set "VS_GENERATOR=%~2"
if not exist "%VS_DEV_CMD%" exit /b 0
echo Found "%VS_DEV_CMD%"
call "%VS_DEV_CMD%" x64 >nul
if errorlevel 1 exit /b 0
set "CMAKE_GENERATOR=%VS_GENERATOR%"
exit /b 0

:invalid_args
echo [ERROR] Missing value for previous argument.
exit /b 1

:show_help
echo Usage: build.bat [options] [extra-cmake-args]
echo.
echo Options:
echo   --clean         Remove the build directory before configuring
echo   --ssl           Enable FASTNET_ENABLE_SSL
echo   --no-examples   Disable example targets
echo   --no-tests      Disable test targets
echo   --release-only  Build Release only
echo   --debug-only    Build Debug only
echo   --build-dir DIR Override build directory
echo   --generator GEN Override the CMake generator
echo   --help          Show this help
echo.
echo Extra arguments are forwarded directly to CMake configure.
exit /b 0
