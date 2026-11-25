#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

enum {
    NET_SYNC_WEATHER = 1u << 0,
    NET_SYNC_HISTORY = 1u << 1,
    NET_SYNC_TRIPS = 1u << 2,
    NET_SYNC_TRIP_OPEN = 1u << 3,
    NET_SYNC_MANUAL = 1u << 4,
    NET_SYNC_LINK = 1u << 6,
    NET_SYNC_ALL = NET_SYNC_WEATHER | NET_SYNC_HISTORY | NET_SYNC_TRIPS,
};

esp_err_t net_sync_start(void);
void net_sync_request(uint32_t tasks);
void net_sync_clock_valid(void);
bool net_sync_expired(void);
bool net_sync_busy(void);
esp_err_t net_sync_open_management(void);
bool net_sync_close_management(void);
esp_err_t net_sync_set_window(unsigned start, unsigned end);
int net_sync_status(char *out, size_t size);
