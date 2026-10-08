# Portable task entry over BLE

## Current Bluetooth stability test

October 7 follow-up: phone Bluetooth reset did not restore service discovery
after a failed link. Heap totals alone cannot rule out fixed packet-pool
exhaustion. Test mode now logs `Host probe` and all registered `Pool` free/minimum
counts every ten seconds while connected, plus pool counts at connect/disconnect
and MTU negotiation. RSSI is valid only when `rssi_rc=0`. A continuing host probe
shows the host event queue is running; it does not prove ATT traffic is flowing.
Capture a complete fresh boot, successful writes, failure, and reconnect without
power cycling so pool recovery and service discovery can be compared.

October 6 read-stress observation: with Wi-Fi/MQTT disabled and no encryption,
three writes succeeded before rapid reads preceded a supervision timeout.
Subsequent links timed out in about 1.1 seconds until power cycling. The initial
connection used a 30ms interval and only a 720ms supervision timeout. Writes
after the power cycle were reported stable (eight by user, seven in pasted log).
This narrows the reproduction but does not establish why radio packets stopped.

Firmware now requests a 30-45ms interval, zero latency and 6000ms supervision
timeout on each successful connection. The phone may reject or alter the
request: verify `Connection update status: 0` and the following link parameters
(`timeout=600` means six seconds). A longer timeout tolerates brief stalls; it
does not repair a stalled controller. Read diagnostics count greeting/status
requests and response-allocation errors, sample heap/host stack availability,
and report counts at disconnect. Status reads use the BLE-owned task count,
avoiding the task model mutex. No task text is included in these diagnostics.

Next hardware check: first leave idle connected, then read f1 repeatedly,
then f3 repeatedly, then mix reads and writes. Record which characteristic
triggers failure, the timing update, read stats, and disconnect summary. After
a failure, stop reads and reconnect without resetting; verify task count remains.

`main/test_mode.h` defaults `TABLET_BLE_TEST_MODE` to 1. Boot opens the task
list directly, skips calibration, disables uncalibrated touch, and prevents
the idle clock transition. Set it to 0 and rebuild to restore normal operation.
Wi-Fi and MQTT are disabled in this mode; the status reads `BLUETOOTH ONLY`.
Encrypted Wi-Fi credential characteristics f4-f7 are omitted, so task testing
does not require pairing. Forget the previous NoteTablet-BLE entry in phone
Bluetooth settings once, then reconnect and rediscover services in nRF Connect.
Use f2 to write tasks and f3 to read status. Do not request pairing for this test.

Identical successful task writes repeated within two seconds return `OK RETRY`
without adding a second entry. Wait at least two seconds to intentionally add
the same title again. Failed writes are never remembered as successful retries.
This guards rapid repeated writes; it does not remove duplicate text inside a
single payload or merge identical tasks from MQTT.

Advertising restart failures now log and retry on the host event queue instead
of aborting. Encryption failures and disconnect reasons appear in serial logs.
Task entry does not require pairing; Wi-Fi configuration still requires it.

Hardware checks: send two different short tasks, read f3 to verify count 2,
repeat a title rapidly to check `OK RETRY`, and leave connected for 30 minutes.
Repeat with phone Wi-Fi on/off. Disconnect
and reconnect ten times without resetting the board. Test credential pairing
separately. Capture serial output around any disconnect, reset, or failed
reconnection. Logs include connection interval, supervision timeout and
encryption/bond status. Connection stability requires hardware verification.

This first version uses an iPhone BLE testing app. No Wi-Fi, MQTT server,
internet, or companion app is needed to add and check off a Bluetooth task.
Normal SMS/iMessage cannot write this service.

## Install the firmware

In an activated WSL ESP-IDF terminal:

```bash
cd /mnt/c/Users/messm/OneDrive/Desktop/2040Bluetooth/Code
idf.py -B build-esp32-wsl -p /dev/ttyUSB0 flash monitor
```

Keep your existing network configuration. Wi-Fi may keep attempting to connect
while away from home; BLE task entry does not wait for it. Finish the three-cross
calibration, then open To-do when test mode is disabled. This is a firmware-only change; no server restart
or server upgrade is needed.

## Send a task from iPhone

1. Install [nRF Connect for Mobile](https://apps.apple.com/us/app/nrf-connect-for-mobile/id1054362403).
2. Enable Bluetooth and allow the app's Bluetooth permission.
3. Scan inside the app and connect to **NoteTablet-BLE**.
4. Open service `12345678-1234-5678-1234-56789abcdef0`.
5. Find writable characteristic `12345678-1234-5678-1234-56789abcdef2`.
6. Choose a **text / UTF-8** value and **Write with response**. Send `Buy milk`.
   Do not use hexadecimal mode, JSON, or append a newline.
7. The task appears before the computer's tasks, with a small **BLE** label.
   Tap its checkbox to complete/reopen it while offline.
8. Optionally read `12345678-1234-5678-1234-56789abcdef3` as text. Success is
   `OK RAM 1/20` (the count grows as tasks are added).

Start with a short title such as `Buy milk`. Firmware accepts up to 120 UTF-8
bytes and requests an ATT MTU of 185. The negotiated connection/app write limit
may be smaller. If a longer write is rejected by the app, shorten the title;
**do not split it into separate writes**, because each write creates a task.

If the new characteristics do not appear after flashing, disconnect, reconnect,
and rediscover services (or restart the testing app). Only one BLE connection
is supported at a time. The existing greeting characteristic still works.

## Configure Wi-Fi over Bluetooth

After flashing this version, connect in nRF Connect and use the same service.
Write each value as **text/UTF-8**, with **Write with response**, in this order:

| Characteristic UUID | Write |
| --- | --- |
| `12345678-1234-5678-1234-56789abcdef4` | Your exact 2.4 GHz Wi-Fi network name (SSID) |
| `12345678-1234-5678-1234-56789abcdef5` | Your Wi-Fi password |
| `12345678-1234-5678-1234-56789abcdef6` | `APPLY` (uppercase) |
| `12345678-1234-5678-1234-56789abcdef7` | Read this characteristic as text for status |

The credential/control characteristics require link encryption. Accept the
iPhone pairing prompt if shown, or use the testing app's pair/encrypt action
and retry the write if it reports insufficient encryption. No PIN is configured:
this uses Just Works pairing with no persistent bond. Bluetooth stays running
while the ESP32 stops and restarts its Wi-Fi connection.

SSID and password are write-only and never returned in status or application
logs. Use a 1–32-byte SSID and an 8–63-character printable ASCII passphrase.
This first version targets WPA2/WPA3 personal networks; open, WEP, enterprise,
and captive-portal setup are not supported. Do not add quotes or line breaks.
If your app limits writes to 20 bytes, negotiate a larger MTU/use a long write;
do not split an SSID or password into independent writes.

The status progresses through `SSID READY`, `PASSWORD READY`, `WIFI APPLYING`,
`WIFI CONNECTING`, and `MQTT CONNECTING` or `MQTT CONNECTED`.
`MQTT CONNECTING` means Wi-Fi has an IP address but the broker is not connected.
After repeated Wi-Fi failures it shows `WIFI FAILED RETRY`; correct the settings
and apply again. `ERR SSID`, `ERR PASSWORD`, or `ERR LENGTH` rejects invalid
input; `BUSY RETRY APPLY` means the network change could not yet be queued.
An accepted APPLY confirms queuing, not successful association.

Start with SSID each time: changing it clears the staged password. Disconnecting
BLE before APPLY clears both staged fields. After APPLY, credentials remain only
in the Wi-Fi driver's RAM. **Reset restores the compiled network settings**;
saving Wi-Fi credentials to flash is deferred along with other persistence.

This does not change the MQTT broker address or credentials. A new Wi-Fi network
must be able to reach the configured computer/server for MQTT to work. Bluetooth
task entry continues to work independently, even with the wrong Wi-Fi password.

Encryption protects the credential link, but Just Works pairing does not verify
the owner's identity or provide man-in-the-middle protection. Physical approval
or authenticated provisioning is still needed for a finished product.

## What is stored and synchronized

- Up to **20 Bluetooth tasks**, separate from the server's maximum of 50.
- Each successful write adds one task except identical retries within two seconds.
- BLE tasks and their completion state are stored in **RAM only**. Disconnecting
  the phone or reconnecting Wi-Fi keeps them; resetting/powering off loses them.
- Incoming MQTT snapshots cannot erase or replace Bluetooth tasks.
- Bluetooth tasks are **not uploaded to the PC** in this version.
- Computer-created tasks still require MQTT for completion changes.
- Completing a Bluetooth task does not free a slot. Deletion and persistent
  storage are later features; the limit returns an error instead of losing data.

## Protocol

All characteristic UUIDs share prefix `12345678-1234-5678-1234-56789abcde`:

| Suffix | Operation | Meaning |
| --- | --- | --- |
| `f1` | Read | Original greeting: `What it do` |
| `f2` | Write with response | One plain UTF-8 task title, 1–120 bytes |
| `f3` | Read | Last write result and current local count |

Spaces at the ends are trimmed. Empty/space-only values, NUL/control characters,
line breaks, invalid UTF-8, and oversized writes are rejected. Unsupported font
characters display as `?`, although valid UTF-8 is kept in memory.

An ATT write success means the task was accepted into RAM, not flash and not
the server. Status values are `READY`, `OK RAM`, `ERR LENGTH`, `ERR TEXT`, or
`ERR FULL`, followed by count/capacity. Invalid text returns application ATT
error `0x80`; full storage returns insufficient resources. Status is read-only,
not a notification stream. There is no task-list download characteristic yet.

Task entry does not require pairing or owner authorization. Wi-Fi credential
writes require encryption as described above. Owner authorization is not yet
implemented; nearby clients can connect to this prototype service.

## Verification

Host tests exercise text limits, UTF-8 validation, local completion, duplicate
completion protection, list capacity, and preservation across server snapshots:

```bash
gcc -std=gnu11 -Wall -Wextra -Imain tests/local_tasks_test.c main/local_tasks.c -o /tmp/tablet-local-test
/tmp/tablet-local-test
```

Hardware acceptance: send a task away from the configured Wi-Fi network, tick
and reopen it, disconnect/reconnect the phone, then return to Wi-Fi and confirm
both lists remain. This requires checking on the actual iPhone and tablet.

## Companion app later

Native Swift/Core Bluetooth development normally uses Xcode on a Mac. A
cross-platform app can be authored on Windows and built for iOS by a hosted
macOS service, such as [Expo EAS](https://docs.expo.dev/build/setup/), using a
native BLE library and a development build. That still needs appropriate Apple
signing/provisioning; the generic Expo Go app is not a substitute for a custom
native BLE build. The GATT interface above can be used by either approach.
