#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
bool web_server_screen_busy(void);
void web_server_note_activity(void);
uint32_t web_server_idle_ms(void);

esp_err_t web_server_start(void);

#endif /* WEB_SERVER_H */
