// Cihaza ozel gizli ayarlar. Bu dosya GitHub'a GONDERILMEZ (.gitignore).
// Ilk kurulumda secrets.example.h dosyasini secrets.h olarak kopyalayip doldurun.
#pragma once

// Wi-Fi
#define SECRET_WIFI_SSID   "WIFI_ADI"
#define SECRET_WIFI_PASS   "WIFI_SIFRESI"

// MQTT broker (Node-RED / Grafana icin)
#define SECRET_MQTT_URI    "mqtt://192.168.1.100:1883"
#define SECRET_MQTT_USER   "esp32"
#define SECRET_MQTT_PASS   "MQTT_SIFRESI"

// Bulut API (Render). Bos birakilirsa API'ye gonderim kapali olur.
// Ornek: "https://stm32iot-api.onrender.com"
#define SECRET_API_URL     "https://<servis-adi>.onrender.com"
#define SECRET_API_KEY     "RENDER_API_KEY_ILE_AYNI"
