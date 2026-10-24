@echo off
rem Road2 ground-shot bot trial: starts (or picks up) training. Close this window or press Ctrl+C to stop;
rem it saves every 10 minutes and on Ctrl+C, and running this again continues where it left off.
rem Default: 8 hours, all cores but two, below-normal priority. Change the hours like this: StartTraining.bat 10
cd /d "%~dp0"
title Road2 bot training
set EXE=..\build\trainer\RLCoachTrainer.exe
set LOG=..\build\trainer\last_run.log
echo StartTraining.bat ran at %date% %time% in %cd% > "%LOG%"
if not exist "%EXE%" (
  echo Build it first: open trainer\RLCoachTrainer.slnx in Visual Studio, Release x64, Build.
  echo exe not found >> "%LOG%"
  pause
  exit /b 1
)
set HOURS=%1
if "%HOURS%"=="" set HOURS=8
echo Starting the trainer...
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%EXE%' train --hours %HOURS% 2>&1 | Tee-Object -FilePath '%LOG%' -Append; exit $LASTEXITCODE"
echo exit code %errorlevel% >> "%LOG%"
echo.
echo Training stopped (exit code %errorlevel%). Progress is in %%APPDATA%%\bakkesmod\bakkesmod\data\RLCoach\bot (eval_log.csv, status.txt).
pause
