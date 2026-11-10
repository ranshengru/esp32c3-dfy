idf_component_register(
    SRCS "main.c" "display.c" "net.c" "portal.c" "sonar.c" "mood.c" "assets.c" "lz4/lz4.c" "idle_lz4s.c"
    INCLUDE_DIRS "." "../.." "lz4"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_client esp_http_server
             esp-tls esp_timer driver json mbedtls
)
