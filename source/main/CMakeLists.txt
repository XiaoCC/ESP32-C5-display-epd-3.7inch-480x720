idf_component_register(
    SRCS
        "main.c"
        "net_sync/net_sync.c"
        "board/board.c"
        "pmic/axp2101.c"
        "rtc/pcf8563.c"
        "imu/msa321.c"
        "nfc_driver/nfc_driver.c"
        "screen_driver/screen_driver.c"
        "screen_driver/screen_rotation.c"
        "screen_ui/screen_ui.c"
        "dashboard/dashboard_data.c"
        "dashboard/page_data.c"
        "dashboard/dashboard_draw.c"
        "qrcode/qrcode.c"
        "tf_card/tf_card.c"
        "wifi_mgr/wifi_mgr.c"
        "web_server/web_server.c"
    INCLUDE_DIRS
        "."
        "board"
        "pmic"
        "rtc"
        "imu"
        "nfc_driver"
        "screen_driver"
        "screen_ui"
        "dashboard"
        "qrcode"
        "tf_card"
        "wifi_mgr"
        "net_sync"
        "web_server"
    PRIV_REQUIRES
        json
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c
        esp_common
        esp_partition
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
        esp_http_client
        mbedtls
        lwip
        fatfs
        sdmmc
    EMBED_FILES
        "dashboard/quote_font.bin"
        "dashboard/L2026.TSV"
        "dashboard/L2027.TSV"
        "dashboard/lunar_web.gz"
        "dashboard/history_seed.tsv"
        "web_server/www/index.html"
        "web_server/www/eink_worker.js"
        "web_server/www/weather.js"
        "web_server/www/place_catalog.gz"
        "web_server/www/eink_pipeline.js"
)


# 压缩网页资源，保留可直接编辑和审查的文本源码。
idf_build_get_property(idf_python PYTHON)
foreach(asset IN ITEMS app.css app.js vue.global.prod.js)
    set(input "${CMAKE_CURRENT_LIST_DIR}/web_server/www/${asset}")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${idf_python}" "${CMAKE_CURRENT_LIST_DIR}/web_server/gzip_asset.py" "${input}" "${output}"
        DEPENDS "${input}" "${CMAKE_CURRENT_LIST_DIR}/web_server/gzip_asset.py"
        VERBATIM
    )
    target_add_binary_data(${COMPONENT_LIB} "${output}" BINARY)
endforeach()
