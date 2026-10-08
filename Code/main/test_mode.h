#pragma once

// Temporary BLE-only soak test: no Wi-Fi/MQTT or credential characteristics.
// Set to 0 to restore networking and calibrated touch/idle UI.
#ifndef TABLET_BLE_TEST_MODE
#define TABLET_BLE_TEST_MODE 1
#endif
