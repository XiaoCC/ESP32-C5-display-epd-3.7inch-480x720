#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include "screen_rotation.h"

bool app_display_request(screen_mode_t mode);
void app_display_next(void);
void app_display_wifi(void);
void app_display_wifi_off(void);
void app_display_schedule_changed(void);
bool app_display_refresh_weather(void);
void app_display_trip_data_changed(void);

#endif
