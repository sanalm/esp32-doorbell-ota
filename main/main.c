#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"

static const char *TAG = "DUAL_CAM_OTA_SYSTEM";

// Global MQTT client handle
esp_mqtt_client_handle_t mqtt_client = NULL;

// --- Configurations ---
#define WIFI_SSID             CONFIG_WIFI_SSID
#define WIFI_PASS             CONFIG_WIFI_PASSWORD
#define MQTT_BROKER_URL       CONFIG_MQTT_BROKER_URL
#define MQTT_TOPIC_SUB        CONFIG_MQTT_TOPIC_SUB
#define IP_CAM_SNAPSHOT_URL   CONFIG_IP_CAM_SNAPSHOT_URL
#define TELEGRAM_BOT_TOKEN    CONFIG_TELEGRAM_BOT_TOKEN
#define TELEGRAM_CHAT_ID      CONFIG_TELEGRAM_CHAT_ID

#define CURRENT_VERSION "1.0.0"
#define VERSION_JSON_URL "https://raw.githubusercontent.com/sanalm/esp32-doorbell-ota/main/version.json"

// --- Camera Pin Definitions (AI-Thinker) ---
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0      5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

// --- Camera Initialization ---
static esp_err_t init_onboard_camera(void) {
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN, .pin_reset = CAM_PIN_RESET, .pin_xclk = CAM_PIN_XCLK,
        .pin_sscb_sda = CAM_PIN_SIOD, .pin_sscb_scl = CAM_PIN_SIOC, .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6, .pin_d5 = CAM_PIN_D5, .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2, .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC, .pin_href = CAM_PIN_HREF, .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG, .frame_size = FRAMESIZE_VGA, .jpeg_quality = 12,
        // .fb_count = 1, .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .fb_count = 2, .grab_mode = CAMERA_GRAB_LATEST,
        
        // CRITICAL FLAG: Tells the driver to allocate memory inside the PSRAM chip
        .fb_location = CAMERA_FB_IN_PSRAM    };
    return esp_camera_init(&config);
}

// --- Dynamic OTA Task ---
static void ota_update_task(void *pvParameter) {
    char *ota_url = (char *)pvParameter;
    ESP_LOGI(TAG, "Starting OTA update sequence from targeted endpoint: %s", ota_url);

    esp_http_client_config_t http_config = {
        .url = ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach, 
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Firmware verified and written cleanly to alternative partition! Rebooting...");
        free(ota_url);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Upgrade execution sequence encountered errors: %s", esp_err_to_name(ret));
        free(ota_url);
        vTaskDelete(NULL);
    }
}

static void check_and_perform_github_ota(void *pvParameter) {
    ESP_LOGI(TAG, "Checking for updates at %s...", VERSION_JSON_URL);

    esp_http_client_config_t config = {
    .url = VERSION_JSON_URL,
    .timeout_ms = 8000,
    .keep_alive_enable = true,
    
    .crt_bundle_attach = esp_crt_bundle_attach, 
};    

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_open(client, 0);

    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        char buffer[512] = {0};
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer));
        
        if (read_len > 0) {
            // Parse the version JSON
            cJSON *json = cJSON_Parse(buffer);
            if (json != NULL) {
                cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "version");
                cJSON *url = cJSON_GetObjectItemCaseSensitive(json, "url");

                if (cJSON_IsString(version) && cJSON_IsString(url)) {
                    ESP_LOGI(TAG, "Latest Version on GitHub: %s (Local: %s)", version->valuestring, CURRENT_VERSION);

                    // Compare versions. If different, start OTA
                    if (strcmp(version->valuestring, CURRENT_VERSION) != 0) {
                        ESP_LOGI(TAG, "New firmware detected. Starting OTA...");
                        
                        esp_http_client_config_t ota_http_config = {
                            .url = url->valuestring,
                            .crt_bundle_attach = esp_crt_bundle_attach,
                        };
                        
                        esp_https_ota_config_t ota_config = {
                            .http_config = &ota_http_config,
                        };

                        esp_err_t ota_res = esp_https_ota(&ota_config);
                        if (ota_res == ESP_OK) {
                            ESP_LOGI(TAG, "OTA Succeeded! Rebooting...");
                            esp_restart();
                        } else {
                            ESP_LOGE(TAG, "OTA Flash Failed!");
                        }
                    } else {
                        ESP_LOGI(TAG, "Firmware is already up-to-date.");
                    }
                }
                cJSON_Delete(json);
            }
        }
    } else {
        ESP_LOGE(TAG, "Failed to connect to GitHub to check version.");
    }
    
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

// --- Telegram Dispatch Helper ---
static void send_to_telegram(const uint8_t *image_data, size_t image_len, const char *caption, const char *filename) {
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", TELEGRAM_BOT_TOKEN);
    const char *boundary = "---ESP32CamBoundary---";
    char header_buf[512];
    int header_len = snprintf(header_buf, sizeof(header_buf),
        "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"%s\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n", boundary, TELEGRAM_CHAT_ID, boundary, caption, boundary, filename);

    char footer_buf[64];
    int footer_len = snprintf(footer_buf, sizeof(footer_buf), "\r\n--%s--\r\n", boundary);
    size_t total_post_len = header_len + image_len + footer_len;

    char content_length_str[16]; snprintf(content_length_str, sizeof(content_length_str), "%d", total_post_len);
    char content_type_str[128];  snprintf(content_type_str, sizeof(content_type_str), "multipart/form-data; boundary=%s", boundary);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        
        // ==== CRITICAL FIX FOR TELEGRAM / GLOBAL HTTPS ====
        .crt_bundle_attach = esp_crt_bundle_attach, // Automatically loads trust bundle
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client) {
        esp_http_client_set_header(client, "Content-Type", content_type_str);
        esp_http_client_set_header(client, "Content-Length", content_length_str);
        if (esp_http_client_open(client, total_post_len) == ESP_OK) {
            esp_http_client_write(client, header_buf, header_len);
            esp_http_client_write(client, (const char *)image_data, image_len);
            esp_http_client_write(client, footer_buf, footer_len);
            esp_http_client_fetch_headers(client);
        }
        esp_http_client_cleanup(client);
    }
}

// --- Action Routine: Camera Pipeline ---
static void execute_dual_camera_capture(void) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        send_to_telegram(fb->buf, fb->len, "📸 Onboard view!", "esp32_cam.jpg");
        esp_camera_fb_return(fb);
    }
    esp_http_client_config_t config = { .url = IP_CAM_SNAPSHOT_URL, .method = HTTP_METHOD_GET, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        if (esp_http_client_open(client, 0) == ESP_OK) {
            int content_length = esp_http_client_fetch_headers(client);
            if (content_length > 0) {
                uint8_t *buffer = malloc(content_length);
                if (buffer) {
                    int read_len = esp_http_client_read(client, (char *)buffer, content_length);
                    if (read_len > 0) {
                        send_to_telegram(buffer, read_len, "📹 Network IP view!", "ip_cam.jpg");
                    }
                    free(buffer);
                }
            }
        }
        esp_http_client_cleanup(client);
    }
}


// This task runs asynchronously, waiting briefly before triggering the hard reset
static void reboot_task(void *pvParameter) {
    ESP_LOGI(TAG, "Reboot task active. Prepared to restart in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000)); // Delay for 2000ms (2 seconds)
    
    ESP_LOGW(TAG, "Rebooting hardware NOW!");
    esp_restart(); // Triggers a hardware reset of the ESP32
}

/* HTTP GET Handler for /reboot */
esp_err_t reboot_get_handler(httpd_req_t *req) {
    ESP_LOGW(TAG, "Reboot request received from client.");

    // 1. Send a friendly HTML response back to the client first
    const char *resp_str = "<html><body><h1>ESP32-CAM is rebooting...</h1><p>Please wait 10 seconds before reloading.</p></body></html>";
    httpd_resp_send(req, resp_str, strlen(resp_str));

    // 2. Spawn an independent, low-priority task to perform the reboot
    // This allows this HTTP handler function to return cleanly and close the socket connection safely.
    xTaskCreate(&reboot_task, "reboot_task", 2048, NULL, 1, NULL);

    return ESP_OK;
}

/* URI Structure Mapping the /reboot path to the handler */
httpd_uri_t uri_reboot = {
    .uri      = "/reboot",
    .method   = HTTP_GET,
    .handler  = reboot_get_handler,
    .user_ctx = NULL
};

/* HTTP GET Handler for /snapshot */
esp_err_t snapshot_get_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;

    ESP_LOGI(TAG, "Taking a snapshot...");

    // 1. Fetch the frame from the camera driver
    fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 2. Set the response content type to JPEG image
    res = httpd_resp_set_type(req, "image/jpeg");
    if (res != ESP_OK) {
        esp_camera_fb_return(fb);
        return res;
    }

    // 3. Set cache control headers so the browser doesn't cache the snapshot
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, private, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    // 4. Send the raw frame buffer bytes directly as the payload
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

    // 5. CRITICAL: Always return the buffer to the pool to prevent memory leaks!
    esp_camera_fb_return(fb);
    
    ESP_LOGI(TAG, "Snapshot sent successfully!");
    return res;
}

/* URI Structure Mapping the /snapshot path to the handler */
httpd_uri_t uri_snapshot = {
    .uri      = "/snapshot",
    .method   = HTTP_GET,
    .handler  = snapshot_get_handler,
    .user_ctx = NULL
};

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        
        // 1. THIS IS WHERE SUBSCRIPTIONS BELONG:
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Transport Connected! Safe to subscribe now.");
            
            // The client is officially connected here, so this will NOT throw an error
            int msg_id = esp_mqtt_client_subscribe(client, MQTT_TOPIC_SUB, 1);
            ESP_LOGI(TAG, "Sent subscribe successful, msg_id=%d", msg_id);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Broken Connection Link. Retrying automatically...");
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT Message Received!");
        if (strncmp(event->topic, MQTT_TOPIC_SUB, event->topic_len) == 0) {
            
            // Check if incoming MQTT payload contains an instruction to upgrade firmware
            // Expected JSON/string frame layout format: "OTA:https://192.168.0.15:8070/firmware.bin"
            if (event->data_len > 4 && strncmp(event->data, "OTA:", 4) == 0) {
                int url_len = event->data_len - 4;
                char *target_url = malloc(url_len + 1);
                if (target_url) {
                    memcpy(target_url, event->data + 4, url_len);
                    target_url[url_len] = '\0';
                    
                    // Fire off the OTA flash process in a separate task thread
                    xTaskCreate(&ota_update_task, "ota_update_task", 8192, (void*)target_url, 5, NULL);
                }
            } else {
                // Default action: Handle image gathering
                execute_dual_camera_capture();
            }
        }
            break;

        default:
            break;
    }
}

httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    // The ESP32-CAM needs a slightly larger stack size to handle large image transfers
    config.stack_size = 8192; 
    config.server_port = 80;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting webserver...");
    if (httpd_start(&server, &config) == ESP_OK) {

        // Register the snapshot URI handler
        httpd_register_uri_handler(server, &uri_snapshot);
        
        // Register the reboot URI handler
        httpd_register_uri_handler(server, &uri_reboot);
        
        return server;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}

static void network_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi radio started. Connecting to AP...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from Wi-Fi. Reconnecting...");
        // If we lose Wi-Fi, stop the MQTT client from hammering the network
        if (mqtt_client) {
            esp_mqtt_client_stop(mqtt_client); 
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Successfully bound to local IP: " IPSTR, IP2STR(&event->ip_info.ip));

        xTaskCreate(&check_and_perform_github_ota, "check_and_perform_github_ota", 8192, NULL, 5, NULL);
        
        // Network layer is officially live! Safe to start the MQTT transport socket now
        if (mqtt_client) {
            ESP_LOGI(TAG, "Starting MQTT client connect sequence...");
            esp_mqtt_client_start(mqtt_client);
        }

        start_webserver();
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    // 1. Register for BOTH Wi-Fi and IP events under one consolidated handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &network_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &network_event_handler, NULL, NULL));
    
    // 2. Set up your Wi-Fi configurations and start the radio
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start()); // <-- This starts Wi-Fi asynchronously

    // 3. Prepare the MQTT client configuration, but leave it idle
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_BROKER_URL };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    // Note: Removed esp_mqtt_client_start() from here!
    ESP_LOGI(TAG, "System initialized. Waiting for IP assignment before launching MQTT...");

    ESP_ERROR_CHECK(init_onboard_camera());
}

