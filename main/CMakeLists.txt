idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp32-camera esp_http_server esp_http_client mqtt
                       PRIV_REQUIRES esp_wifi nvs_flash esp_https_ota
                       EMBED_TXTFILES "ca_cert.pem")
