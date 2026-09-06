@echo off
rem Start MemOptimizer (local only) - use argument --demo to spawn test processes
setlocal
if not exist bin\memtool.exe (
  call build.bat
)
bin\memtool.exe %*
endlocal