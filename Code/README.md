# ESP32 note tablet: portrait ST7796S + BLE

## Current milestone: MQTT task display

See `PLAN.md` for the iPhone / MQTT roadmap and synchronization design.
After calibration, tap the idle clock screen to open the menu. Choose To-do
List or Draw Notes. Notes has Write, Erase and Home controls. Drawings survive
navigation and the 60-second idle timeout in a 16,960-byte monochrome RAM buffer.
Reset/power loss still clears the note. The idle screen is not hardware sleep.

The To-do view receives the computer's list over Wi-Fi/MQTT, with two tasks per
page and Prev/Next controls. Tap a checkbox to complete/reopen while connected;
the server-confirmed snapshot updates the display. Titles are edited in the browser.
See `../LocalMQTT/README.md` for the server and Windows firewall setup.
Run `python3 setup_network.py` in this directory before flashing to enter Wi-Fi
credentials locally and import the generated MQTT credentials. This configuration
is required to build; older firmware silently disabled Wi-Fi when it was missing.
The clock still displays `--:--` / `TIME NOT SET`; time synchronization is later.
BLE now supports text task entry from a phone testing app, with offline local
checkboxes. See [BLE.md](BLE.md). Local tasks remain in RAM and are separate
from the computer's list; a companion app and cross-transport sync are later.

Flash from the activated WSL shell, inside the **Code** directory:

```bash
idf.py -B build-esp32-wsl -p /dev/ttyUSB0 flash monitor
```

Validation: host UI tests cover tap/release suppression across navigation,
Write/Erase switching, note-restore calls, and inactivity return to the clock.
Hardware check: draw, return Home, revisit Notes, erase, wait 60 seconds, then
open Notes again and confirm the image survives. Reset should clear it.

Target hardware: **Inland ESP32 board with ESP32-WROOM-32E module**.
The project now uses ESP-IDF (CMake), not the Pico SDK or Arduino.
The MQTT build is verified with your ESP-IDF 6.1 development checkout.
ESP32-S3 also has an optional pin map,
but the default and the wiring below are for your original ESP32.
No PSRAM is required or enabled. Default flash size is a conservative 4 MB;
confirm actual flash size before increasing it.

## Wire the display (USB disconnected)

These are **GPIO labels**, not header position numbers. Do not retain the Pico
physical-pin wiring. Check labels on the Inland board before connecting.

| ST7796S module | ESP32-WROOM-32E development board |
| --- | --- |
| VCC | USB-derived 5V supply pin, after confirming the board pinout |
| GND | GND |
| CS | GPIO27 |
| RESET / RST | GPIO25 |
| DC / RS | GPIO26 |
| SDI / MOSI | GPIO23 |
| SCK | GPIO18 |
| LED | 3V3 |
| SDO / MISO | Unconnected |
| Touch pins | See pen-tracing section below |

The previously identified display module accepts 3.3–5V VCC; its LED control
uses 3.3V. ESP32 signal pins use 3.3V: never connect 5V to a GPIO.
Use soldered headers and secure short jumpers. Remove any SD card for this test.
The white-screen fault on the earlier wiring is not proven fixed by this port.
A lit backlight does not establish that the LCD controller has stable power.

## Install and activate ESP-IDF

Use Espressif's official setup instructions for ESP-IDF 5.4 and your OS:
https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/get-started/index.html

The Arm compiler used for the Pico cannot compile this Xtensa ESP32 project.
In Linux/WSL, after installing ESP-IDF, activate it in each new shell:

```bash
source ~/esp/esp-idf/export.sh
```

On Windows, use the ESP-IDF terminal installed by Espressif. Native Windows
flashing is often simpler than forwarding a USB serial device into WSL.

## Build and flash

From this project directory in an activated ESP-IDF shell:

```bash
idf.py -B build-esp32 set-target esp32
idf.py -B build-esp32 build
idf.py -B build-esp32 -p /dev/ttyUSB0 flash monitor
```

Replace `/dev/ttyUSB0` with your board's serial port (`COM5`, for example, in
Windows). If automatic flashing fails, hold BOOT while connecting/resetting
and release when flashing begins. Exit the serial monitor with Ctrl+].
ESP32 uses serial flashing, not Pico BOOTSEL/UF2 drag-and-drop.

For subsequent Bash builds, `bash compile.sh` runs the build without deleting
anything. The old `build/` and `build-display/` directories contain Pico output;
do not use those binaries for the ESP32. The new build is `build-esp32/`.

## What to expect

- Portrait 320x480 screen with Hello and World! on separate lines and RGB bars.
- BLE device named **NoteTablet-BLE** (previously PicoW-BLE).
- Service: `12345678-1234-5678-1234-56789abcdef0`.
- Read characteristic: `12345678-1234-5678-1234-56789abcdef1`.
- Readable text: **What it do**, preserving your latest greeting edit.
- Advertising resumes after disconnection or a failed connection attempt.

Test by power cycling, leaving the display running for several minutes, reading
via a BLE scanner, disconnecting, and reconnecting. Check serial logs for resets
or brownout reports if the screen goes white. This demo has no pairing requirement
and no writable notes, persistent storage, or Wi-Fi yet.

## Source layout

- `main/main.c`: initialize the display once, then start BLE.
- `main/display.c`: SPI ST7796S driver, 1 MHz, RGB565, portrait; small scanline buffer.
- `main/board.h`: wiring by chip target.
- `main/ble.c`: NimBLE GATT service and connection/advertising callbacks.
- `sdkconfig.defaults`: BLE and build defaults, no dependency on PSRAM.

NimBLE runs in its own FreeRTOS task; no Pico async polling loop is needed.
The repeated display-reset loop found in your latest Pico code is not carried
into the ESP32 app. Only the display code owns SPI2; touch/SD integration later
will need shared-bus coordination. NVS currently contains only system data;
its initialization may erase/reinitialize NVS on a format/full-pages error.
Revisit that policy before storing user notes there.

The previous Pico sources, including your edits, are saved in `legacy/pico/`.
The Pico SDK and old build directories are left in place but unused. To rebuild
that snapshot, set PICO_SDK_PATH to the existing SDK and configure it separately.

Optional ESP32-S3 pin map: CS=10, MOSI=11, SCK=12, DC=9, RESET=8. Select it only
for actual S3 hardware with `idf.py -B build-esp32 set-target esp32s3`; changing
target regenerates sdkconfig. Do not use those pins on your WROOM-32E.

## Validation status

The port has been source-reviewed against ESP-IDF 5.4 NimBLE and SPI APIs.
The pen-tracing firmware compiled successfully with the installed WSL ESP-IDF 6.1 development checkout. Display/BLE operation was confirmed by the user before this change; touch calibration and drawing still need hardware verification.

References:
- https://github.com/espressif/esp-idf/tree/v5.4.2/examples/bluetooth/nimble/bleprph
- https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/peripherals/spi_master.html

## Pen tracing (XPT2046 resistive touch)

Keep the working display connections. With USB unplugged, add these five
connections for the ESP32-WROOM-32E (GPIO labels, not header positions):

| Display touch pin | ESP32 |
| --- | --- |
| T_CLK | IO14 |
| T_DIN | IO13 |
| T_DO | IO19 |
| T_CS | IO32 |
| T_IRQ | IO33 |

Touch uses a separate SPI3 bus at 500 kHz. Do not connect T_CLK/T_DIN to the
LCD bus in this configuration. LCD SDO remains disconnected. If your display
has CTP_* labels instead of T_*, this driver is not for that touch variant.

Build/flash in the activated WSL ESP-IDF shell:

```bash
idf.py -B build-esp32-wsl -p /dev/ttyUSB0 flash monitor
```

At each boot the hello screen is followed by three red calibration crosses:
upper left, upper right, then lower left. Hold the stylus steadily on each cross
until it disappears, then lift. After all three, a white canvas appears; draw
with moderate stylus pressure to leave a black trace. It does not recognize
handwriting or save strokes yet. Press EN/reset to clear and recalibrate.
BLE remains active during calibration and drawing. If the first cross never
advances, check T_IRQ, T_CS and the other touch wires. The serial monitor logs
each calibration target and canvas readiness. Small sample filtering and a
three-point affine transform handle axis reversal and portrait orientation.

## Write and Erase tools

After calibration, the top toolbar has Write and Erase buttons. The selected
button is green. Tap and lift to select a tool, then use the canvas below:
Write draws a 3-pixel black stroke; Erase rubs out a 17-pixel-wide area.
Switching tools keeps the rest of the drawing. Neither tool can paint over
the toolbar. Reset still clears the page and restarts calibration; BLE stays on.
