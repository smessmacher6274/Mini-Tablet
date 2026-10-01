#include "wifi_settings.h"
#include <string.h>

void wifi_settings_clear(wifi_settings_t *settings) {
    volatile unsigned char *p = (volatile unsigned char *)settings;
    for (size_t n = 0; n < sizeof(*settings); ++n) p[n] = 0;
}

bool wifi_settings_ssid(wifi_settings_t *settings, const char *value, size_t length) {
    // Starting a new SSID invalidates the previous password, even on error.
    wifi_settings_clear(settings);
    if (!value || !length || length > 32) return false;
    for (size_t n = 0; n < length; ++n)
        if ((unsigned char)value[n] < 32 || value[n] == 127) return false;
    memcpy(settings->ssid, value, length);
    settings->have_ssid = true;
    return true;
}

bool wifi_settings_password(wifi_settings_t *settings, const char *value, size_t length) {
    memset(settings->password, 0, sizeof(settings->password));
    settings->have_password = false;
    if (!settings->have_ssid || !value || length < 8 || length > 63) return false;
    for (size_t n = 0; n < length; ++n)
        if ((unsigned char)value[n] < 32 || (unsigned char)value[n] > 126) return false;
    memcpy(settings->password, value, length);
    settings->have_password = true;
    return true;
}

bool wifi_settings_ready(const wifi_settings_t *settings) {
    return settings->have_ssid && settings->have_password;
}
