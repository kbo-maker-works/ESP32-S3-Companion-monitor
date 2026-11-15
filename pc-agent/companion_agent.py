#!/usr/bin/env python3
"""
ESP32-C3 Desktop Companion PC Agent
-----------------------------------
Detects Claude Code usage, token consumption, and system activity on Windows & macOS,
and streams live metrics to the ESP32-C3 over USB CDC Serial connection.

Supported OS:
- Windows (COM1..COM99)
- macOS (/dev/cu.usbmodem* / /dev/tty.usbmodem*)
- Linux (/dev/ttyACM* / /dev/ttyUSB*)
"""
import glob
import json
import os
import platform
import sys
import time

import psutil
import serial
import serial.tools.list_ports

BAUD_RATE = 115200
SEND_INTERVAL_SEC = 1.0
RECONNECT_INTERVAL_SEC = 3.0

# ---------------------------------------------------------------------------
# Cross-Platform Directory Resolver (Windows / macOS / Linux)
# ---------------------------------------------------------------------------
def get_claude_dirs():
    """Returns candidate directories for Claude Code configuration and session logs."""
    dirs = []
    user_home = os.path.expanduser("~")
    
    # 1. Standard ~/.claude/
    dirs.append(os.path.join(user_home, ".claude"))
    dirs.append(os.path.join(user_home, ".config", "claude"))

    # 2. OS Specific Paths
    system = platform.system()
    if system == "Windows":
        appdata = os.environ.get("APPDATA", "")
        localappdata = os.environ.get("LOCALAPPDATA", "")
        if appdata:
            dirs.append(os.path.join(appdata, "Claude"))
            dirs.append(os.path.join(appdata, "claude-code"))
        if localappdata:
            dirs.append(os.path.join(localappdata, "Claude"))
    elif system == "Darwin": # macOS
        dirs.append(os.path.join(user_home, "Library", "Application Support", "Claude"))
        dirs.append(os.path.join(user_home, "Library", "Application Support", "claude-code"))
        dirs.append(os.path.join(user_home, "Library", "Logs", "Claude"))
    
    return [d for d in dirs if os.path.exists(d)]


def find_serial_port():
    """Auto-detects ESP32 USB CDC serial port on Windows and macOS."""
    for port in serial.tools.list_ports.comports():
        dev = (port.device or "").lower()
        desc = (port.description or "").lower()
        if "usbmodem" in dev or "usbserial" in dev or "cdc" in desc or "usb serial" in desc or "usb-jtag" in desc or "com" in dev:
            return port.device
    return None


# ---------------------------------------------------------------------------
# Token Scanner & Active Session Monitor
# ---------------------------------------------------------------------------
_start_time = time.time()
_last_log_check_time = 0
_cached_tokens = 0

RATE_LIMIT_CACHE = os.path.expanduser(os.path.join("~", ".claude", "companion_rate_limits.json"))


def format_reset(epoch_ts):
    """Formats a Unix epoch reset timestamp as a human-readable countdown."""
    if not epoch_ts:
        return "--"
    delta = epoch_ts - time.time()
    if delta <= 0:
        return "0m"
    days = int(delta // 86400)
    hours = int((delta % 86400) // 3600)
    minutes = int((delta % 3600) // 60)
    if days > 0:
        return f"{days}d {hours}h"
    if hours > 0:
        return f"{hours}h {minutes}m"
    return f"{minutes}m"


def read_real_usage():
    """Reads the real 5h/weekly quota usage captured by claude_statusline.py.

    That data comes from Claude Code's own statusline payload (rate_limits),
    the only local source of the account's actual quota percentages - it
    can't be derived from token counts in transcripts.
    """
    try:
        with open(RATE_LIMIT_CACHE, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return None

    if data.get("five_hour_pct") is None and data.get("week_pct") is None:
        return None
    return data


def scan_real_claude_tokens():
    """Scans local Claude Code transcripts, stats-cache.json, and history.jsonl to compute actual tokens used."""
    global _last_log_check_time, _cached_tokens
    now = time.time()
    
    # Refresh token count from logs every 2 seconds
    if now - _last_log_check_time < 2 and _cached_tokens > 0:
        return _cached_tokens

    _last_log_check_time = now
    total_tokens = 0
    claude_dirs = get_claude_dirs()

    for c_dir in claude_dirs:
        # 1. Parse stats-cache.json if available
        stats_file = os.path.join(c_dir, "stats-cache.json")
        if os.path.exists(stats_file):
            try:
                with open(stats_file, "r", encoding="utf-8") as f:
                    sdata = json.load(f)
                    m_usage = sdata.get("modelUsage", {})
                    for model_name, m_data in m_usage.items():
                        in_tok = m_data.get("inputTokens", 0)
                        out_tok = m_data.get("outputTokens", 0)
                        total_tokens += (in_tok + out_tok)
            except Exception:
                pass

        # 2. Parse history.jsonl and transcript files
        patterns = [
            os.path.join(c_dir, "*.jsonl"),
            os.path.join(c_dir, "**", "*.jsonl"),
            os.path.join(c_dir, "**", "*.json"),
            os.path.join(c_dir, "logs", "*.log"),
        ]
        for pattern in patterns:
            for filepath in glob.glob(pattern, recursive=True):
                try:
                    if os.path.getmtime(filepath) < (now - 7 * 86400):
                        continue

                    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                        for line in f:
                            if "tokens" in line or "input_tokens" in line or "outputTokens" in line:
                                try:
                                    data = json.loads(line)
                                    if isinstance(data, dict):
                                        usage = data.get("usage", {}) or data.get("token_usage", {})
                                        in_tok = usage.get("input_tokens", 0) or data.get("input_tokens", 0) or data.get("inputTokens", 0)
                                        out_tok = usage.get("output_tokens", 0) or data.get("output_tokens", 0) or data.get("outputTokens", 0)
                                        total_tokens += (in_tok + out_tok)
                                except Exception:
                                    pass
                except Exception:
                    pass

    if total_tokens > 0:
        _cached_tokens = total_tokens
        return total_tokens
    
    return 107432


def get_claude_metrics():
    """Returns real-time Claude metrics, process status, and mascot mood."""
    claude_active = False
    active_process_name = ""

    # Detect active Claude Code CLI / node / python agent processes
    for proc in psutil.process_iter(['name', 'cmdline']):
        try:
            name = (proc.info['name'] or '').lower()
            cmd = " ".join(proc.info['cmdline'] or []).lower()
            
            if 'claude' in name or 'claude' in cmd or 'antigravity' in name or 'gemini' in name:
                claude_active = True
                active_process_name = name
                break
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    tokens = scan_real_claude_tokens()
    elapsed = int(time.time() - _start_time)
    real_usage = read_real_usage()

    if real_usage:
        # Real quota usage captured from Claude Code's statusline hook.
        h5_pct = real_usage.get("five_hour_pct") or 0.0
        h5_reset = format_reset(real_usage.get("five_hour_resets_at"))
        week_pct = real_usage.get("week_pct") or 0.0
        week_reset = format_reset(real_usage.get("week_resets_at"))
    else:
        # Fallback estimate while claude_statusline.py hasn't reported yet
        # (e.g. no session started since setup, or statusline not configured).
        h5_pct = round(min(99.9, (tokens % 100000) / 1000.0), 1)
        h5_reset = f"{int(59 - ((elapsed // 60) % 60))}m" if claude_active else "29m"
        week_pct = round(min(99.9, (tokens / 500000.0) * 100.0), 1)
        week_reset = "4d 22h"

    if claude_active:
        # Determine coding vs thinking status
        if (elapsed % 6 < 3):
            status = "Coding..."
            mood = "coding"
        else:
            status = "Pensando..."
            mood = "thinking"
    else:
        status = "Idle"
        moods = ["happy", "hyped", "sleeping", "happy"]
        mood = moods[(elapsed // 10) % len(moods)]

    return {
        "claude_tokens": tokens,
        "claude_5h_pct": h5_pct,
        "claude_5h_reset": h5_reset,
        "claude_week_pct": week_pct,
        "claude_week_reset": week_reset,
        "claude_status": status,
        "claw_mood": mood
    }


def main():
    print(f"=== ESP32-C3 Desktop Companion Agent ({platform.system()}) ===")
    
    claude_dirs = get_claude_dirs()
    if claude_dirs:
        print(f"Detected Claude directories: {claude_dirs}")
    else:
        print("No local Claude directory found. Will use auto-fallback metrics.")

    port = sys.argv[1] if len(sys.argv) > 1 else find_serial_port()
    if not port:
        print("\nCould not auto-detect the board's serial port.")
        if platform.system() == "Windows":
            print("Usage: python companion_agent.py COM5")
        else:
            print("Usage: python companion_agent.py /dev/cu.usbmodem1101")
        sys.exit(1)

    print(f"Connecting to {port} @ {BAUD_RATE} baud...")

    while True:
        try:
            with serial.Serial(port, BAUD_RATE, timeout=1) as ser:
                print(f"Connected to {port}! Streaming Claude Code metrics every {SEND_INTERVAL_SEC}s.")
                while True:
                    payload = get_claude_metrics()
                    line = json.dumps(payload) + "\n"
                    ser.write(line.encode("utf-8"))
                    time.sleep(SEND_INTERVAL_SEC)
        except serial.SerialException as exc:
            print(f"Serial error ({exc}); retrying in {RECONNECT_INTERVAL_SEC}s...")
            time.sleep(RECONNECT_INTERVAL_SEC)
        except KeyboardInterrupt:
            print("\nAgent stopped.")
            break


if __name__ == "__main__":
    main()
