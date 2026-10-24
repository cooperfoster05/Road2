@echo off
rem Runs the bot's best checkpoint on the 61 held-out ground shots (the benchmark), next to the engine's result.
cd /d "%~dp0"
set EXE=..\build\trainer\RLCoachTrainer.exe
if not exist "%EXE%" (
  echo Build it first: open trainer\RLCoachTrainer.slnx in Visual Studio, Release x64, Build.
  pause
  exit /b 1
)
"%EXE%" evalbot --lines 32
pause
