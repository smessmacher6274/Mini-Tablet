# Portable task entry over BLE

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
calibration, then open To-do. This is a firmware-only change; no server restart
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
- Each successful write adds one task, including repeated identical titles.
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
