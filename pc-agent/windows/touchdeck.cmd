@echo off
rem TouchDeck PC agent on demand: touchdeck start | stop | restart | status | settings | log | run
rem No action (a double click too) = start. See touchdeck.ps1 beside this file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0touchdeck.ps1" %*
set rc=%errorlevel%
rem A double click closes the window at once: keep it open when something failed.
if not "%rc%"=="0" if "%~1"=="" pause
exit /b %rc%
