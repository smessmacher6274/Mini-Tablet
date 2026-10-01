#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    char ssid[33];
    char password[64];
    bool have_ssid;
    bool have_password;
} wifi_settings_t;
bool wifi_settings_ssid(wifi_settings_t *settings, const char *value, size_t length);
bool wifi_settings_password(wifi_settings_t *settings, const char *value, size_t length);
bool wifi_settings_ready(const wifi_settings_t *settings);
void wifi_settings_clear(wifi_settings_t *settings);
