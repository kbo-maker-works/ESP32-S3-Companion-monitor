# CompaDeck

A desk companion and touch deck for the **Guition JC3248W535** (ESP32-S3, 3.5" 320x480 touch screen).
It shows your PC's stats, controls the PC, follows your 3D printers and Claude Code, and runs a few
full-screen apps, all laid out from a web portal on the device.

It grew out of [TouchDeck](https://github.com/Keralots/TouchDeck) by Keralots and the StatNode
Companion, and keeps their MIT license.

## What it does

**The deck.** Pages of tiles you lay out yourself, landscape or portrait. A page can be stacked under another,
or show only while the PC is on or a printer is powered. Tiles can be:

- **Action** tiles: open a page or an app, send a command to the PC (hotkeys, media, scripts) or call any HTTP
  endpoint (Home Assistant and the like).
- **Sensor** tiles that read a value over HTTP, and a **slider** for the PC's volume.
- **PC metric** tiles: CPU, GPU, RAM, temperatures, network, FPS, as a value, a bar or a chart.
- **Clock**, **now playing** with media controls, and **printer** tiles.
- **Widgets**: weather, the Claude Code mascot, Claude plan usage, GitLab merge requests, a Jira filter, PC stats
  rings and batteries, and the flight radar.

Themes (Midnight, Daylight and Glass over your own wallpapers or daily pictures), a quick panel, an on-device
editor, a night schedule, sleep on presence and a photo frame while idle.

**Apps** (full screen, from a tile or a widget):

- **AI**: your Claude Code sessions. Their permission prompts and questions pop up on the deck, and you can
  answer them there.
- **Work**: GitLab merge requests and pipelines, Jira filters.
- **Bench**: a sheet for the workbench, sent from the PC (soldering steps, wire colours, a pinout picture).
- **Radar**: air traffic around you from [adsb.lol](https://adsb.lol).
- **Settings**: the device's address and status.

**Bambu Lab printers**, over the LAN or through Bambu Cloud via the PC agent: progress, temperatures, AMS
trays, errors, and an alert when the bed has cooled.

**Hardware extras** (all optional): battery with charge detection and deep sleep on a power cut, speaker sounds,
a vibration motor, and an LED, either a single PWM LED or an addressable strip (WS2812) with effects. The strip
can also show print progress in the filament's colour, a printer error, Claude waiting on you, or the radar's
sweep.

## Hardware

- Guition **JC3248W535**: ESP32-S3-N16R8 (16 MB flash, 8 MB PSRAM), AXS15231B QSPI panel, capacitive touch,
  NS4168 speaker amplifier, battery connector, microSD slot.
- Optional: an addressable LED strip on the UART connector (5V, GND, data on RX, GPIO 44), a vibration motor,
  a USB-C breakout for the case.

The enclosure is a modified version of the TouchDeck case; its files are not in this repository.

## Build and flash

The firmware is a [PlatformIO](https://platformio.org) project on the
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform (Arduino 3.x, ESP-IDF 5), with LVGL 9.

```
pio run                # build
pio run -t upload      # first flash, over USB
```

- `platformio.ini` uses its own PlatformIO folder (`~/.platformio-pioarduino`) so it does not clash with
  projects on the older espressif32 platform. Run one build at a time.
- The portal in `web/` is gzipped into the firmware at build time (`scripts/web_embed.py`).
- After the first flash, update over WiFi from the portal (Maintenance, Firmware update).
- The partition table (`partitions_16mb.csv`, two 6.25 MB OTA slots) can only change with a USB flash.

## First start

With no WiFi saved, the device opens a WiFi access point named `CompaDeck-XXXX` (password `touchdeck1234`).
A captive portal there asks for your network. Improv WiFi over the USB serial port works too.

Once it is on your network, open `http://compadeck.local` (or its IP, shown on the Settings app) for the portal:

- the deck editor;
- display, wallpapers, screen, hardware, apps, printers, time and network settings;
- configuration backup and firmware updates.

## The PC agent

`pc-agent/` holds the companion that runs on your PC or Mac (Python). It sends:

- the PC's metrics, now playing and battery levels;
- Claude Code's state, usage and prompts;
- GitLab, Jira and Bambu Cloud data, and the Bench sheets.

It also runs the commands the deck sends. Setup and details are in
[pc-agent/README.md](pc-agent/README.md).

## Layout

| Path | What |
|---|---|
| `src/` | firmware: `ui/` (the deck), `apps/`, `widgets/`, `hardware/`, `network/`, `printers/`, `services/`, `core/` |
| `include/` | build configuration (`config.h`), settings, LVGL config |
| `web/` | the device's web portal |
| `pc-agent/` | the PC companion and its settings page |
| `tools/` | helper scripts (wallpapers, microSD migration) |
| `docs/` | hardware notes |

## License

MIT, see [LICENSE](LICENSE). TouchDeck © Rafał Stolarek (Keralots).
