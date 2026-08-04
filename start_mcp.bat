@echo off
REM Ark Viewer 2 性能诊断 MCP 服务器启动脚本
REM 需先 pip install -r requirements.txt
cd /d "%~dp0"
python mcp_server.py
