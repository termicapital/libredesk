idf_component_register(
    SRCS "app_main.c" "app_boot_flow.c" "display_port.c" "display_port_flow.c" "ui_task.c" "config_store.c" "provisioning.c" "provisioning_session.c" "provisioning_portal.c" "improv_wifi.c" "mvp_config.c" "wifi_startup_flow.c" "wifi_setup_sequence.c" "wifi_manager.c" "mvp_http_flow.c" "net_task.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES espcoredump esp_event esp_http_server esp_lcd esp_netif esp_wifi esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_usb_serial_jtag esp_lvgl_port esp_system esp_timer esp_http_client esp-tls esp_app_format vfs lwip mbedtls glucose_core runtime_core ui_screens nvs_flash
)

if(COMMAND idf_component_register AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/mvp_secrets.h")
    message(FATAL_ERROR "Missing firmware/main/mvp_secrets.h. Copy firmware/main/mvp_secrets.example.h to that ignored path and replace its invalid placeholders locally.")
endif()
