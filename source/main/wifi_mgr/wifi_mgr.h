#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define WIFI_AP_SSID     "Eink E6 3.7"
#define WIFI_AP_CHANNEL  36
#define WIFI_AP_URL      "http://192.168.4.1"

typedef enum {
    WIFI_MGR_MODE_AP = 0,
    WIFI_MGR_MODE_STA,
} wifi_mgr_mode_t;

typedef void (*wifi_mgr_ready_cb_t)(void);

esp_err_t wifi_mgr_start(void);
esp_err_t wifi_mgr_show_hotspot(void);
esp_err_t wifi_mgr_weather_connect(unsigned timeout_ms);
typedef enum {
    WIFI_LEASE_MANAGEMENT, WIFI_LEASE_SYNC, WIFI_LEASE_WEB, WIFI_LEASE_COUNT,
} wifi_lease_reason_t;
void wifi_mgr_acquire(wifi_lease_reason_t reason);
void wifi_mgr_release(wifi_lease_reason_t reason);
bool wifi_mgr_has_leases(void);
void wifi_mgr_stop(void);
bool wifi_mgr_management_active(void);
wifi_mgr_mode_t wifi_mgr_mode(void);
bool wifi_mgr_is_sta_up(void);
bool wifi_mgr_has_credentials(void);
bool wifi_mgr_internet_confirmed(void);
void wifi_mgr_confirm_internet(void);
esp_err_t wifi_mgr_get_ip(char *buf, size_t len);
esp_err_t wifi_mgr_get_ssid(char *buf, size_t len);
esp_err_t wifi_mgr_service_url(char *buf, size_t len);
esp_err_t wifi_mgr_scan_json(char *buf, size_t len);
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);
esp_err_t wifi_mgr_clear_creds(void);
void wifi_mgr_set_ready_cb(wifi_mgr_ready_cb_t cb);

#endif /* WIFI_MGR_H */
