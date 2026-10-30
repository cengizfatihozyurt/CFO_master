// Cihaza ozel gizli ayarlar. Bu dosya GitHub'a GONDERILMEZ (.gitignore).
// Ilk kurulumda secrets.example.h dosyasini secrets.h olarak kopyalayip doldurun.
#pragma once

// Wi-Fi
#define SECRET_WIFI_SSID   "WIFI_ADI"
#define SECRET_WIFI_PASS   "WIFI_SIFRESI"

// Istege bagli ikinci Wi-Fi (ör. telefon hotspotu); baglanti yoksa sirayla denenir
//#define SECRET_WIFI2_SSID  "HOTSPOT_ADI"
//#define SECRET_WIFI2_PASS  "HOTSPOT_SIFRESI"

// MQTT broker. Her agdan erisim icin bulut broker (HiveMQ Cloud, TLS):
//   "mqtts://<cluster>.s1.eu.hivemq.cloud:8883"; bilgisayardaki Mosquitto koprusu veriyi
//   buradan alir (ESP32/mosquitto/kopru_hivemq.conf.example).
// Yalnizca ev aginda yerel broker: "mqtt://<bilgisayar IP>:1883"
#define SECRET_MQTT_URI    "mqtts://<cluster>.s1.eu.hivemq.cloud:8883"
#define SECRET_MQTT_USER   "MQTT_KULLANICI"
#define SECRET_MQTT_PASS   "MQTT_SIFRESI"

// Bulut API (Render). Bos birakilirsa API'ye gonderim kapali olur.
// Ornek: "https://stm32iot-api.onrender.com"
#define SECRET_API_URL     "https://<servis-adi>.onrender.com"
#define SECRET_API_KEY     "RENDER_API_KEY_ILE_AYNI"
