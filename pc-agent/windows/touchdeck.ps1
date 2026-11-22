# CompaDeck PC agent on demand (Windows). It runs in the background with no
# window (pythonw) and logs to %LOCALAPPDATA%\TouchDeck\agent.log, which the
# settings page shows under Logs. Run it through touchdeck.cmd beside it.
#
#   touchdeck start [--host 192.168.1.225]   start it; nothing if it already runs
#   touchdeck stop                           stop it, however it was started
#   touchdeck restart [--host ...]
#   touchdeck status
#   touchdeck settings                       open the settings page (starts the agent if needed)
#   touchdeck log                            follow the log (Ctrl+C to leave)
#   touchdeck run [--verbose]                run it in this console instead (Ctrl+C stops)
#   touchdeck shortcuts                      one .lnk per action in windows\shortcuts, for
#                                            launchers that take no arguments (Stream Deck)
#
# No action = start. Anything after the action goes to touchdeck_agent.py.
# Keep this file ASCII: Windows PowerShell 5.1 reads it in the ANSI code page.

$ErrorActionPreference = "Stop"
$Action = if ($args.Count) { "$($args[0])".ToLower() } else { "start" }
$AgentArgs = @($args | Select-Object -Skip 1)

$Here = Split-Path -Parent $PSScriptRoot
$Agent = Join-Path $Here "touchdeck_agent.py"
$PyW = Join-Path $Here ".venv\Scripts\pythonw.exe"
$Py = Join-Path $Here ".venv\Scripts\python.exe"
$Log = Join-Path $env:LOCALAPPDATA "TouchDeck\agent.log"
$Port = if ($env:TOUCHDECK_PORTAL_PORT) { [int]$env:TOUCHDECK_PORTAL_PORT } else { 4213 }
$Url = "http://127.0.0.1:$Port/"

function Get-AgentProcess {
    # Every running agent, however it was started. A Claude Code hook or a
    # "settings" / "pair" call is a short run of the same script, not one.
    # A venv's python.exe is a launcher, so one agent shows as two processes.
    @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" |
        Where-Object {
            $_.CommandLine -match 'touchdeck_agent\.py' -and
            $_.CommandLine -notmatch 'touchdeck_agent\.py"?\s+(hook|pair|settings)\b' -and
            $_.CommandLine -notmatch '--once'
        })
}

function Test-Portal {
    $c = New-Object Net.Sockets.TcpClient
    try { $c.Connect("127.0.0.1", $Port); $true } catch { $false } finally { $c.Close() }
}

function Assert-Venv {
    if (-not (Test-Path $PyW)) {
        Write-Host "No Python environment in $Here\.venv: set it up first (README.md, Setup)."
        exit 1
    }
}

function Start-Agent {
    $running = Get-AgentProcess
    if ($running.Count) {
        Write-Host "Already running (PID $($running.ProcessId -join ', ')). Settings: $Url"
        return
    }
    Assert-Venv
    New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null
    # A fresh log per start, the previous one kept beside it.
    if (Test-Path $Log) { Move-Item -Force $Log "$Log.1" -ErrorAction SilentlyContinue }
    $argv = @("`"$Agent`"", "--log", "`"$Log`"") + $AgentArgs
    Start-Process -FilePath $PyW -ArgumentList $argv -WorkingDirectory $Here -WindowStyle Hidden
    for ($i = 0; $i -lt 20 -and -not (Test-Portal); $i++) { Start-Sleep -Milliseconds 500 }
    if (Test-Portal) {
        Write-Host "Running. Settings: $Url"
    } elseif ((Get-AgentProcess).Count) {
        Write-Host "Running, but the settings page does not answer yet. Log: $Log"
    } else {
        Write-Host "The agent stopped at once. End of its log ($Log):"
        Get-Content $Log -Tail 20 -Encoding UTF8 -ErrorAction SilentlyContinue
        Write-Host "Nothing there? 'touchdeck run' shows the error in this console."
        exit 1
    }
}

function Stop-Agent {
    $running = Get-AgentProcess
    if (-not $running.Count) { Write-Host "Not running."; return }
    foreach ($p in $running) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
    for ($i = 0; $i -lt 10 -and (Get-AgentProcess).Count; $i++) { Start-Sleep -Milliseconds 300 }
    Write-Host "Stopped."
}

switch ($Action) {
    "start" { Start-Agent }
    "stop" { Stop-Agent }
    "restart" { Stop-Agent; Start-Agent }
    "status" {
        $running = Get-AgentProcess
        if ($running.Count) {
            Write-Host "Running (PID $($running.ProcessId -join ', '))."
            if (Test-Portal) { Write-Host "Settings: $Url" } else { Write-Host "Settings page: not answering" }
        } else {
            Write-Host "Not running."
        }
        Write-Host "Log: $Log"
    }
    "settings" {
        if (-not (Test-Portal)) { Start-Agent }
        if (Test-Portal) { Start-Process $Url; Write-Host $Url } else { exit 1 }
    }
    "shortcuts" {
        # Stream Deck's Open runs a file but passes no arguments: a shortcut
        # carries them. PowerShell runs hidden and the shortcut minimized, so
        # no console window shows. Machine paths inside: not in the repo.
        $dir = Join-Path $PSScriptRoot "shortcuts"
        New-Item -ItemType Directory -Force $dir | Out-Null
        $shell = New-Object -ComObject WScript.Shell
        $ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
        foreach ($a in "start", "stop", "restart", "settings") {
            $path = Join-Path $dir "TouchDeck $a.lnk"
            $lnk = $shell.CreateShortcut($path)
            $lnk.TargetPath = $ps
            $lnk.Arguments = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$PSCommandPath`" $a"
            $lnk.WorkingDirectory = $Here
            $lnk.WindowStyle = 7
            $lnk.Description = "CompaDeck agent: $a"
            $lnk.Save()
            Write-Host $path
        }
    }
    "log" {
        if (-not (Test-Path $Log)) { Write-Host "No log yet ($Log)."; exit 1 }
        Get-Content $Log -Tail 40 -Wait -Encoding UTF8
    }
    "run" {
        Assert-Venv
        if ((Get-AgentProcess).Count) { Write-Host "Already running in the background: 'touchdeck stop' first."; exit 1 }
        & $Py $Agent @AgentArgs
    }
    default {
        Write-Host "Unknown action '$Action'. Use: start, stop, restart, status, settings, log, run, shortcuts."
        exit 2
    }
}
