# 📸 ESP32-CAM Smart Doorbell with GitHub OTA & Telegram Alerts

A lightweight, robust ESP-IDF firmware for the ESP32-CAM module. This project acts as a smart video doorbell that detects motion via a hardware PIR sensor, captures a real-time snapshot without stale frames, sends the image to a designated Telegram Group, and supports secure Over-The-Air (OTA) updates hosted on GitHub Releases.

---

## 🚀 Features

* **Anti-Stale Frame Capture:** Configured with double-buffering (`fb_count = 2`) and `CAMERA_GRAB_LATEST` to guarantee you always receive a real-time snapshot.
* **Telegram Group Dispatch:** Sends high-resolution JPEGs directly to a Telegram group using a custom Bot.
* **Automated GitHub OTA:** Securely polls a remote `version.json` on a GitHub repository, compares local/remote firmware versions, and pulls updates directly from GitHub Release assets over HTTPS.
* **On-Demand Web Server:** Hosts a local `/snapshot` HTTP GET endpoint to pull raw camera frames on your local network.

---

## 🛠️ Hardware Requirements

* **ESP32-CAM Module** (with OV2640 camera sensor)

---

## ⚙️ Configuration & Setup

### 1. ESP-IDF Dependencies
To prevent compiler errors, ensure your `main/CMakeLists.txt` registers the necessary components:

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server esp_wifi esp_event esp_camera esp_https_ota)
```

## Environment

### Environment Setup (Debian)
Assumes IDF_PATH="$HOME/workspace/esp/esp-idf"

### configure user credentials 
```get_idf
idf.py menuconfig
```

#### Doorbell Project Configuration --->
```CONFIG_WIFI_SSID="ssid"
CONFIG_WIFI_PASSWORD="password"
CONFIG_MQTT_BROKER_URL="mqtt://ipAddress:1883"
CONFIG_MQTT_TOPIC_SUB="home/doorbell/ind"
CONFIG_IP_CAM_SNAPSHOT_URL="http://ipcamUrl"
CONFIG_TELEGRAM_BOT_TOKEN="token"
CONFIG_TELEGRAM_CHAT_ID="chatId"	
```
#### Build firmware
```cd ~/workspace/doorbell
get_idf
idf.py build
```

#### Flash OTA
Reboot ESP

Firmware version is compared on boot, if firmware version is different, the new firmware is loaded

Update version in version.json in ~/workspace/doorbell
```idf.py build
```

Drop in 
```build/doorbell.bin``` 
and rename to 
```firmware.bin```

### Flash over serial connection
```On ESP-CAM, momentarily depress IO0 (which shorts it to GND)
cd ~/workspace/doorbell
get_idf
idf.py fullclean
idf.py build flash monitor -p /dev/ttyUSB0  
```

## API
### HTTP GET
```http://<your-esp32-ip>/snapshot
http://<your-esp32-ip>/reboot
```

## TODO
-