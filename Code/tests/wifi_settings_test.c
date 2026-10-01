#include "wifi_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    wifi_settings_t settings = {0};
    assert(!wifi_settings_password(&settings, "password", 8));
    assert(wifi_settings_ssid(&settings, "Home WiFi", 9));
    assert(!wifi_settings_ready(&settings));
    assert(wifi_settings_password(&settings, "password", 8));
    assert(wifi_settings_ready(&settings));
    assert(!strcmp(settings.ssid, "Home WiFi"));
    assert(wifi_settings_ssid(&settings, "Other", 5));
    assert(!wifi_settings_ready(&settings) && settings.password[0] == 0);
    assert(!wifi_settings_password(&settings, "short", 5));
    assert(!wifi_settings_password(&settings, "bad\0pass", 8));
    assert(!wifi_settings_password(&settings, "password\n", 9));
    assert(wifi_settings_password(&settings, "password", 8));
    assert(!wifi_settings_ssid(&settings, "", 0));
    assert(!wifi_settings_ready(&settings));
    assert(!wifi_settings_ssid(&settings, "bad\0ssid", 8));
    char max[65]; memset(max, 'x', sizeof(max));
    assert(!wifi_settings_ssid(&settings, max, 33));
    assert(wifi_settings_ssid(&settings, max, 32));
    assert(settings.ssid[32] == 0);
    assert(!wifi_settings_password(&settings, max, 64));
    assert(wifi_settings_password(&settings, max, 63));
    assert(settings.password[63] == 0);
    wifi_settings_clear(&settings);
    const unsigned char *bytes = (const unsigned char *)&settings;
    for (size_t n = 0; n < sizeof(settings); ++n) assert(bytes[n] == 0);
    puts("Wi-Fi staging, validation, bounds and clearing tests passed");
}
