@echo off
rem Build Alone on Windows. Needs CMake and Visual Studio 2019+ (Desktop C++)
rem or MinGW. raylib is downloaded automatically.
cd /d "%~dp0\.."
cmake -S . -B build || goto :error
cmake --build build --config Release || goto :error
ctest --test-dir build -C Release --output-on-failure
echo.
echo Done. Run build\Release\alone.exe
goto :eof
:error
echo Build failed.
exit /b 1
