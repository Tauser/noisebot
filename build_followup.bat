@echo off
call C:\esp\v5.5.4\esp-idf\export.bat
echo EXPORT_EXIT=%ERRORLEVEL%
cd /d D:\Projetos\Noisebot
idf.py build
echo BUILD_EXIT=%ERRORLEVEL%
