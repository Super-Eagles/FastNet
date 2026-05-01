@echo off
setlocal EnableDelayedExpansion
title FastNet High-Performance Benchmark Suite

:: 设置控制台颜色
color 0B

echo ========================================================
echo        FastNet 极限性能压测脚手架 (High-End PC)
echo ========================================================
echo.
echo 检测到将要进行极限压榨测试...
echo 请确保当前机器已关闭不必要的后台程序。
echo CPU 线程调度：[自动全开模式]
echo.
pause

set BIN_DIR=%~dp0bin
if not exist "%BIN_DIR%\fastnet_tcp_loopback_benchmark.exe" (
    echo [错误] 找不到编译后的可执行文件！请确保你将整个 bin 文件夹和本脚本放在一起。
    pause
    exit /b 1
)

:: 通用高配参数：更长的测试时间，交由 FastNet 去自动嗅探宿主机 CPU 核心数 (--threads 0)
set DURATION=10
set PAYLOAD=512
set THREADS=0

echo.
echo ======================================================== 
echo   [1/4] 启动 UDP 极限测试 (无连接，高吞吐测试) 
echo ======================================================== 
:: 极高的 in-flight 深度，彻底打满网卡的收发中断
"%BIN_DIR%\fastnet_udp_loopback_benchmark.exe" --max-inflight 512 --payload %PAYLOAD% --duration %DURATION% --threads %THREADS%
echo.

echo ======================================================== 
echo   [2/4] 启动 TCP 极限测试 (闭环长连接，流水线全开) 
echo ======================================================== 
:: 大规模客户端并发 (2000)，配合深流水线 (in-flight 64)，规避 Windows 本机环回串行延迟
"%BIN_DIR%\fastnet_tcp_loopback_benchmark.exe" --clients 2000 --max-inflight 64 --payload %PAYLOAD% --duration %DURATION% --threads %THREADS% --rounds 1
echo.

echo ======================================================== 
echo   [3/4] 启动 WebSocket 极限测试 (带 Mask 解析与状态机) 
echo ======================================================== 
:: 中高规模并发 (1000)
"%BIN_DIR%\fastnet_websocket_loopback_benchmark.exe" --clients 1000 --payload %PAYLOAD% --duration %DURATION% --threads %THREADS% --rounds 1 --connect-timeout 30000
echo.

echo ======================================================== 
echo   [4/4] 启动 HTTP/1.1 极限测试 (字符串密集与结构解析) 
echo ======================================================== 
:: 中等规模并发 (500)
"%BIN_DIR%\fastnet_http_loopback_benchmark.exe" --clients 500 --payload %PAYLOAD% --duration %DURATION% --threads %THREADS% --rounds 1
echo.

echo ========================================================
echo 测试全部完成！
echo 查看各项测试结果中的 'throughput' 字段 (QTPS) 即为每秒事务处理量。
echo ========================================================
pause
