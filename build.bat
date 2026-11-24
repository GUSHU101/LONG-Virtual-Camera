@echo off
setlocal EnableExtensions
call "%~dp0engines\camreplace\build.bat"
exit /b %ERRORLEVEL%
