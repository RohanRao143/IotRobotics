@echo off
TITLE App Launcher, Rebuilder, and Port Killer

:: ==========================================
:: 1. DEFINE PATHS & SOLUTION FILENAMES
:: ==========================================
set "DIR1=C:\Users\Admin\projects\IotRobotics\bots\VirtualBotApplication\x64\Release"
set "DIR2=C:\Users\Admin\projects\IotRobotics\L1Parser\FrameParser\x64\Release"
set "DIR3=C:\Users\Admin\projects\IotRobotics\L1Parser\Servers"
set "DIR4=C:\Users\Admin\projects\IotRobotics\L1Parser\Servers"

:: Define paths to your .sln or .vcxproj files
set "SLN1=C:\Users\Admin\projects\IotRobotics\bots\VirtualBotApplication\VirtualBotApplication.slnx"

C:\Users\Admin\projects\IotRobotics\bots\VirtualBotApplication
set "SLN2=C:\Users\Admin\projects\IotRobotics\L1Parser\FrameParser\FrameParser.slnx"

:: ==========================================
:: 2. LOCATE MSBUILD UTILITY
:: ==========================================
echo Locating MSBuild...
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -property installationPath`) do (
  set "VS_PATH=%%i"
)

if not exist "%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe" (
    echo [ERROR] MSBuild could not be found. Please ensure Visual Studio is installed.
    pause
    exit /b
)

set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe"
echo Found MSBuild at: "%MSBUILD%"
echo.

:: ==========================================
:: 3. CLEAN AND REBUILD C++ PROJECTS
:: ==========================================
echo ==========================================
echo Cleaning and Rebuilding C++ Solutions...
echo ==========================================

echo [1/2] Rebuilding VirtualBotApplication...
"%MSBUILD%" "%SLN1%" /t:Clean,Build /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to build VirtualBotApplication. Aborting launch.
    pause
    exit /b
)

echo [2/2] Rebuilding FrameParser...
"%MSBUILD%" "%SLN2%" /t:Clean,Build /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to build FrameParser. Aborting launch.
    pause
    exit /b
)

echo Build process completed successfully!
echo.

:: ==========================================
:: 4. KILL PORTS
:: ==========================================
echo Killing existing processes on ports...

for %%p in (5555 6666) do (
    echo Clearing port %%p...
    for /f "tokens=5" %%a in ('netstat -ano ^| findstr :%%p') do (
        taskkill /F /PID %%a 2>nul
    )
)

echo Ports cleared.
timeout /t 2 >nul

:: ==========================================
:: 5. CHANGE DIRECTORIES & RUN APPLICATIONS
:: ==========================================
echo Starting applications in Windows Terminal tabs...

:: Open the first tab and establish the window (-w 0 targets the current/first WT window)
wt -w 0 -p "Command Prompt" --title "L0 Simulation Sensor Data" cmd /k "cd /d %DIR1% && VirtualBotApplication.exe"

:: Wait a brief moment to ensure the window initializes before targeting it with new tabs
timeout /t 1 >nul

:: Open subsequent tabs inside that exact same window
wt -w 0 new-tab -p "Command Prompt" --title "L1 Thread 1 Custom Frame Parser and Network Routing App" cmd /k "cd /d %DIR2% && FrameParser.exe"
wt -w 0 new-tab -p "Command Prompt" --title "L1 Thread 2 UDP Receiver App" cmd /k "cd /d %DIR3% && python udp_receiver.py"
wt -w 0 new-tab -p "Command Prompt" --title "L1 Thread 3 TCP Receiver App" cmd /k "cd /d %DIR4% && python tcp_client.py"

echo All applications triggered.
pause
