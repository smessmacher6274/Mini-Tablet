# Pico W Bluetooth with CMake

## ST7796S display hello world

The firmware now draws `Hello World!` on two lines in 320x480 portrait with red, green, and blue
test bars, then starts the same BLE service. Display initialization takes about
two seconds. Touch is not enabled. Drawing runs once before radio startup;
the existing BLE polling loop remains unchanged. Future live screen updates
must be broken into short operations so BLE continues to be serviced.

Wiring: CS=GP17 (22), SCK=GP18 (24), MOSI=GP19 (25), RESET=GP20 (26),
DC=GP21 (27). Parentheses indicate Pico physical pins. For the previously
identified 3.3–5V module, VCC=VBUS (40), GND=GND (38), LED=3V3 OUT (36).
MISO, touch, and SD connections are unused. Power off before changing wiring.

Rebuild from your existing Bash environment:

```bash
cmake --build build --parallel
```

Flash `build/picow_bluetooth.uf2` using BOOTSEL. Verify the text and RGB bars,
then connect to `PicoW-BLE` and read the greeting as before. Disconnect and
reconnect to check BLE remains functional. If only the backlight appears,
check CS/DC/RESET/MOSI/SCK against the labels and confirm the ST7796S driver.
The SPI clock is reduced to 1 MHz in `display.c` to provide more margin with
jumper wires. This is a diagnostic mitigation, not a confirmed white-screen fix.
There is no display timeout or clear operation after the startup image.
If it turns white, check whether BLE still responds, and whether a full USB
power cycle restores the image. With power disconnected, reseat VCC, ground,
RESET (GP20/physical 26), CS, and the SPI connections. A lit backlight alone
does not confirm that the LCD controller has stable power or is out of reset.

This starter targets the **Raspberry Pi Pico W / WH (RP2040 + CYW43439)**.
The RP2040 chip itself has no radio; a different RP2040 board needs a compatible
radio and driver. This is a BLE peripheral, not Bluetooth Classic serial or audio.
It advertises as `PicoW-BLE` and exposes a readable `Hello from Pico W!` value.

## Windows prerequisites

Install CMake, Ninja, Python 3, Git, and the Arm GNU bare-metal toolchain
(`arm-none-eabi-gcc`). Put their executables on PATH and reopen PowerShell.
Use the Arm toolchain, not the Windows/MSVC compiler. The Raspberry Pi Pico
VS Code extension can also provision the Pico development tools.

## Configure and build

Run from this project folder. Use a current stable Pico SDK with its submodules:

```powershell
git clone --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
$env:PICO_SDK_PATH = (Resolve-Path .\pico-sdk).Path
cmake -S . -B build -G Ninja -DPICO_BOARD=pico_w
cmake --build build --parallel
```

If you already have the SDK, set `PICO_SDK_PATH` to that folder instead of cloning.
For an existing incomplete checkout, run:

```powershell
git -C "$env:PICO_SDK_PATH" submodule update --init --recursive
```

CMake generates `service.h` from `service.gatt` using Python; do not create it
manually. `pico_btstack_ble` supplies BLE, `pico_btstack_cyw43` connects BTstack
to the radio, and `pico_cyw43_arch_none` provides radio integration without Wi-Fi.

## Flash and verify

1. Hold BOOTSEL while plugging the Pico W into USB. Release when `RPI-RP2` appears.
2. Copy `build/picow_bluetooth.uf2` to that drive. The board reboots automatically.
3. Open a BLE GATT scanner on your phone, scan, and connect to `PicoW-BLE`.
   Use the scanner's connection flow; operating-system Bluetooth pairing is not needed.
4. Open service `12345678-1234-5678-1234-56789abcdef0` and read characteristic
   `12345678-1234-5678-1234-56789abcdef1`. Decode as UTF-8 to see the greeting.
5. Disconnect and scan again to verify reconnection.

USB serial logging is enabled. Startup logs can occur before a terminal connects;
BLE advertising does not wait for a terminal. This demo has no pairing requirement
and its test value is publicly readable while connected.

## Troubleshooting

- `cmake`, `ninja`, or `arm-none-eabi-gcc` not recognized: install the missing
  prerequisite or add its `bin` directory to PATH.
- Missing BTstack/CYW43 files: initialize SDK submodules and check `PICO_SDK_PATH`.
- Changed SDK, compiler, or board: configure into a fresh build directory.
- Device absent: confirm it is a Pico W, use a data-capable USB cable, and check
  phone Bluetooth/scanner permissions. The ordinary Pico has no onboard Bluetooth.

## References and validation

- SDK: https://github.com/raspberrypi/pico-sdk
- Official Bluetooth examples: https://github.com/raspberrypi/pico-examples
- SDK networking libraries: https://www.raspberrypi.com/documentation/pico-sdk/networking.html

The display-plus-BLE firmware was successfully compiled using the WSL Arm
toolchain in `build-display/`. The ready-to-flash file is
`build-display/picow_bluetooth.uf2`. Hardware behavior still needs verification
on the connected screen and Pico W. The SDK emits an existing ENABLE_BLE
redefinition warning; it does not prevent the build.
