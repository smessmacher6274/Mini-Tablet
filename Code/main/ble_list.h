#pragma once
#include <stdbool.h>
#include <stddef.h>

void ble_list_start(void);
// Nonblocking; copies the item. Rejects controls and empty/oversized text.
bool ble_list_valid(const char *text, size_t length);
bool ble_list_submit(const char *text, size_t length, unsigned number);
