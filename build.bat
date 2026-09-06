@echo off
rem MemOptimizer build script (MinGW-w64 gcc)
setlocal
set "GCC=F:\winlibs_mingw-16.1.0posix-14.0.0-ucrt-r3\mingw64\bin\gcc.exe"
set "TOOLDIR=F:\winlibs_mingw-16.1.0posix-14.0.0-ucrt-r3\mingw64\bin"
set "PATH=%TOOLDIR%;%PATH%"

if not exist "%GCC%" (
  echo [ERROR] gcc not found: %GCC%
  exit /b 1
)
if not exist bin mkdir bin

echo Compiling...
"%GCC%" -O2 -std=c11 -Wall -Wextra -Isource -Iinclude src\util.c src\json.c src\config.c src\snapshot.c src\counters.c src\sampler.c src\runtime.c src\leak.c src\hotspot.c src\fragment.c src\optimize.c src\optimize_oneclick.c src\protect.c src\report.c src\pdf.c src\storage.c src\http_server.c src\routes.c src\main.c -o bin\memtool.exe -lws2_32 -lpsapi
if errorlevel 1 (
  echo [ERROR] build failed
  exit /b 1
)

echo Copying web assets...
if not exist bin\web mkdir bin\web
xcopy /Y /E /Q src\web bin\web\ >nul 2>&1

echo Done: bin\memtool.exe
endlocal