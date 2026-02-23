@echo off
setlocal
cd /d "%~dp0"

echo 正在启动本地服务...
start "RPS Local Server" cmd /k node "rps_local_server.js"
timeout /t 1 /nobreak >nul
start "" "http://127.0.0.1:5500/camera_rps_always_win.html"
