@echo off
REM Builds server.exe from src\*.cpp (C++20, Winsock2, MySQL C API).
REM Edit MYSQL below if MySQL is installed somewhere else.
set MYSQL=C:\Program Files\MySQL\MySQL Server 8.0

REM -std=c++20  : needed for std::counting_semaphore later
REM -Iinclude   : our own headers live in include\
REM -lws2_32    : Winsock2 (network sockets on Windows)
REM -lbcrypt    : BCryptGenRandom, secure random numbers for session ids
REM -lmysql     : MySQL C API (same flags as the old build.bat)
g++ -std=c++20 -Wall -Wextra -static-libgcc -static-libstdc++ -o server.exe ^
  src\*.cpp ^
  -Iinclude -I"%MYSQL%\include" -L"%MYSQL%\lib" -lmysql -lws2_32 -lbcrypt -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic

if errorlevel 1 (
  echo.
  echo BUILD FAILED
  exit /b 1
)
echo Build OK: server.exe
