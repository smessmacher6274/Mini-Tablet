# Note tablet roadmap

## Product flow

Boot -> touch calibration -> idle clock -> tap -> menu -> To-do or Notes.
Home returns to menu; Clock returns to idle. After 60 seconds without touch,
return to the clock. Preserve the current note across screen changes. This
idle screen is a UI state, not deep sleep: the existing backlight wiring stays on.

The owner uses an iPhone and has no existing MQTT infrastructure.

Local computer milestone implemented in `../LocalMQTT/`: browser CRUD, SQLite
persistence, retained MQTT state and restart/duplicate/conflict tests. The first
broker is embedded aMQTT to avoid an additional Docker/Mosquitto installation.
It binds only to loopback; ESP32 subscription and authenticated LAN access remain
next steps. This changes the initial broker implementation, not the MQTT format.

## Architecture

iPhone web app -> HTTPS API -> synchronization service + SQLite database
                                      |
                                  MQTT broker
                                      |
                              ESP32 over Wi-Fi

Start with Mosquitto and a small service on the development computer on the
same LAN. The host must remain running and reachable. Later move them to an
always-on machine or hosted service for access away from home. Do not expose
an unauthenticated broker publicly. Use per-device credentials/topic ACLs and
TLS outside an isolated local development setup. BLE remains available during
development; MQTT uses Wi-Fi, not the existing BLE greeting characteristic.

Use an iPhone-friendly web app first (add to Home Screen if desired). Text
entry can use the iPhone keyboard's dictation. No browser SpeechRecognition
dependency for the first release. Native iOS app/background notifications are
a later decision. Drawings stay separate from tasks until the user approves
recognized text. Handwriting recognition runs on the server/phone, not ESP32.

## Milestones and acceptance

1. **Device navigation foundation (current implementation)**
   - Clock state, tap-to-menu, To-do screen, Notes with Write/Erase/Home.
   - Honest unset clock until a real time source exists.
   - Monochrome note buffer survives menu/idle changes in RAM, not power loss.
   - No fake synchronized tasks; empty To-do view until storage/sync is added.
   - Check tap release does not activate the next screen; toolbar cannot be drawn on.
2. **Offline data and time**
   - Save calibration with an explicit recalibration path.
   - Task IDs, list IDs, title, completion, revision; local flash persistence.
   - Persist note pages (1-bit bitmap initially, versioned format).
   - Wi-Fi setup, SNTP UTC time, explicit timezone/DST setting; resync after reboot.
   - Verify reboot preservation, missing Wi-Fi, full storage and write failures.
3. **Local broker and durable synchronization**
   - Mosquitto plus a small service and SQLite, reproducible local startup.
   - Add ESP-MQTT managed component for ESP-IDF 6.x.
   - Server is authoritative; ESP caches confirmed state and queues offline operations.
   - Read/list, create, rename, complete/reopen and delete tasks and lists.
   - Verify duplicated, delayed, reordered messages and reconnection/reboot.
4. **iPhone input**
   - Responsive list UI: type/dictate, review, submit, then see delivery status.
   - Same account/device association as broker authorization; no credentials in JS.
   - Test iPhone on same LAN, then design remote access before deployment.
5. **Notes, then recognition (last)**
   - Multiple persistent pages, export/upload and backup, explicit clear/delete.
   - Recognition returns editable draft text; never overwrites the original image.
   - User explicitly chooses whether to turn recognized text into tasks.

## Proposed MQTT contract (design, not yet implemented)

Prefix: `notepad/v1/devices/<device-id>/`.

| Topic | Publisher | Semantics |
| --- | --- | --- |
| commands | ESP -> server | Non-retained QoS 1 operations with operation_id |
| acknowledgements | server -> ESP | Non-retained QoS 1 result + operation_id |
| state | server -> ESP | Retained QoS 1 versioned snapshot for small bounded lists |
| presence | ESP -> server | Retained online status, offline Last Will |

Each operation includes schema_version, operation_id, entity_id, operation type,
and base_revision. Server applies each operation_id once. Completion uses explicit
`completed: true/false`, never a toggle command that could repeat. Detect stale
base revisions and return a conflict instead of silently losing edits. Device
ignores older snapshots. UI distinguishes locally pending vs server-confirmed.
Set limits for number of tasks, UTF-8 title bytes and message size; reject invalid
payloads. Assemble fragmented MQTT events before parsing. Large note images use
HTTP upload or a separate chunk protocol, not unbounded retained state payloads.

## Implementation constraints

ESP32-WROOM-32E, 320x480 ST7796S, XPT2046 touch, no assumed PSRAM.
Only the UI task draws/reads touch. Network callbacks deliver bounded queued
events to the UI. Avoid blocking networking on flash writes or full-screen redraw.
Use a 1-bit canvas (~17 KB) rather than a 307 KB RGB framebuffer.
Don't erase all NVS automatically once real user data is stored there.

References:
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mqtt.html
- https://mosquitto.org/documentation/
