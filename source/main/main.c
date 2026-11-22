#include "board.h"
#include "app_display.h"
#include "dashboard.h"
#include "page_data.h"
#include "esp_netif_sntp.h"
#include "axp2101.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nfc_driver.h"
#include "pcf8563.h"
#include "msa321.h"
#include "pin_map.h"
#include "screen_driver.h"
#include "screen_ui.h"
#include "screen_rotation.h"
#include "tf_card.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";
static TaskHandle_t s_screen_task;
static screen_mode_t s_requested_mode = SCREEN_MODE_COUNT;
static bool s_management;
static bool s_management_sntp;
static bool s_management_synced;

#define US_PER_MINUTE (60LL * 1000000)
#define MANAGEMENT_IDLE_MS 180000

#define SCREEN_EVENT_NETWORK  (1u << 0)
#define SCREEN_EVENT_POWER_TOGGLE (1u << 1)
#define SCREEN_EVENT_MODE (1u << 2)
#define SCREEN_EVENT_NEXT (1u << (2u + SCREEN_MODE_COUNT))
#define SCREEN_EVENT_WIFI (1u << (3u + SCREEN_MODE_COUNT))
#define SCREEN_EVENT_POWER_SHORTCUT (1u << (4u + SCREEN_MODE_COUNT))
#define SCREEN_EVENT_WAKE (1u << (5u + SCREEN_MODE_COUNT))
#define SCREEN_EVENT_WEATHER_UPDATE (1u << (6u + SCREEN_MODE_COUNT))
#define SCREEN_EVENT_WIFI_OFF (1u << (7u + SCREEN_MODE_COUNT))
#define SCREEN_EVENT_TRIP_DATA (1u << (8u + SCREEN_MODE_COUNT))
#define POWER_DOUBLE_CLICK_TICKS pdMS_TO_TICKS(450)

void app_display_wifi(void)
{
    if (s_screen_task) xTaskNotify(s_screen_task, SCREEN_EVENT_WIFI, eSetBits);
}

void app_display_wifi_off(void)
{
    if (s_screen_task) xTaskNotify(s_screen_task, SCREEN_EVENT_WIFI_OFF, eSetBits);
}

void app_display_next(void)
{
    if (s_screen_task) xTaskNotify(s_screen_task, SCREEN_EVENT_NEXT, eSetBits);
}

void app_display_schedule_changed(void)
{
    if (s_screen_task) xTaskNotify(s_screen_task, SCREEN_EVENT_WAKE, eSetBits);
}

static void on_lan_ready(void)
{
    if (wifi_mgr_management_active()) app_display_schedule_changed();
}

bool app_display_request(screen_mode_t mode)
{
    if (!s_screen_task || mode >= SCREEN_MODE_COUNT) return false;
    __atomic_store_n(&s_requested_mode, mode, __ATOMIC_RELEASE);
    return xTaskNotify(s_screen_task, SCREEN_EVENT_MODE, eSetBits) == pdPASS;
}

bool app_display_refresh_weather(void)
{
    return s_screen_task &&
        xTaskNotify(s_screen_task, SCREEN_EVENT_WEATHER_UPDATE, eSetBits) == pdPASS;
}

void app_display_trip_data_changed(void)
{
    if (s_screen_task && screen_rotation_current_mode() == SCREEN_MODE_TRIP)
        xTaskNotify(s_screen_task, SCREEN_EVENT_TRIP_DATA, eSetBits);
}

static void lock_idle_display(void)
{
    for (;;) {
        while (web_server_screen_busy()) vTaskDelay(pdMS_TO_TICKS(25));
        screen_rotation_lock();
        screen_rotation_local_busy(true);
        if (!web_server_screen_busy()) return;
        screen_rotation_local_busy(false);
        screen_rotation_unlock();
    }
}

static void unlock_idle_display(void)
{
    screen_rotation_local_busy(false);
    screen_rotation_unlock();
}

static void refresh_weather(bool force)
{
    dashboard_weather_use_cache();
    const int64_t now = esp_timer_get_time();
    if ((!force && (dashboard_weather_offline() || !wifi_mgr_is_sta_up())) ||
            (!dashboard_weather_claim_today() && !force)) {
        ESP_LOGI(TAG, "Weather cache selected");
        return;
    }
    const bool temporary = !s_management;
    const int64_t deadline = now + 15000000;
    esp_err_t err = wifi_mgr_weather_connect(5000);
    bool sync_started = false;
    if (err == ESP_OK) {
        if (temporary) {
            sync_started = dashboard_clock_sync_start();
            struct tm local;
            if (sync_started && !dashboard_local_time(&local))
                (void)esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1200));
        }
        err = dashboard_weather_fetch(deadline);
    }
    if (sync_started) dashboard_clock_sync_stop();
    if (temporary) wifi_mgr_stop();

    if (err != ESP_OK) ESP_LOGW(TAG, "Weather cached after network attempt: %s", esp_err_to_name(err));
    else (void)dashboard_weather_claim_today();
}

static void draw_mode(screen_mode_t mode)
{
    lock_idle_display();
    const esp_err_t err = screen_rotation_show_mode(mode);
    unlock_idle_display();
    if (err != ESP_OK) ESP_LOGW(TAG, "display mode %u: %s", (unsigned)mode, esp_err_to_name(err));
}

static void show_mode(screen_mode_t mode)
{
    if (mode == SCREEN_MODE_WEATHER) refresh_weather(false);
    draw_mode(mode);
}

static void IRAM_ATTR pmic_irq_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)arg, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t setup_pmic_irq(void)
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = gpio_set_intr_type(pmic_irq, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) return err;
    err = gpio_isr_handler_add(pmic_irq, pmic_irq_handler, xTaskGetCurrentTaskHandle());
    if (err != ESP_OK) return err;
    err = axp2101_pwron_init();
    if (err != ESP_OK) {
        (void)gpio_isr_handler_remove(pmic_irq);
        return err;
    }
    /* Service a latched event even if IRQ was already low before registration. */
    if (gpio_get_level(pmic_irq) == 0) {
        xTaskNotifyGive(xTaskGetCurrentTaskHandle());
    }
    ESP_LOGI(TAG, "AXP2101 IRQ on GPIO%d, falling edge", (int)pmic_irq);
    return ESP_OK;
}

static void power_key_task(void *arg)
{
    (void)arg;
    const esp_err_t pwron_err = setup_pmic_irq();
    if (pwron_err != ESP_OK) {
        ESP_LOGE(TAG, "PWRON init failed: %s", esp_err_to_name(pwron_err));
        vTaskDelete(NULL);
        return;
    }

    bool pending = false;
    TickType_t first = 0;
    while (1) {
        TickType_t wait = portMAX_DELAY;
        if (pending) {
            const TickType_t elapsed = xTaskGetTickCount() - first;
            wait = elapsed < POWER_DOUBLE_CLICK_TICKS ? POWER_DOUBLE_CLICK_TICKS - elapsed : 0;
        }
        const bool irq = ulTaskNotifyTake(pdTRUE, wait) != 0;
        const TickType_t now = xTaskGetTickCount();
        if (pending && now - first >= POWER_DOUBLE_CLICK_TICKS) {
            xTaskNotify(s_screen_task, SCREEN_EVENT_POWER_SHORTCUT, eSetBits);
            pending = false;
        }
        if (!irq) continue;
        bool pressed;
        const esp_err_t err = axp2101_take_pwron_short_press(&pressed);
        if (pressed) {
            if (pending) {
                xTaskNotify(s_screen_task, SCREEN_EVENT_POWER_TOGGLE, eSetBits);
                pending = false;
            } else {
                first = now;
                pending = true;
            }
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "PWRON read failed: %s", esp_err_to_name(err));
        if (gpio_get_level(pmic_irq) == 0) {
            vTaskDelay(pdMS_TO_TICKS(err == ESP_OK ? 10 : 100));
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }
    }
}

static void screen_task(void *arg)
{
    (void)arg;
    screen_mode_t page_before_management = SCREEN_MODE_CALENDAR;
    dashboard_schedule_t previous = {0};
    int64_t last_switch = esp_timer_get_time();
    int64_t quote_retry_at = 0;
    bool pairing = false;
    bool weather_checked_on_link = false;
    int page_sync_date = -1;
    int last_trip_cleanup = -1;
    while (1) {
        int64_t wait_ms = INT32_MAX;
        const int64_t before_wait = esp_timer_get_time();
        const bool hidden = screen_rotation_hidden();
        struct tm day;
        const bool clock_valid = dashboard_local_time(&day);
        if (clock_valid) {
            const int date = day.tm_year * 400 + day.tm_yday;
            if (date != last_trip_cleanup) {
                const bool changed_day = last_trip_cleanup >= 0;
                last_trip_cleanup = date;
                const bool pruned = page_trip_prune(&day);
                if ((changed_day || pruned) && !hidden && screen_rotation_current_mode() == SCREEN_MODE_TRIP)
                    draw_mode(SCREEN_MODE_TRIP);
            }
        }
        if (previous.running && !hidden && !pairing && !screen_rotation_shortcut_active()) {
            wait_ms = (last_switch + previous.every * US_PER_MINUTE - before_wait) / 1000;
        }
        if (clock_valid) {
            const int64_t midnight = (86400 - day.tm_hour * 3600 - day.tm_min * 60 - day.tm_sec) * 1000LL;
            if (midnight < wait_ms) wait_ms = midnight;
        }
        if (s_management) {
            const int64_t idle_left = (int64_t)MANAGEMENT_IDLE_MS - web_server_idle_ms();
            const int64_t lease = idle_left > 0 ? idle_left : (web_server_screen_busy() ? 1000 : 0);
            if (lease < wait_ms) wait_ms = lease;
            struct tm local;
            if (wifi_mgr_is_sta_up() && dashboard_local_time(&local)) {
                const int64_t midnight = (86400 - local.tm_hour * 3600 - local.tm_min * 60 - local.tm_sec) * 1000LL;
                if (midnight < wait_ms) wait_ms = midnight;
            }
        }
        const screen_mode_t displayed = screen_rotation_current_mode();
        if (!hidden && !pairing && (displayed == SCREEN_MODE_QUOTE || displayed == SCREEN_MODE_HISTORY)) {
            struct tm local;
            int64_t quote_wait = 60000;
            if (dashboard_local_time(&local)) {
                quote_wait = dashboard_quote_due()
                    ? (quote_retry_at - before_wait) / 1000
                    : (86400 - local.tm_hour * 3600 - local.tm_min * 60 - local.tm_sec) * 1000LL;
            }
            if (quote_wait < wait_ms) wait_ms = quote_wait;
        }
        uint32_t events = 0;
        if (__atomic_load_n(&s_requested_mode, __ATOMIC_ACQUIRE) < SCREEN_MODE_COUNT) wait_ms = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, wait_ms == INT32_MAX ? portMAX_DELAY :
            pdMS_TO_TICKS(wait_ms > 10 ? wait_ms : 10));
        const bool power = events & SCREEN_EVENT_POWER_TOGGLE;
        if (s_management && !s_management_synced && wifi_mgr_is_sta_up()) {
            s_management_sntp = dashboard_clock_sync_start();
            s_management_synced = true;
        }
        if (s_management && ((events & (SCREEN_EVENT_WIFI | SCREEN_EVENT_WIFI_OFF)) ||
                (web_server_idle_ms() >= MANAGEMENT_IDLE_MS && !web_server_screen_busy()))) {
            if (events & SCREEN_EVENT_WIFI_OFF) vTaskDelay(pdMS_TO_TICKS(300));
            events &= ~SCREEN_EVENT_WIFI;
            if (s_management_sntp) dashboard_clock_sync_stop();
            s_management = s_management_synced = s_management_sntp = false;
            wifi_mgr_stop();
            if (pairing && !screen_rotation_hidden() && screen_rotation_current_mode() == SCREEN_MODE_COUNT) {
                if (page_before_management == SCREEN_MODE_WEATHER) dashboard_weather_use_cache();
                draw_mode(page_before_management);
            }
            pairing = false;
            last_switch = esp_timer_get_time();
        }
        if (!wifi_mgr_is_sta_up()) weather_checked_on_link = false;
        if (s_management && wifi_mgr_is_sta_up()) {
            struct tm local;
            if (!weather_checked_on_link && s_management_sntp && !dashboard_local_time(&local))
                (void)esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1200));
            if (dashboard_local_time(&local)) {
                const bool new_link = !weather_checked_on_link;
                const int date = local.tm_year * 400 + local.tm_yday;
                weather_checked_on_link = true;
                if (new_link) refresh_weather(false);
                if (new_link || date != page_sync_date) {
                    page_sync_date = date;
                    page_network_refresh();
                }
            }
        }
        dashboard_schedule_t schedule;
        dashboard_schedule_get(&schedule);
        if (power) {
            lock_idle_display();
            const esp_err_t err = screen_rotation_toggle_blank();
            unlock_idle_display();
            if (err == ESP_OK) ESP_LOGI(TAG, "PWRON double click: white screen or restored");
            else ESP_LOGW(TAG, "PWRON display: %s", esp_err_to_name(err));
            last_switch = esp_timer_get_time();
            continue;
        }
        if (events & SCREEN_EVENT_POWER_SHORTCUT) {
            lock_idle_display();
            const screen_mode_t page = page_shortcut_get();
            const unsigned other_pages = schedule.enabled & ~(1U << page);
            screen_mode_t fallback = pairing ? page_before_management : screen_rotation_next_mode(other_pages);
            if (fallback == SCREEN_MODE_COUNT || fallback == page) fallback = page == SCREEN_MODE_CALENDAR ? SCREEN_MODE_TODO : SCREEN_MODE_CALENDAR;
            const esp_err_t err = screen_rotation_toggle_shortcut(page, fallback);
            unlock_idle_display();
            if (err != ESP_OK) ESP_LOGW(TAG, "PWRON shortcut: %s", esp_err_to_name(err));
            else ESP_LOGI(TAG, "PWRON single click: display %u", (unsigned)screen_rotation_current_mode());
            pairing = false;
            last_switch = esp_timer_get_time();
            continue;
        }
        if (events & SCREEN_EVENT_NETWORK) {
            const unsigned local_pages = schedule.enabled & ~(1U << SCREEN_MODE_WEATHER);
            const screen_mode_t first = screen_rotation_next_mode(local_pages);
            show_mode(first == SCREEN_MODE_COUNT ? SCREEN_MODE_CALENDAR : first);
            ESP_LOGI(TAG, "Boot page shown with Wi-Fi idle");
            last_switch = esp_timer_get_time();
        }
        if (events & SCREEN_EVENT_WIFI) {
            if (wifi_mgr_show_hotspot() == ESP_OK) {
                s_management = true;
                const screen_mode_t page = screen_rotation_current_mode();
                if (page < SCREEN_MODE_COUNT) page_before_management = page;
                lock_idle_display();
                pairing = screen_ui_show_wifi_qr(WIFI_AP_SSID, WIFI_AP_URL) == ESP_OK;
                unlock_idle_display();
                web_server_note_activity();
                ESP_LOGI(TAG, "Temporary Wi-Fi management ready");
            }
            continue;
        }
        if (events & SCREEN_EVENT_WEATHER_UPDATE) {
            pairing = false;
            refresh_weather(true);
            draw_mode(SCREEN_MODE_WEATHER);
            last_switch = esp_timer_get_time();
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (schedule.running != previous.running || schedule.enabled != previous.enabled ||
                schedule.every != previous.every) {
            previous = schedule;
            last_switch = now;
        }
        const int64_t interval = (int64_t)schedule.every * 60 * 1000000;
        const bool due = schedule.running && !screen_rotation_shortcut_active() &&
            now - last_switch >= interval;
        const screen_mode_t requested = __atomic_exchange_n(&s_requested_mode, SCREEN_MODE_COUNT, __ATOMIC_ACQ_REL);
        const bool manual = requested < SCREEN_MODE_COUNT;
        if ((events & SCREEN_EVENT_NEXT) || manual) pairing = false;
        if ((events & SCREEN_EVENT_NEXT) || (due && now - last_switch >= interval && !manual && !pairing &&
                !screen_rotation_hidden())) {
            last_switch = now;
            screen_rotation_lock();
            const screen_mode_t next = screen_rotation_next_mode(schedule.enabled);
            screen_rotation_unlock();
            if (next != SCREEN_MODE_COUNT &&
                    ((events & SCREEN_EVENT_NEXT) || next != screen_rotation_current_mode() ||
                     next == SCREEN_MODE_WEATHER || next == SCREEN_MODE_PHOTO ||
                     next == SCREEN_MODE_CALENDAR)) {
                ESP_LOGI(TAG, "%s: next display mode %u",
                    (events & SCREEN_EVENT_NEXT) ? "BOOT single click" : "carousel timer", (unsigned)next);
                show_mode(next);
                last_switch = esp_timer_get_time();
            }
        }

        if (manual && (screen_rotation_available_modes() & (1u << requested))) {
            show_mode(requested);
            last_switch = esp_timer_get_time();
        } else if (!manual && (events & SCREEN_EVENT_TRIP_DATA) &&
                !screen_rotation_hidden() && screen_rotation_current_mode() == SCREEN_MODE_TRIP) {
            draw_mode(SCREEN_MODE_TRIP);
        }
        const screen_mode_t current = screen_rotation_current_mode();
        if (!pairing && !screen_rotation_hidden() && !web_server_screen_busy() &&
                (current == SCREEN_MODE_QUOTE || current == SCREEN_MODE_HISTORY) &&
                now >= quote_retry_at && dashboard_quote_due()) {
            quote_retry_at = now + 60 * 1000000;
            show_mode(current);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Eink E6 3.7  ESP32-C5");
    esp_err_t board_err = board_init();

    if (board_err != ESP_OK) {
        ESP_LOGE(TAG, "board_init: %s, retry", esp_err_to_name(board_err));
        vTaskDelay(pdMS_TO_TICKS(200));
        board_err = board_init();
    }

    if (board_err != ESP_OK) {
        ESP_LOGE(TAG, "board_init failed: %s", esp_err_to_name(board_err));
        return;
    }
    /* Optional I2C peripherals share the bus and lock created by board_init. */
    esp_err_t rtc_err = pcf8563_init();
    if (rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed: %s", esp_err_to_name(rtc_err));
    }

    esp_err_t imu_err = msa321_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed: %s", esp_err_to_name(imu_err));
    } else {
        msa321_sample_t sample;
        imu_err = msa321_read(&sample);
        if (imu_err == ESP_OK) {
            ESP_LOGI(TAG, "IMU acceleration x=%.3f y=%.3f z=%.3f g",
                     (double)sample.x_g, (double)sample.y_g, (double)sample.z_g);
        } else {
            ESP_LOGW(TAG, "IMU read failed: %s", esp_err_to_name(imu_err));
        }
    }

    esp_err_t tf_err = tf_card_mount();

    if (tf_err != ESP_OK) {
        ESP_LOGW(TAG, "TF mount failed: %s  NFC will log only", esp_err_to_name(tf_err));
    }
    dashboard_quote_init();
    const esp_err_t pages = page_data_init();
    if (pages != ESP_OK) ESP_LOGW(TAG, "SD page services: %s", esp_err_to_name(pages));

    esp_err_t nfc_err = nfc_driver_init();

    if (nfc_err != ESP_OK) {
        ESP_LOGW(TAG, "NFC init failed: %s", esp_err_to_name(nfc_err));
    }

    ESP_ERROR_CHECK(screen_driver_init());
    ESP_ERROR_CHECK(wifi_mgr_start());
    dashboard_display_style_restore();
    wifi_mgr_set_ready_cb(on_lan_ready);
    dashboard_clock_start();
    dashboard_weather_restore();
    ESP_ERROR_CHECK(screen_rotation_init());
    ESP_ERROR_CHECK(web_server_start());
    if (xTaskCreate(screen_task, "screen_ui", 10240, NULL, 3, &s_screen_task) != pdPASS) {
        ESP_LOGE(TAG, "screen task; key controls unavailable");
    } else {
        if (xTaskCreate(power_key_task, "power_key", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "power key task unavailable");
        }
        xTaskNotify(s_screen_task, SCREEN_EVENT_NETWORK, eSetBits);
    }

    ESP_LOGI(TAG, "Offline display ready; hold BOOT for Wi-Fi management");
}
