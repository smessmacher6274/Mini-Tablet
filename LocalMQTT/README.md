# Local MQTT to-do desk

A local browser UI + SQLite task store + a real MQTT 3.1.1 broker (aMQTT).
Add/manage tasks from this computer and receive them on the ESP32 over Wi-Fi.
The tablet displays task titles and completion state with Prev/Next paging.
Task editing is on the computer for now; iPhone access comes later.

## Connect the tablet (Windows server + WSL firmware)

LAN credentials have been provisioned for this computer at `192.168.1.148`.
The server must run with `--lan` (or double-click `run.cmd`). Keep it running.
If the computer's address changes, run `.\.venv\Scripts\python.exe setup_lan.py
--host NEW_IP`, restart the server, update the firewall, and rerun tablet setup.

Allow the connection once in **Administrator PowerShell**:

```powershell
cd C:\Users\messm\OneDrive\Desktop\2040Bluetooth\LocalMQTT
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\allow-mqtt.ps1
```

The rule allows TCP 1883 only to the configured local IP from the local subnet.
The execution policy override applies only to that PowerShell process.

Then in **WSL**, with the ESP32 attached as `/dev/ttyUSB0`:

```bash
source ~/esp32/esp-idf/export.sh
cd /mnt/c/Users/messm/OneDrive/Desktop/2040Bluetooth/Code
python3 setup_network.py
idf.py -B build-esp32-wsl -p /dev/ttyUSB0 flash monitor
```

Enter your 2.4 GHz Wi-Fi name and password locally; accept `192.168.1.148`
as the computer IP. The ESP32 and computer must share a LAN without client
isolation. Setup writes ignored `Code/main/network_config.h`; credentials are
also present in the firmware binary, so do not publish either. BLE stays enabled.

After touch calibration, open To-do. Look for `Wi-Fi ready`, `MQTT connected`,
and `Received ... tasks` in the monitor. Add an item at http://localhost:8080.
The browser reports the device's received revision, and the tablet updates.
This receipt means the update reached the tablet's RAM queue, not flash storage.
Exit the monitor with Ctrl+].

Tasks are saved on the PC and resent after reconnection or tablet reboot.
Tablet notes still live only in RAM. Non-ASCII characters display as `?` with
the current bitmap font. The clock is not synchronized by this change.

The original roadmap suggested Mosquitto; this local prototype embeds aMQTT
so Python is the only runtime needed. Standard MQTT clients can subscribe now,
and the broker can be replaced by Mosquitto later without changing the payload.

## Run on Windows (already installed in this workspace)

From a normal PowerShell terminal, no ESP-IDF activation required:

```powershell
cd C:\Users\messm\OneDrive\Desktop\2040Bluetooth\LocalMQTT
.\.venv\Scripts\python.exe server.py --lan
```

Open http://localhost:8080 in a browser. Leave the terminal running; Ctrl+C
stops both HTTP and MQTT. If already running, just open the browser page.
You can also double-click `run.cmd` to start it in a terminal.

During initial setup, Codex started one hidden Windows process and wrote its PID
to `server.pid`; logs are `server.log` and `server-error.log`. To stop that
instance from PowerShell in this folder, first verify it is still this server:

```powershell
$serverProcessId = [int](Get-Content .\server.pid)
Get-CimInstance Win32_Process -Filter "ProcessId=$serverProcessId" |
    Select-Object ProcessId, ExecutablePath, CommandLine
```

If it shows this folder's `.venv\Scripts\python.exe` running `server.py`, use
`taskkill /PID $serverProcessId /T /F` to stop its Python child process too.
Future foreground runs stop with Ctrl+C.

For a fresh installation, Python 3.11+:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe server.py
```

## Run in WSL instead

Use a separate Linux virtual environment (Windows .venv cannot run in WSL):

```bash
cd /mnt/c/Users/messm/OneDrive/Desktop/2040Bluetooth/LocalMQTT
python3 -m venv .venv-wsl
.venv-wsl/bin/python -m pip install -r requirements.txt
.venv-wsl/bin/python server.py
```

Windows normally forwards WSL localhost:8080 to the browser. Run only one
instance against a database at a time; do not run both OS versions together.

## Features

- One Inbox list, up to 50 tasks, titles up to 120 UTF-8 bytes.
- Add, edit, complete/reopen, delete; All / To do / Completed filters.
- SQLite persistence at `data/tasks.sqlite3`, with revisions for stale-edit detection.
- Idempotent adds via request_id; retrying an add does not duplicate a task.
- Retained QoS 1 snapshot on each change; latest saved state is republished on restart.
- Browser distinguishes saved, published, and received-by-tablet revisions.
- No notes/ESP flash persistence changes. The database lives on this computer.

## MQTT and API

LAN broker: `mqtt://192.168.1.148:1883`, MQTT 3.1.1 with generated passwords
and topic permissions. The service publishes state; the tablet can only receive
state and publish its presence/received revision. Credentials are in ignored
`data/connection.json`; password hashes are in `data/passwords.txt`.
MQTT is unencrypted for this trusted home LAN prototype; do not port-forward it.
HTTP remains localhost-only. Without `--lan`, the server uses anonymous
loopback MQTT for computer-only development.

Retained state topic: `notepad/v1/devices/tablet-001/state`

```json
{
  "schema_version": 1,
  "device_id": "tablet-001",
  "list_id": "inbox",
  "revision": 1,
  "items": [{"id": "uuid", "title": "Buy milk", "completed": false,
             "revision": 1, "created_at": "2026-09-29T22:00:00.000Z"}]
}
```

HTTP API at localhost:8080 (JSON requests):

| Method/path | Body |
| --- | --- |
| GET /api/state | none; includes transport publication status |
| POST /api/items | title, request_id (UUID) |
| PATCH /api/items/{id} | base_revision, title and/or completed (boolean) |
| DELETE /api/items/{id} | base_revision |

MQTT is currently the **output** of this API; incoming MQTT task commands and
task edits from ESP32 are not implemented. The SQLite store is authoritative.
The tablet sends retained presence every 15 seconds and after receiving state;
the UI treats it as offline after 45 seconds without a heartbeat or on its will.
Retained messages are reconstructed after a broker/server restart.
There is no cloud dependency or remote service during operation.

## Troubleshooting and tests

If a port is occupied, stop the previous instance or choose different ports:
`python server.py --port 8081 --mqtt-port 1884`.

Tests use temporary databases and an independent real MQTT client:

```powershell
.\.venv\Scripts\python.exe -m unittest -v test_server
```

They cover CRUD, duplicate create requests, stale revisions, input validation,
origin restrictions, task limit, retained MQTT delivery and server restart.
`requirements-lock.txt` records the packages used for the verified Windows run.

Next: device commands need operation IDs,
acknowledgments and conflict handling before two-way synchronization.
