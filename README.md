# ESP32 Note Tablet

A touchscreen note pad and to-do display built with an Inland ESP32-WROOM-32E,
a Hosyond 4-inch ST7796S display, and XPT2046 resistive touch. Firmware uses
ESP-IDF and CMake. A local Python server provides a browser task editor,
SQLite storage, and an MQTT broker.

The project began on a Raspberry Pi Pico W; the active firmware is now ESP32.
This README describes the current implementation. The original Pico code is
preserved under `Code/legacy/pico/`.

## Current features

| Feature | Status |
| --- | --- |
| Portrait display | 320 × 480, SPI ST7796S |
| Pen input | Three-point touch calibration at each boot |
| Home menu | To-do list, Draw Notes, Clock |
| Drawing | Black pen, area eraser, Home button |
| Notes across navigation | Preserved in RAM, including the idle screen |
| Idle screen | Appears after 60 seconds without touch; not deep sleep |
| Clock | Placeholder until time is set; automatic time sync is not implemented |
| Computer task editor | Add, edit, complete/reopen, delete, and filter tasks |
| Task storage | SQLite on the computer |
| Wi-Fi/MQTT | Authenticated LAN subscription and tablet receipt status |
| Tablet to-do view | Two tasks per page, Prev/Next; editing stays on the computer |
| BLE | Advertises `NoteTablet-BLE` with a readable greeting |
| BLE task transfer / Wi-Fi setup | Proposed, not implemented |
| iPhone app, dictation, handwriting recognition | Planned |
| Tablet flash persistence | Deferred: notes, tasks, and calibration are not saved by the app |

Display/BLE and navigation have been exercised during development. The MQTT
firmware builds and host tests pass; an on-device Wi-Fi/MQTT connection still
needs confirmation after configuring and flashing the board.

## Project layout

```text
2040Bluetooth/
├── README.md                 Main setup and usage guide
├── Code/                     Active ESP-IDF CMake project
│   ├── main/                 Display, touch, UI, BLE, Wi-Fi/MQTT, task parser
│   ├── tests/                Host-side UI and task-parser tests
│   ├── setup_network.py      Generate local firmware network configuration
│   ├── sdkconfig.defaults*   Firmware configuration defaults
│   ├── dependencies.lock    Managed component versions
│   ├── PLAN.md              Earlier detailed roadmap; see status here first
│   └── legacy/pico/         Previous Pico firmware
├── LocalMQTT/                Python server, browser UI, and integration tests
│   ├── server.py             HTTP API + SQLite + embedded aMQTT broker
│   ├── static/               Browser interface
│   ├── setup_lan.py          Generate MQTT credentials and configure LAN IP
│   ├── allow-mqtt.ps1        Windows firewall rule, requires Administrator
│   ├── check_connection.py   Read-only authenticated MQTT connection check
│   ├── run.cmd               Start the Windows server
│   └── data/                 Generated database and local credentials
├── Case/                     Enclosure/pen-holder STL files
└── Kicad/                    Reserved for electronics design files
```

Generated build folders and the old Pico SDK may also be present. Always run
ESP-IDF commands from **Code**, not the workspace root. This guide uses
`Code/build-esp32-wsl`; `compile.sh` instead defaults to `Code/build-esp32`.

## Hardware and wiring

Use the **GPIO/IO labels**, not physical header positions. Disconnect USB before
rewiring. These connections target the original ESP32-WROOM-32E, not ESP32-S3.

| Display pin | ESP32 connection |
| --- | --- |
| VCC | 5V, for the project's identified display module |
| GND | GND |
| CS | IO27 |
| RESET / RST | IO25 |
| DC / RS | IO26 |
| SDI / MOSI | IO23 |
| SCK | IO18 |
| LED | 3V3 |
| SDO / MISO | Leave disconnected |
| T_CLK | IO14 |
| T_DIN | IO13 |
| T_DO | IO19 |
| T_CS | IO32 |
| T_IRQ | IO33 |

ESP32 GPIO signals are 3.3V; do not apply 5V to signal pins. Confirm the supply
requirements if using a different display module. The LCD uses SPI2 at 1 MHz;
touch uses a separate SPI3 bus. Keep the touch wires on the pins above.

Both modules need secure, soldered headers. Direct jumpers resolved the earlier
white-screen problem; the breadboard or a jumper was suspected. A lit backlight
does not prove that SPI communication works. Pin definitions are in
[`Code/main/board.h`](Code/main/board.h).

## How the current connection works

```text
Computer browser at localhost:8080
                  |
          HTTP API + SQLite
                  |
       MQTT broker on computer
                  |
         Home router / LAN
                  |
         ESP32 Wi-Fi client
                  |
         Tablet to-do screen
```

The ESP32 joins your existing 2.4 GHz Wi-Fi. It does **not** create a network
that appears in the computer's Wi-Fi list. The computer may use Ethernet or
another Wi-Fi band if both devices can communicate on the same LAN.

`localhost` means the computer where the browser/server runs. The ESP32 uses
the computer's **LAN IPv4 address**, not localhost and not the public internet
IP. `192.168.1.148` was the development computer's address; confirm it using
Windows `ipconfig` under the active Wi-Fi/Ethernet adapter.

The computer must remain awake with the server running for syncing. The tablet
keeps its last received tasks in RAM during disconnection; after reboot it
needs to reconnect to receive them again. The phone is not part of this path yet.

## 1. Start the local server — Windows PowerShell

The following paths match this development computer; change them for another
checkout. Python 3.11 was used for the verified server environment.

For a fresh environment:

```powershell
cd C:\Users\messm\OneDrive\Desktop\2040Bluetooth\LocalMQTT
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements-lock.txt
```

Skip those installation steps if `.venv` is already installed. Provision the
LAN address and start the server:

```powershell
ipconfig
# Replace this address if your active adapter shows a different IPv4 address.
.\.venv\Scripts\python.exe setup_lan.py --host 192.168.1.148
.\.venv\Scripts\python.exe server.py --lan
```

Open [http://localhost:8080](http://localhost:8080). Leave the terminal running;
Ctrl+C stops it. On later runs, double-click `LocalMQTT/run.cmd` or run the final
command above. Run only one server instance. If the page is already available,
check the existing instance before launching another.

The server also supports loopback-only development without `--lan`, but the
ESP32 cannot connect to that mode. MQTT LAN mode uses generated credentials and
topic permissions; HTTP remains accessible only on the computer.

### Allow MQTT through Windows Firewall

Run once from **Administrator PowerShell**:

```powershell
cd C:\Users\messm\OneDrive\Desktop\2040Bluetooth\LocalMQTT
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\allow-mqtt.ps1
```

This allows inbound TCP 1883 to the configured LAN address from the local
subnet. The execution-policy override applies only to that command's process.
The earlier automatic firewall attempt lacked Administrator rights; do not
assume the rule exists until this succeeds.

## 2. Configure and flash the tablet — WSL Bash

The verified firmware toolchain is the installed ESP-IDF 6.1 development
checkout at `~/esp32/esp-idf`. It uses the **esp32** target, not esp32s3.
See [Espressif's setup guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html)
for a new toolchain installation. Activate the existing installation in each
new WSL shell:

```bash
source ~/esp32/esp-idf/export.sh
idf.py --version
```

If activation reports that its Python virtual environment is missing:

```bash
cd ~/esp32/esp-idf
./install.sh esp32
source ./export.sh
```

Connect the ESP32 USB device to WSL using your USB forwarding setup. Windows
COM5 and WSL `/dev/ttyUSB0` are names on different operating systems; use the
port that actually appears in your current shell:

```bash
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

Configure the firmware, then build and flash:

```bash
cd /mnt/c/Users/messm/OneDrive/Desktop/2040Bluetooth/Code
python3 setup_network.py
idf.py -B build-esp32-wsl -p /dev/ttyUSB0 flash monitor
```

The setup script asks for the Wi-Fi network name, a hidden Wi-Fi password, and
the computer LAN IP. Press Enter to accept the bracketed IP if it is correct.
It imports the MQTT credentials from `LocalMQTT/data/connection.json` and writes
`Code/main/network_config.h`. Without that header, the firmware keeps Wi-Fi
disabled and displays `WIFI NOT SET`.

Build without flashing with `idf.py -B build-esp32-wsl build`. ESP32 output is
flashed over serial; do not look for a Pico-style `.uf2` file. Exit the serial
monitor with **Ctrl+]**. If automatic flashing cannot connect, hold BOOT during
connection and release when flashing begins.

## Daily use

1. Start the computer server and leave it running.
2. Power the tablet. Hold the stylus on each red cross, then lift when it advances.
   These three points map raw touch readings to screen coordinates.
3. Tap the idle screen to open the menu.
4. Open **To-do List** to see tasks, completion state, and Prev/Next pages.
5. Add or change tasks in the computer browser. The tablet receives new snapshots.
6. Open **Draw Notes** to draw. Tap and lift on **Write**, **Erase**, or **Home**.
   Erase is an area eraser, not a whole-page clear button.

The note survives Home and the 60-second idle timeout. Power loss/reset clears
it and restarts calibration. The idle screen does not turn off the backlight.
Task titles support up to 120 UTF-8 bytes on the server, with at most 50 tasks;
the tablet's bitmap font substitutes `?` for unsupported characters.

Successful connection logs include `Wi-Fi ready`, `MQTT connected`, and
`Received ... tasks`. Browser status distinguishes publication to the broker
from the revision received by the tablet. Receipt confirms arrival in the
tablet's RAM queue, not a flash write or proof that pixels have been rendered.

## BLE and the proposed iPhone workflow

Current BLE service:

| Setting | Value |
| --- | --- |
| Advertising name | `NoteTablet-BLE` |
| Service UUID | `12345678-1234-5678-1234-56789abcdef0` |
| Read characteristic | `12345678-1234-5678-1234-56789abcdef1` |
| Greeting | `What it do` |

Use a BLE GATT scanner to read the greeting. This is not a keyboard/audio
profile or task-transfer protocol. BLE stays enabled with Wi-Fi/MQTT.

The proposed portable workflow is direct iPhone-to-tablet BLE task sync, with
optional MQTT for computer updates at home. BLE could also send Wi-Fi credentials
without recompiling. These are two different features, neither implemented yet.
MQTT currently runs over Wi-Fi/TCP; it does not run through the greeting service.
A phone gateway would need explicit BLE-to-MQTT bridge code.

## Data, credentials, and protocol

- `LocalMQTT/data/tasks.sqlite3`: authoritative tasks and revisions on the PC.
- `LocalMQTT/data/connection.json`: local MQTT credentials and server address.
- `LocalMQTT/data/passwords.txt`: broker password hashes.
- `Code/main/network_config.h`: generated Wi-Fi/MQTT configuration.

Credential files are ignored by their component `.gitignore` files. Compiled
firmware also contains credentials; do not publish it or those files. Back up
the database with the server stopped. Tablet drawings are not in this database.

MQTT uses retained QoS 1 snapshots at
`notepad/v1/devices/tablet-001/state`. The tablet publishes heartbeat/received
revision at `notepad/v1/devices/tablet-001/presence`, including an offline Last
Will. The service is allowed to publish state; tablet credentials cannot edit
it. Server restart reconstructs retained state from SQLite.

This is a trusted-LAN prototype using unencrypted MQTT. Do not port-forward it
to the internet. The browser editor is localhost-only, so opening the same URL
on an iPhone will not reach this computer.

See [`LocalMQTT/README.md`](LocalMQTT/README.md) for the JSON snapshot format,
HTTP API, process management, and server details.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| No ESP32 network in the Wi-Fi list | Expected: the tablet joins your router; it is not an access point. |
| `idf.py: command not found` | Source the WSL ESP-IDF `export.sh` in this terminal. |
| PowerShell syntax errors in Bash | Use WSL commands in Bash and Windows commands in PowerShell. |
| `CMakeLists.txt not found` | Change into the project's `Code` directory. |
| No `/dev/ttyUSB0` | Check USB cable and USB forwarding into WSL; COM5 is a Windows name. |
| Port not readable | Check `ls -l /dev/ttyUSB0` and serial-group membership; if owned by `dialout`, add your user with `sudo usermod -aG dialout "$USER"`, then start a new login session. |
| `WIFI NOT SET` | Run `setup_network.py`, rebuild, and flash. |
| `WIFI CONNECTING` | Verify SSID/password and 2.4 GHz availability. |
| `MQTT CONNECTING` | Check server `--lan`, LAN IP, firewall, and router client isolation. |
| White display but BLE works | Check LCD power, ground, CS/DC/reset/SPI wires; previously direct jumpers fixed this. |
| Calibration does not advance | Check the separate touch wiring, especially T_IRQ and T_CS. |
| Notes disappear after reboot | Expected until flash persistence is implemented. |
| Port 8080/1883 is occupied | An existing server may already be running; see the server README before stopping it. |

If the computer LAN IP changes, rerun `setup_lan.py --host NEW_IP`, restart the
server, rerun the firewall script, then rerun tablet setup and flash. Updating
only the firmware IP does not change the server's listening address.

## Validation and development

Server integration tests use temporary databases and MQTT clients:

```powershell
cd C:\Users\messm\OneDrive\Desktop\2040Bluetooth\LocalMQTT
.\.venv\Scripts\python.exe -m unittest -v test_server
.\.venv\Scripts\python.exe check_connection.py
```

The connection check requires the real server running. It verifies authenticated
subscription on its LAN address without editing tasks; it does not prove the
Windows firewall admits traffic from another device.

Host UI/parser tests in WSL, from `Code` (managed cJSON is fetched by ESP-IDF
during the firmware build):

```bash
gcc -std=gnu11 -Itests/stubs -Imain tests/ui_test.c main/ui.c -o /tmp/tablet-ui-test
/tmp/tablet-ui-test
gcc -std=gnu11 -Imain -Imanaged_components/espressif__cjson/cJSON tests/todo_test.c main/todo_model.c managed_components/espressif__cjson/cJSON/cJSON.c -o /tmp/tablet-todo-test
/tmp/tablet-todo-test
```

Previously verified: ESP32 MQTT firmware build; UI navigation, input suppression,
pagination, note restore, and idle tests; task-parser tests; four server tests
covering CRUD, revisions, validation, retained delivery, restart, authentication,
topic permissions, and presence. Hardware tests remain necessary after flashing.

## Next milestones

1. Confirm Wi-Fi/MQTT task delivery on the physical tablet.
2. Design direct BLE task transfer and an iPhone companion app for text/dictation.
3. Add shared task IDs, acknowledgments, and conflict handling before two-way edits.
4. Add BLE Wi-Fi configuration and real clock/timezone synchronization.
5. Add flash persistence for calibration, tasks, and note pages when ready.
6. Add multiple notes and export; handwriting-to-text is a later feature.

The earlier design is retained in [`Code/PLAN.md`](Code/PLAN.md). Its proposed
cloud/web-app and Mosquitto architecture is not the current deployment; today
the broker is embedded aMQTT and the browser editor runs locally on the PC.
