idf_component_register(
    SRCS "main.c" "display.c" "net.c" "portal.c"
    INCLUDE_DIRS "." "../.."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_client esp_http_server
             esp-tls esp_timer driver json mbedtls
    EMBED_FILES "idle.rgb565"
)
