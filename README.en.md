# HueLeaf Base Community Firmware

[简体中文](README.md) · [GitHub release notes](RELEASE.md) · [Third-party notices](NOTICE.md)

An ESP-IDF firmware fork for OSPTEK's HueLeaf Base, an ESP32-C5 board with 4 MB flash, a PCF8563 RTC, a microSD slot, and a 3.7-inch 480 × 720 six-color EPD037A02 panel. It builds on the manufacturer's `examples/3.7einkDvb` project at commit `a34b55b168e0e6e12fc6215c6706e7a850e2d6a4`. [Original project](https://gitee.com/osptek/devkit-hueleaf_base) · [Panel documentation](https://gitee.com/osptek/display-epd-3.7inch-480x720/tree/main/versions/EPD037A02)

## v2026.11.25.1 · Sync and photo processing

- Status and Network shows sync results, retries, disk space and up to 50 in-memory log entries. Sync Now and the configurable daily window are available here.
- With saved router credentials, startup, day rollover and the daily window can trigger a short Wi-Fi session. The default window is 03:30–05:30 China Standard Time. Automatic connections are skipped below 10% battery. Sync and web requests defer Wi-Fi shutdown.
- Forecasts are marked successful after complete data is saved. History coverage is checked by date. New trip and history cache records add metadata and integrity fields while remaining compatible with existing files.
- Photo processing includes four dither algorithms, adjustable ink colors, image presets, Worker rendering and original-crop comparison. Album reorder settings stay in the browser. The OTP waveform still uses full-panel refresh. Default color values are estimates until calibrated on hardware.


## Features

- Send text and process photos in the device web console. Crop or rotate a picture, show it immediately, or save originals and display frames separately on SD. The gallery supports batch uploads, thumbnail selection and removal.
- Use a monthly calendar, a seven-day weather clock (Yangzhou by default), a lunar date and literary quote page with daily almanac entries, or a to-do list with deadlines and adjustable type.
- Save up to 12 manually entered train or flight itineraries. Choose a destination from the offline China place index, preview arrival-day weather when available, pin a trip or follow the automatic selection. Trips are pruned seven days after arrival. The forecast is cached on SD for arrivals from today through seven days ahead. On the arrival date, opening the itinerary triggers a fresh forecast if online fetching is enabled and the device is connected. The weather section shows the arrival date.
- Read On This Day events from a seven-day SD cache. Annual almanac files for 2026 and 2027 are bundled and can be written to the SD card.
- Pick pages for timed rotation and set the interval in minutes. Set black-and-white or color per page. Both use a full-panel refresh, usually around 30 seconds. The clock updates when the page redraws.

The router IP QR code returns to the previous page or carousel after 60 seconds; Manually enabled Wi-Fi stays on until explicitly turned off. The history page uses the APIHz mainland China endpoint and accepts a personal developer ID and key if the public demo is rate limited. Existing cached entries remain readable.

## Prepare and flash

Use a data-capable USB-C cable, a backed-up FAT32 microSD card, Python 3 and the matching board and screen. Open PowerShell or Terminal in the extracted release directory. The steps below pin esptool 4.13.0.

**Windows PowerShell**

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
py -3 -m venv .venv
$py = ".\.venv\Scripts\python.exe"
& $py -m pip install "esptool==4.13.0"
$port = "COM4"  # Replace with your port
& $py -m esptool --chip esp32c5 --port $port flash_id
Get-FileHash .\firmware\*.bin -Algorithm SHA256
& $py -m esptool --chip esp32c5 --port $port --baud 460800 write_flash 0x10000 .\firmware\hueleaf-app-0x10000.bin
```

**macOS Terminal**

```bash
ls /dev/cu.*
python3 -m venv .venv
. .venv/bin/activate
python -m pip install "esptool==4.13.0"
PORT=/dev/cu.usbmodemXXXX  # Replace with your port
python -m esptool --chip esp32c5 --port "$PORT" flash_id
(cd firmware && shasum -a 256 -c SHA256SUMS.txt)
python -m esptool --chip esp32c5 --port "$PORT" --baud 460800 write_flash 0x10000 firmware/hueleaf-app-0x10000.bin
```

These commands update the application on a board using the factory partition layout. To check the existing layout, run `read_flash 0x8000 0xC00 current-partitions.bin` with the same esptool prefix and compare the file's SHA-256 with the bundled partition table. For a blank board or full installation, use this argument sequence after the esptool prefix. Forward-slash paths work in both shells.

```text
write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 firmware/bootloader-0x2000.bin 0x8000 firmware/partition-table-0x8000.bin 0x10000 firmware/hueleaf-app-0x10000.bin
```

To back up the whole 4 MB chip first, use `read_flash 0x0 0x400000 hueleaf-local-backup.bin` and keep the file private. If esptool stalls at Connecting, hold BOOT, tap RESET, and release BOOT once esptool identifies the chip. Reduce the speed to 115200 if needed.

## Use the device

Hold BOOT to enable temporary Wi-Fi, join the hotspot shown on screen and open `http://192.168.4.1`. Use the Network tab to connect the board to your router when you want new forecasts or historical events. The hotspot runs at 5 GHz and the console is intended for a trusted local network. Manually enabled management Wi-Fi stays on until turned off with BOOT or the web switch.

| Button | Action |
| --- | --- |
| BOOT single press | Next enabled page |
| BOOT double press | Rotate 90 degrees |
| BOOT long press | Toggle temporary Wi-Fi |
| Power single press | Open the chosen shortcut page, then return on the next press |
| Power double press | Show white or restore the last page |

The [Chinese README](README.md) lists the exact SD paths and the full first-install, connection, gallery, trip and troubleshooting workflows.

## Build and contribute

Activate [ESP-IDF v5.5.4](https://github.com/espressif/esp-idf/tree/v5.5.4), then run `idf.py build` from `source/`. Use `idf.py -p YOUR_PORT app-flash` for an application update. A redistributable Noto Sans CJK SC bitmap font is included in this build.

Open an issue or PR to share a solution for unfinished upstream features, documented display tests, or a firmware fork of your own. Include the board, panel, toolchain version, reproduction steps and hardware test results. The code inherits the upstream [Apache-2.0 license](LICENSE). See the [notices](NOTICE.md) for display hardware and third-party licensing.


### v2026.11.25.1 photo processing

Legacy remains the default after synthetic benchmarks. Fine, Sharp and Vivid are available as experimental profiles. Use the advanced display calibration controls to show ink patches, sample a white card and save the palette. Calibrate under even lighting at the operating temperature.


### v2026.11.26.1

Configure a daily Wi-Fi start time in Status and Network. Scheduled Wi-Fi starts syncing immediately and closes around 15 minutes later. Manual Wi-Fi can optionally use the same timeout. The Wi-Fi icon has no rectangular background.


### v2026.11.26.2

The optional Clean Six-Ink preset suppresses isolated chromatic dots in flat areas using the current palette. Legacy remains the default. Existing settings, SD files and the frame protocol remain compatible. [FrameFilm / ForFilm](docs/framefilm-analysis.md)


Inspired by ForFilm’s six-ink workflow, separate preview and editor, and visible job progress. No upstream code, UI assets, or palette values were imported. FrameFilm is GPL-3.0. [FrameFilm / ForFilm](https://github.com/kirmisaki/FrameFilm/tree/main/tools/ForFilm)
