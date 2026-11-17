@echo off
rem Reports which Vulkan features of this PC's graphics cards the renderer needs (and the fallback for each missing one).
rem Writes GpuCheck.txt beside this file: send it with your results.
"%~dp0game-tools\bin\mh4n_vkprobe.exe" > "%~dp0GpuCheck.txt" 2>&1
type "%~dp0GpuCheck.txt"
echo.
echo Saved in GpuCheck.txt
pause
