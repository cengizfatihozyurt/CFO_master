/*
 * ESP32 - STM32F103 sensor alicisi + web paneli + veri kaydi
 *
 * STM32 USART2 (115200 8N1) her saniye su satiri gonderir:
 *   T:25.06,VX:0.012,VY:0.010,VZ:0.015,V:0.022\r\n
 *   T  : DS18B20 sicaklik (C), okunamazsa "ERR"
 *   VX/VY/VZ : eksen bazinda titresim RMS (g)
 *   V  : toplam titresim RMS (g)
 *
 * Baglanti (ESP32 Serial2 varsayilan pinleri):
 *   ESP32 GPIO16 (RX2) <- STM32 PA2 (USART2_TX)
 *   ESP32 GPIO17 (TX2) -> STM32 PA3 (USART2_RX)
 *   GND ortak olmali
 *
 * Ozellikler:
 *   - Wi-Fi'ye baglanir, IP adresini Serial'e yazar, http://<IP>/ panelini sunar
 *   - NTP ile saat (Turkiye, UTC+3); kayitlar saat damgali
 *   - Veriler LittleFS'e ikili (16 bayt/kayit) kaydedilir, dolunca en eski silinir
 *   - Sicaklik/titresim set degerleri web'den degistirilir, NVS'de saklanir
 *     (elektrik kesilse de kalir)
 *   - Set asimi ve hatalar kodlu olarak olay kaydina yazilir
 *
 * Hata kodlari:
 *   E01 Sicaklik set degerinin uzerinde      E03 Sicaklik sensoru hatasi
 *   E02 Titresim set degerinin uzerinde      E04 STM32 veri iletisimi kesildi
 *   I01 Set degerleri guncellendi            I02 Kayitlar silindi
 *   I03 Anomali modeli ogrenildi             I04 Otomatik ogrenme baslatildi
 *   I05 Anomali degerleri elle girildi
 *
 * Anomali kodlari (uc birimde tespit, ayrintilar anomali.h):
 *   A01 Ani darbe                            A03 Anormal sefer / uzun titresim
 *   A02 Durusta anormal titresim             A04 Hizli isinma
 *
 * Web uclari:
 *   /            panel            /data      canli degerler (JSON)
 *   /history     grafik verisi    /events    olay listesi (JSON)
 *   /storage     depolama durumu  /log.csv   tum kayitlar (Excel uyumlu)
 *   /events.csv  olay kaydi       /settings  POST: tset, vset, logint
 *   /clear       POST: tum kayitlari sil
 *   /anomaly/auto    POST minutes: otomatik ogrenmeyi N dk ile baslat (/anomaly/reset: mevcut sureyle)
 *   /anomaly/manual  POST mu, sigma, ridemax, peak: normal durum degerlerini elle gir
 *
 * MQTT (bulut, Node-RED ile izleme). Tum konular MQTT_BASE onekiyle:
 *   <base>/status        "online" / "offline" (retained, LWT)
 *   <base>/info          {"ip","rssi","fw"} (retained)
 *   <base>/telemetry     her paket (1 s): {"ts","temp","vx","vy","vz","v","flags","alarms","tset","vset","ast","az"}
 *                        ast: anomali modu (0 ogreniyor, 1 durus, 2 hareket), az: durusa gore z-skoru
 *   <base>/event         her olay: {"ts","code","state","value","limit","desc"} (QoS1)
 *   <base>/settings      guncel set degerleri {"tset","vset","logint"} (retained)
 *   <base>/settings/set  ESP32 dinler: {"tset":..,"vset":..,"logint":..} (eksik alan eski degerde kalir)
 *                        {"learnmin":N} N dk otomatik ogrenme, {"relearn":1} mevcut sureyle,
 *                        {"man_mu","man_sigma","man_ridemax","man_peak"} manuel anomali degerleri
 *
 * Bulut API (Render, api/ klasoru):
 *   Her kayit araliginda yazilan olcumler ve olaylar HTTPS ile API'ye toplu gonderilir.
 *   Gonderilen son kaydin zamani NVS'de tutulur; sunucu kapaliysa veriler LittleFS'te
 *   birikir ve sunucu acilinca eskiden yeniye sirayla gonderilir (sunucu tekrarlari atlar).
 */

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "secrets.h"
#include "anomali.h"
#include <math.h>
#include <vector>
#include <algorithm>

const char *WIFI_SSID = SECRET_WIFI_SSID;
const char *WIFI_PASS = SECRET_WIFI_PASS;

#define STM_RX_PIN   16
#define STM_TX_PIN   17
#define STM_BAUD     115200
#define TIMEOUT_MS   3000   // bu sure veri gelmezse baglanti koptu say

// Loopback testi: 1 yap, STM32'yi ayir, GPIO16 ile GPIO17'yi jumper'la birlestir.
// ESP32 kendi gonderdigi sahte paketi alirsa ESP32 tarafi saglamdir.
#define LOOPBACK_TEST 0

#define TZ_INFO      "<+03>-3"          // Turkiye UTC+3
#define NTP_SERVER1  "pool.ntp.org"
#define NTP_SERVER2  "time.google.com"

#define LOG_DIR             "/log"
#define EVENTS_FILE         "/events.csv"
#define EVENTS_MAX_BYTES    65536        // olay dosyasi bunu asarsa eski yarisi silinir
#define CHUNK_RECORDS       2048         // her log dosyasi 2048 kayit (32 KB)
#define LOG_FS_SHARE        0.85f        // dosya sisteminin loglara ayrilan orani
#define FS_MIN_FREE         16384        // dosya sisteminde birakilacak en az bos alan
#define HISTORY_MAX_POINTS  720          // grafige gonderilecek en fazla nokta
#define EVENTS_MAX_SHOWN    200          // panelde gosterilecek son olay sayisi

#define TEMP_HYST           0.5f         // sicaklik alarmi set-0.5 C altinda biter
#define VIB_HYST_RATIO      0.9f         // titresim alarmi set*0.9 altinda biter

#define DEFAULT_TEMP_SET    40.0f        // C
#define DEFAULT_VIB_SET     0.500f       // g RMS
#define DEFAULT_LOG_INT     10           // s

// MQTT broker: Node-RED'in calistigi bilgisayardaki Mosquitto (sifreli kullanici).
// Bilgisayarin IP'si degisirse MQTT_URI'yi guncelleyin (modemde IP sabitlemek onerilir).
// Bulut broker icin ornek: "mqtts://<adres>:8883" (TLS, sertifika paketi hazir)
#define MQTT_URI    SECRET_MQTT_URI
#define MQTT_USER   SECRET_MQTT_USER
#define MQTT_PASS   SECRET_MQTT_PASS
#define MQTT_BASE   "stm32iot-fa6dbb/cihaz01"   // Node-RED akisindaki onekle ayni olmali
#define FW_VERSION  "1.4"

#define API_URL         SECRET_API_URL
#define API_KEY         SECRET_API_KEY
#define API_DEVICE      "cihaz01"     // API'deki cihaz adi
#define API_BATCH       100           // istek basina en fazla olcum
#define API_EVENT_BATCH 50            // istek basina en fazla olay
#define API_PERIOD_MS   5000          // bekleyen veri yoksa kontrol araligi
#define API_RETRY_MS    30000         // sunucuya ulasilamazsa tekrar deneme araligi

/* ------------------------------------------------------------------------- */

struct SensorData {
  float temperature;   // C
  bool  tempValid;
  float vibX, vibY, vibZ, vibTotal;  // g RMS
  uint32_t lastUpdateMs;
  uint32_t packetCount;
  uint32_t rxBytes;      // tani: Serial2'den gelen toplam bayt
  uint32_t badLines;     // tani: ayristirilamayan satir sayisi
};

struct Settings {
  float    tempSet;      // C
  float    vibSet;       // g
  uint16_t logInterval;  // s
};

// Diskteki kayit: 16 bayt
struct __attribute__((packed)) LogRec {
  uint32_t t;           // epoch (UTC)
  int16_t  tempC100;    // sicaklik * 100, INT16_MIN = gecersiz
  uint16_t vxMg;        // aralik ortalamasi, mg
  uint16_t vyMg;
  uint16_t vzMg;
  uint16_t vMg;         // aralik icindeki en yuksek toplam titresim, mg
  uint8_t  flags;       // aralikta aktif olan alarm bitleri
  uint8_t  reserved;
};
static_assert(sizeof(LogRec) == 16, "LogRec 16 bayt olmali");

struct LogFile {
  uint32_t start;       // dosyadaki ilk kaydin epoch'u (dosya adi)
  uint32_t size;        // bayt
};

// Kayit araligi boyunca toplanan degerler
struct Accum {
  float    tSum;
  uint32_t tN;
  float    vxSum, vySum, vzSum;
  float    vMax;
  uint32_t n;
  uint8_t  flags;
};

// MQTT gorevinden ana donguye aktarilan set komutu
struct MqttMsg {
  char payload[192];
};

// Grafik icin kayit gruplama
struct Bucket {
  uint32_t t;
  float    tSum;
  uint32_t tN;
  float    vx, vy, vz;   // mg toplami
  uint16_t vMax;        // mg
  uint32_t n;
  uint8_t  flags;
};

// AL_SHOCK..AL_TEMPRATE sirasi anomali.h'deki AN_* sirasiyla ayni olmali
enum AlarmId { AL_TEMP, AL_VIB, AL_SENSOR, AL_LINK, AL_SHOCK, AL_IDLEVIB, AL_RIDE, AL_TEMPRATE, AL_COUNT };

struct AlarmDef {
  const char *code;
  const char *desc;
  uint8_t     flag;
};

static const AlarmDef ALARMS[AL_COUNT] = {
  {"E01", "Sıcaklık set değerinin üzerinde",   0x01},
  {"E02", "Titreşim set değerinin üzerinde",   0x02},
  {"E03", "Sıcaklık sensörü hatası (DS18B20)", 0x04},
  {"E04", "STM32 veri iletişimi kesildi",      0x08},
  {"A01", "Anomali: ani darbe",                0x10},
  {"A02", "Anomali: duruşta anormal titreşim", 0x20},
  {"A03", "Anomali: anormal sefer / uzun titreşim", 0x40},
  {"A04", "Anomali: hızlı ısınma (C/dk)",      0x80},
};
static_assert((int)AL_SHOCK + (int)AN_COUNT == (int)AL_COUNT, "anomali kodlari AN_* ile eslesmeli");

SensorData sensor = {0};
Settings   settings = {DEFAULT_TEMP_SET, DEFAULT_VIB_SET, DEFAULT_LOG_INT};
WebServer  server(80);
Preferences prefs;

static bool     alarmActive[AL_COUNT];
static uint32_t alarmSince[AL_COUNT];

#define ANOM_SAVE_MS  600000UL   // ogrenilen normal NVS'ye 10 dk'da bir yazilir

static AnomalyDetector anom;
static uint32_t lastAnomSaveMs = 0;

static std::vector<LogFile> logFiles;   // baslangic zamanina gore sirali
static uint32_t logBytes = 0;
static Accum    acc = {};
static uint32_t lastLogMs = 0;
static bool     streaming = false;      // uzun HTTP yaniti sirasinda log yazma ertelenir
static bool     fsOk = false;

// Log/olay dosyalarina ana dongu ve API gorevi birlikte erisir
static SemaphoreHandle_t fsMutex = nullptr;
struct FsLock {
  FsLock()  { if (fsMutex) xSemaphoreTakeRecursive(fsMutex, portMAX_DELAY); }
  ~FsLock() { if (fsMutex) xSemaphoreGiveRecursive(fsMutex); }
};

// API gonderim durumu (web sayfasinda gosterilir)
static volatile bool     apiOnline = false;
static volatile int      apiLastStatus = 0;     // son HTTP kodu, -1 = baglanti yok
static volatile uint32_t apiPending = 0;        // gonderilmeyi bekleyen olcum
static volatile uint32_t apiSentTotal = 0;
static volatile uint32_t apiLastOk = 0;         // son basarili gonderim (epoch)

static char   lineBuf[128];
static size_t lineLen = 0;

static esp_mqtt_client_handle_t mqtt = nullptr;
static volatile bool mqttConnected = false;
static volatile bool mqttAnnounce = false;   // baglaninca durum/ayar yayinla
static QueueHandle_t mqttRxQueue = nullptr;

static void pumpSerial();

/* --------------------------- Yardimcilar --------------------------------- */

// Saat NTP ile ayarlandiysa epoch, degilse 0
static uint32_t nowEpoch()
{
  time_t t = time(nullptr);
  return (t > 1700000000) ? (uint32_t)t : 0;
}

static void fmtTime(uint32_t t, char *buf, size_t n)
{
  if (!t) {
    strlcpy(buf, "saat yok", n);
    return;
  }
  time_t tt = t;
  struct tm tmv;
  localtime_r(&tt, &tmv);
  strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

// Turkce Excel icin ondalik ayiraci virgul
static void dotToComma(char *s)
{
  for (; *s; s++) {
    if (*s == '.') *s = ',';
  }
}

static uint16_t toMg(float g)
{
  float mg = g * 1000.0f + 0.5f;
  if (mg < 0) mg = 0;
  if (mg > 65535) mg = 65535;
  return (uint16_t)mg;
}

static uint8_t activeFlags()
{
  uint8_t f = 0;
  for (int i = 0; i < AL_COUNT; i++) {
    if (alarmActive[i]) f |= ALARMS[i].flag;
  }
  return f;
}

/* --------------------------- Ayarlar (NVS) ------------------------------- */

static void loadSettings()
{
  prefs.begin("cfg", true);
  settings.tempSet     = prefs.getFloat("tset", DEFAULT_TEMP_SET);
  settings.vibSet      = prefs.getFloat("vset", DEFAULT_VIB_SET);
  settings.logInterval = prefs.getUShort("logint", DEFAULT_LOG_INT);
  prefs.end();
}

static void saveSettings()
{
  prefs.begin("cfg", false);
  prefs.putFloat("tset", settings.tempSet);
  prefs.putFloat("vset", settings.vibSet);
  prefs.putUShort("logint", settings.logInterval);
  prefs.end();
}

// Anomali modeli (ogrenilen normal) elektrik kesilince kaybolmasin
static void loadAnomaly()
{
  uint8_t buf[sizeof(AnomPersist)];
  prefs.begin("anom", true);
  size_t len = prefs.getBytesLength("st");
  size_t n = (len > 0 && len <= sizeof(buf)) ? prefs.getBytes("st", buf, len) : 0;
  prefs.end();
  if (n && anom.load(buf, n)) {
    Serial.printf("Anomali modeli yuklendi: %s, durus mu=%.4f g sigma=%.4f g, %lu sefer\n",
                  anom.isManual() ? "manuel" : (anom.p.learned ? "ogrenilmis" : "ogreniliyor"),
                  anom.mu(), anom.sigma(), (unsigned long)anom.p.nRides);
  } else {
    Serial.printf("Anomali modeli yok, %d sn ogrenme ile baslanacak\n", ANOM_LEARN_S);
  }
}

static void saveAnomaly()
{
  prefs.begin("anom", false);
  prefs.putBytes("st", &anom.p, sizeof(anom.p));
  prefs.end();
  lastAnomSaveMs = millis();
}

/* --------------------------- MQTT ---------------------------------------- */

static String mqttTopic(const char *leaf)
{
  return String(MQTT_BASE "/") + leaf;
}

// QoS1 mesajlar baglanti yokken outbox'ta bekletilir, baglaninca gonderilir.
// QoS0 (telemetri) yalnizca bagliyken gonderilir.
static void mqttPublish(const char *leaf, const char *payload, int qos, bool retain)
{
  if (!mqtt) return;
  String t = mqttTopic(leaf);
  if (qos > 0) {
    esp_mqtt_client_enqueue(mqtt, t.c_str(), payload, 0, qos, retain, true);
  } else if (mqttConnected) {
    esp_mqtt_client_publish(mqtt, t.c_str(), payload, 0, 0, retain);
  }
}

static void jsonFloat(char *buf, size_t n, float v, int dec)
{
  if (isnan(v)) {
    strlcpy(buf, "null", n);
  } else {
    snprintf(buf, n, "%.*f", dec, v);
  }
}

static void mqttPublishSettings()
{
  char j[96];
  snprintf(j, sizeof(j), "{\"tset\":%.2f,\"vset\":%.3f,\"logint\":%u}",
           settings.tempSet, settings.vibSet, settings.logInterval);
  mqttPublish("settings", j, 1, true);
}

static void mqttPublishInfo()
{
  char j[128];
  snprintf(j, sizeof(j), "{\"ip\":\"%s\",\"rssi\":%d,\"fw\":\"%s\"}",
           WiFi.localIP().toString().c_str(), WiFi.RSSI(), FW_VERSION);
  mqttPublish("info", j, 1, true);
}

static void mqttPublishEvent(uint32_t t, const char *code, const char *state,
                             float value, float limit, const char *desc)
{
  char v[16], l[16], j[320];
  jsonFloat(v, sizeof(v), value, 3);
  jsonFloat(l, sizeof(l), limit, 3);
  snprintf(j, sizeof(j),
           "{\"ts\":%lu,\"code\":\"%s\",\"state\":\"%s\",\"value\":%s,\"limit\":%s,\"desc\":\"%s\"}",
           (unsigned long)t, code, state, v, l, desc);
  mqttPublish("event", j, 1, false);
}

static uint8_t activeFlags();

static void mqttPublishTelemetry()
{
  if (!mqttConnected) return;
  char temp[16], j[384], al[64] = "";
  jsonFloat(temp, sizeof(temp), sensor.tempValid ? sensor.temperature : NAN, 2);
  uint8_t flags = activeFlags();
  for (int i = 0; i < AL_COUNT; i++) {
    if (flags & ALARMS[i].flag) {
      if (al[0]) strlcat(al, ",", sizeof(al));
      strlcat(al, "\"", sizeof(al));
      strlcat(al, ALARMS[i].code, sizeof(al));
      strlcat(al, "\"", sizeof(al));
    }
  }
  snprintf(j, sizeof(j),
           "{\"ts\":%lu,\"temp\":%s,\"vx\":%.3f,\"vy\":%.3f,\"vz\":%.3f,\"v\":%.3f,"
           "\"flags\":%u,\"alarms\":[%s],\"tset\":%.2f,\"vset\":%.3f,\"ast\":%u,\"az\":%.2f}",
           (unsigned long)nowEpoch(), temp, sensor.vibX, sensor.vibY, sensor.vibZ, sensor.vibTotal,
           flags, al, settings.tempSet, settings.vibSet, anom.mode, anom.z);
  mqttPublish("telemetry", j, 0, false);
}

// MQTT gorevinde calisir: sadece bayrak/kuyruk gunceller, isi ana dongu yapar
static void mqttEventHandler(void *args, esp_event_base_t base, int32_t id, void *data)
{
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
  static const char setTopic[] = MQTT_BASE "/settings/set";

  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      mqttConnected = true;
      esp_mqtt_client_subscribe(ev->client, setTopic, 1);
      mqttAnnounce = true;
      break;
    case MQTT_EVENT_DISCONNECTED:
      mqttConnected = false;
      break;
    case MQTT_EVENT_DATA:
      if (ev->topic_len == (int)strlen(setTopic) &&
          strncmp(ev->topic, setTopic, ev->topic_len) == 0 &&
          ev->current_data_offset == 0 &&
          ev->data_len < (int)sizeof(MqttMsg::payload)) {
        MqttMsg m;
        memcpy(m.payload, ev->data, ev->data_len);
        m.payload[ev->data_len] = '\0';
        xQueueSend(mqttRxQueue, &m, 0);
      }
      break;
    default:
      break;
  }
}

static void mqttStart()
{
  static char clientId[32];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "esp32-%06llx", (unsigned long long)(mac >> 24) & 0xFFFFFF);

  static const char willTopic[] = MQTT_BASE "/status";
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = MQTT_URI;
  cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;  // mqtts:// icin
  if (MQTT_USER[0]) {
    cfg.credentials.username = MQTT_USER;
    cfg.credentials.authentication.password = MQTT_PASS;
  }
  cfg.credentials.client_id = clientId;
  cfg.session.keepalive = 30;
  cfg.session.last_will.topic = willTopic;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.outbox.limit = 32768;   // baglanti yokken bekletilecek olay mesajlari

  mqttRxQueue = xQueueCreate(4, sizeof(MqttMsg));
  mqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
  esp_mqtt_client_start(mqtt);
  Serial.printf("MQTT: %s, konu oneki %s/\n", MQTT_URI, MQTT_BASE);
}

/* --------------------------- Olay kaydi ---------------------------------- */

// Olay dosyasi cok buyurse eski yarisini at
static void trimEvents()
{
  FsLock lock;
  File f = LittleFS.open(EVENTS_FILE, FILE_READ);
  if (!f) return;
  size_t size = f.size();
  if (size <= EVENTS_MAX_BYTES) {
    f.close();
    return;
  }
  f.seek(size / 2);
  f.readStringUntil('\n');  // yarim satiri atla
  File tmp = LittleFS.open("/events.tmp", FILE_WRITE);
  uint8_t buf[256];
  while (tmp && f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    tmp.write(buf, n);
  }
  f.close();
  if (tmp) {
    tmp.close();
    LittleFS.remove(EVENTS_FILE);
    LittleFS.rename("/events.tmp", EVENTS_FILE);
  }
}

// Satir bicimi: epoch,kod,durum,deger,limit,aciklama  (aciklamada virgul olmaz)
static void appendEvent(const char *code, const char *state, float value, float limit, const char *desc)
{
  uint32_t t = nowEpoch();
  char ts[24];
  fmtTime(t, ts, sizeof(ts));
  Serial.printf("OLAY [%s] %s %s deger=%.3f limit=%.3f %s\n", ts, code, state, value, limit, desc);
  mqttPublishEvent(t, code, state, value, limit, desc);

  if (!fsOk) return;
  FsLock lock;
  File f = LittleFS.open(EVENTS_FILE, FILE_APPEND);
  if (!f) return;
  f.printf("%lu,%s,%s,%.3f,%.3f,%s\n", (unsigned long)t, code, state, value, limit, desc);
  f.close();
  trimEvents();
}

static void setAlarm(int id, bool active, float value, float limit)
{
  if (alarmActive[id] == active) return;
  alarmActive[id] = active;
  alarmSince[id]  = active ? nowEpoch() : 0;
  appendEvent(ALARMS[id].code, active ? "BASLADI" : "BITTI", value, limit, ALARMS[id].desc);
}

static void evaluateAlarms()
{
  setAlarm(AL_LINK, false, NAN, NAN);
  setAlarm(AL_SENSOR, !sensor.tempValid, NAN, NAN);

  if (sensor.tempValid) {
    if (!alarmActive[AL_TEMP] && sensor.temperature > settings.tempSet) {
      setAlarm(AL_TEMP, true, sensor.temperature, settings.tempSet);
    } else if (alarmActive[AL_TEMP] && sensor.temperature <= settings.tempSet - TEMP_HYST) {
      setAlarm(AL_TEMP, false, sensor.temperature, settings.tempSet);
    }
  }

  if (!alarmActive[AL_VIB] && sensor.vibTotal > settings.vibSet) {
    setAlarm(AL_VIB, true, sensor.vibTotal, settings.vibSet);
  } else if (alarmActive[AL_VIB] && sensor.vibTotal <= settings.vibSet * VIB_HYST_RATIO) {
    setAlarm(AL_VIB, false, sensor.vibTotal, settings.vibSet);
  }
}

// Uc birimde anomali tespiti (anomali.h); her STM32 paketinde (1 s) calisir
static void evaluateAnomalies()
{
  bool wasLearned = anom.p.learned;
  anom.update(sensor.vibTotal, sensor.tempValid, sensor.temperature);

  if (!wasLearned && anom.p.learned) {
    char desc[128];
    snprintf(desc, sizeof(desc), "Anomali modeli öğrenildi: duruş titreşimi %.4f g / sigma %.4f g",
             anom.mu(), anom.sigma());
    appendEvent("I03", "BILGI", anom.mu(), anom.sigma(), desc);
    saveAnomaly();
  }
  for (int i = 0; i < AN_COUNT; i++) {
    setAlarm(AL_SHOCK + i, anom.active[i], anom.value[i], anom.limit[i]);
  }
  if (millis() - lastAnomSaveMs > ANOM_SAVE_MS) saveAnomaly();
}

static void clearAnomalyAlarms()
{
  for (int i = 0; i < AN_COUNT; i++) setAlarm(AL_SHOCK + i, false, NAN, NAN);
}

// Otomatik ogrenme: verilen sure (dk) boyunca normal durum yeniden ogrenilir
static bool startAutoLearning(long minutes, const char *source)
{
  if (minutes < ANOM_LEARN_MIN_S / 60 || minutes > ANOM_LEARN_MAX_S / 60) return false;
  anom.startAuto((uint32_t)minutes * 60);
  clearAnomalyAlarms();
  saveAnomaly();
  char desc[96];
  snprintf(desc, sizeof(desc), "Otomatik öğrenme başlatıldı (%s): %ld dk", source, minutes);
  appendEvent("I04", "BILGI", NAN, NAN, desc);
  return true;
}

// Manuel mod: normal durum degerleri kullanicidan, sistem kendiliginden degistirmez
static bool applyManualAnomaly(float mu, float sigma, long rideMaxS, float peakG, const char *source)
{
  if (isnan(mu) || isnan(sigma) || isnan(peakG) || mu < 0 || mu > 2 || sigma < 0.0005f || sigma > 1 ||
      rideMaxS < 5 || rideMaxS > 3600 || peakG <= mu || peakG > 16) {
    return false;
  }
  anom.setManual(mu, sigma, (uint32_t)rideMaxS, peakG);
  clearAnomalyAlarms();
  saveAnomaly();
  char desc[160];
  snprintf(desc, sizeof(desc), "Anomali değerleri elle girildi (%s): duruş %.4f g / sigma %.4f g / sefer en fazla %ld s / tepe en fazla %.3f g",
           source, mu, sigma, rideMaxS, peakG);
  appendEvent("I05", "BILGI", mu, sigma, desc);
  return true;
}

/* --------------------------- Veri kaydi ---------------------------------- */

static String logPath(uint32_t start)
{
  return String(LOG_DIR "/") + start + ".bin";
}

static uint32_t maxLogBytes()
{
  return (uint32_t)(LittleFS.totalBytes() * LOG_FS_SHARE);
}

static void scanLogs()
{
  FsLock lock;
  logFiles.clear();
  logBytes = 0;
  if (!LittleFS.exists(LOG_DIR)) {
    LittleFS.mkdir(LOG_DIR);
  }
  File dir = LittleFS.open(LOG_DIR);
  if (!dir) return;
  File f = dir.openNextFile();
  while (f) {
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.endsWith(".bin")) {
      logFiles.push_back({(uint32_t)strtoul(name.c_str(), nullptr, 10), (uint32_t)f.size()});
      logBytes += f.size();
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  std::sort(logFiles.begin(), logFiles.end(),
            [](const LogFile &a, const LogFile &b) { return a.start < b.start; });
}

// Alan dolunca en eski dosyayi sil (halka tampon)
static void enforceLogLimit()
{
  while (logFiles.size() > 1 &&
         (logBytes > maxLogBytes() ||
          LittleFS.totalBytes() - LittleFS.usedBytes() < FS_MIN_FREE)) {
    LittleFS.remove(logPath(logFiles.front().start));
    logBytes -= logFiles.front().size;
    logFiles.erase(logFiles.begin());
  }
}

static void writeRecord(const LogRec &r)
{
  FsLock lock;
  if (logFiles.empty() || logFiles.back().size >= CHUNK_RECORDS * sizeof(LogRec)) {
    uint32_t start = r.t;
    if (!logFiles.empty() && start <= logFiles.back().start) {
      start = logFiles.back().start + 1;
    }
    logFiles.push_back({start, 0});
  }
  File f = LittleFS.open(logPath(logFiles.back().start), FILE_APPEND);
  if (!f) return;
  size_t w = f.write((const uint8_t *)&r, sizeof(r));
  f.close();
  logFiles.back().size += w;
  logBytes += w;
  enforceLogLimit();
}

static void flushLog()
{
  if (streaming) return;
  if (millis() - lastLogMs < settings.logInterval * 1000UL) return;
  lastLogMs = millis();

  uint32_t now = nowEpoch();
  if (acc.n && now && fsOk) {
    LogRec r = {};
    r.t        = now;
    r.tempC100 = acc.tN ? (int16_t)lroundf(acc.tSum / acc.tN * 100.0f) : INT16_MIN;
    r.vxMg     = toMg(acc.vxSum / acc.n);
    r.vyMg     = toMg(acc.vySum / acc.n);
    r.vzMg     = toMg(acc.vzSum / acc.n);
    r.vMg      = toMg(acc.vMax);
    r.flags    = acc.flags;
    writeRecord(r);
  }
  acc = {};
}

/* --------------------------- STM32 paketleri ---------------------------- */

// "KEY:VALUE" parcasini isle
static void parseField(char *field)
{
  char *sep = strchr(field, ':');
  if (!sep) return;
  *sep = '\0';
  const char *key = field;
  const char *val = sep + 1;

  if (strcmp(key, "T") == 0) {
    if (strcmp(val, "ERR") == 0) {
      sensor.tempValid = false;
    } else {
      sensor.temperature = atof(val);
      sensor.tempValid = true;
    }
  } else if (strcmp(key, "VX") == 0) {
    sensor.vibX = atof(val);
  } else if (strcmp(key, "VY") == 0) {
    sensor.vibY = atof(val);
  } else if (strcmp(key, "VZ") == 0) {
    sensor.vibZ = atof(val);
  } else if (strcmp(key, "V") == 0) {
    sensor.vibTotal = atof(val);
  }
}

// Tam bir satiri isle; gecerli paketse true doner
static bool parseLine(char *line)
{
  if (strncmp(line, "T:", 2) != 0) {  // bozuk/yarim satir
    sensor.badLines++;
    Serial.printf("Gecersiz satir: \"%s\"\n", line);
    return false;
  }

  char *saveptr;
  for (char *tok = strtok_r(line, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
    parseField(tok);
  }
  sensor.lastUpdateMs = millis();
  sensor.packetCount++;
  return true;
}

// Yeni paket: alarmlari degerlendir, kayit araligina ekle
static void onNewData()
{
  evaluateAlarms();
  evaluateAnomalies();

  if (sensor.tempValid) {
    acc.tSum += sensor.temperature;
    acc.tN++;
  }
  acc.vxSum += sensor.vibX;
  acc.vySum += sensor.vibY;
  acc.vzSum += sensor.vibZ;
  if (sensor.vibTotal > acc.vMax) acc.vMax = sensor.vibTotal;
  acc.n++;
  acc.flags |= activeFlags();

  mqttPublishTelemetry();
}

static void pumpSerial()
{
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    sensor.rxBytes++;

    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen > 0 && parseLine(lineBuf)) {
        onNewData();
      }
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;  // tasma: satiri at
    }
  }
}

/* --------------------------- Web sayfasi --------------------------------- */

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="tr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sensör Paneli</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js"></script>
<style>
  :root { --bg:#f4f6f8; --card:#fff; --fg:#1d2733; --muted:#5b6776; --line:#e3e7ec; --bad:#c0392b; --ok:#1e8449; --acc:#2471a3; }
  * { box-sizing:border-box; }
  body { font-family:system-ui,-apple-system,"Segoe UI",sans-serif; background:var(--bg); color:var(--fg); margin:0; padding:16px; }
  main { max-width:1100px; margin:0 auto; display:flex; flex-direction:column; gap:14px; }
  header { display:flex; flex-wrap:wrap; justify-content:space-between; align-items:baseline; gap:8px; }
  h1 { font-size:1.35rem; margin:0; }
  h2 { font-size:1.05rem; margin:0 0 10px; }
  #clock { font-variant-numeric:tabular-nums; color:var(--muted); }
  #status { width:100%; font-size:.85rem; color:var(--muted); }
  .grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:12px; }
  .card { background:var(--card); border-radius:10px; padding:14px; box-shadow:0 1px 3px rgba(0,0,0,.08); }
  .label { font-size:.85rem; color:var(--muted); }
  .value { font-size:1.8rem; font-weight:600; margin-top:4px; font-variant-numeric:tabular-nums; }
  .unit { font-size:1rem; font-weight:400; color:var(--muted); }
  .sub { font-size:.8rem; color:var(--muted); margin-top:4px; }
  .card.alarm { box-shadow:0 0 0 2px var(--bad); }
  .card.alarm .value { color:var(--bad); }
  .err { color:var(--bad); }
  .row { display:flex; flex-wrap:wrap; align-items:center; justify-content:space-between; gap:8px; margin-bottom:10px; }
  .row h2 { margin:0; }
  .btns { display:flex; flex-wrap:wrap; gap:6px; }
  button, .btn { font:inherit; font-size:.85rem; padding:6px 12px; border-radius:6px; border:1px solid var(--line); background:#fff; color:var(--fg); cursor:pointer; text-decoration:none; }
  button.on { background:var(--acc); border-color:var(--acc); color:#fff; }
  button.primary { background:var(--acc); border-color:var(--acc); color:#fff; }
  button.danger { color:var(--bad); border-color:var(--bad); }
  .chartWrap { position:relative; height:340px; }
  #chartMsg { font-size:.8rem; color:var(--muted); margin-top:6px; }
  #activeAlarms div { padding:6px 0; border-bottom:1px solid var(--line); }
  #activeAlarms div:last-child { border-bottom:0; }
  form { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr)); gap:12px; align-items:end; }
  label { display:flex; flex-direction:column; gap:4px; font-size:.85rem; color:var(--muted); }
  input { font:inherit; font-size:1rem; padding:6px 8px; border:1px solid var(--line); border-radius:6px; color:var(--fg); }
  #setMsg { font-size:.85rem; margin-top:8px; }
  .tableWrap { overflow-x:auto; }
  table { width:100%; border-collapse:collapse; font-size:.85rem; }
  th, td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--line); white-space:nowrap; }
  td:last-child { white-space:normal; min-width:180px; }
  tr.BASLADI td:nth-child(3) { color:var(--bad); font-weight:600; }
  tr.BITTI td:nth-child(3) { color:var(--ok); font-weight:600; }
  #storage p { margin:4px 0; font-size:.9rem; }
  .bar { height:8px; background:var(--line); border-radius:4px; overflow:hidden; margin:8px 0; }
  .bar div { height:100%; background:var(--acc); width:0; }
  .pill { font-size:.8rem; padding:3px 10px; border-radius:999px; background:var(--bg); color:var(--muted); }
  .modes { display:grid; grid-template-columns:repeat(auto-fit,minmax(280px,1fr)); gap:12px; margin-top:12px; }
  .mode { border:1px solid var(--line); border-radius:8px; padding:12px; }
  .mode h3 { font-size:.95rem; margin:0 0 4px; }
  .mode .sub { margin:0 0 10px; }
  #anomMsg { font-size:.85rem; margin-top:8px; }
</style>
</head>
<body>
<main>
<header>
  <h1>STM32 Sensör Paneli</h1>
  <div id="clock">-</div>
  <div id="status">Bağlanılıyor...</div>
</header>

<section class="grid">
  <div class="card" id="cardT"><div class="label">Sıcaklık</div><div class="value"><span id="t">-</span> <span class="unit">&deg;C</span></div><div class="sub" id="tsetLbl"></div></div>
  <div class="card" id="cardV"><div class="label">Titreşim RMS (toplam)</div><div class="value"><span id="v">-</span> <span class="unit">g</span></div><div class="sub" id="vsetLbl"></div></div>
  <div class="card"><div class="label">Titreşim X</div><div class="value"><span id="vx">-</span> <span class="unit">g</span></div></div>
  <div class="card"><div class="label">Titreşim Y</div><div class="value"><span id="vy">-</span> <span class="unit">g</span></div></div>
  <div class="card"><div class="label">Titreşim Z</div><div class="value"><span id="vz">-</span> <span class="unit">g</span></div></div>
</section>

<section class="card" id="alarmCard">
  <h2>Aktif Alarmlar</h2>
  <div id="activeAlarms">Yok</div>
</section>

<section class="card" id="anomCard">
  <div class="row">
    <h2>Anomali Tespiti (uç birim)</h2>
    <span class="pill" id="anSource">-</span>
  </div>
  <section class="grid">
    <div><div class="label">Durum</div><div class="value" id="anMode">-</div></div>
    <div><div class="label">Anomali skoru (z)</div><div class="value" id="anZ">-</div><div class="sub">duruş titreşimine göre, 4 üstü dikkat</div></div>
    <div><div class="label">Normal duruş titreşimi</div><div class="value"><span id="anMu">-</span> <span class="unit">g</span></div><div class="sub" id="anSigma"></div></div>
    <div><div class="label">Sefer sınırları</div><div class="value" id="anRides">-</div><div class="sub" id="anRideInfo"></div></div>
  </section>
  <div class="bar" id="anLearnBar" style="display:none"><div id="anLearn"></div></div>

  <div class="modes">
    <div class="mode">
      <h3>Otomatik öğrenme</h3>
      <p class="sub">Asansör normal çalışırken başlatın. Seçilen süre boyunca normal durum öğrenilir; bu sürede karta dokunmayın.</p>
      <form id="autoForm">
        <label>Öğrenme süresi (dk)<input type="number" id="inLearn" step="1" min="1" max="240" required></label>
        <button type="submit" class="primary">Otomatik öğrenmeyi başlat</button>
      </form>
    </div>
    <div class="mode">
      <h3>Manuel giriş</h3>
      <p class="sub">Normal durum değerlerini siz girin. Sistem bu değerleri kendiliğinden değiştirmez. Ani darbe eşiği, sefer tepe sınırının 2 katıdır (en az 0,15 g); sınır çok düşük girilirse güçlü seferler darbe olarak da bildirilir.</p>
      <form id="manForm">
        <label>Duruş titreşimi ortalaması (g)<input type="number" id="inMu" step="0.0001" min="0" max="2" required></label>
        <label>Duruş titreşimi sapması σ (g)<input type="number" id="inSig" step="0.0001" min="0.0005" max="1" required></label>
        <label>En uzun normal sefer (s)<input type="number" id="inRide" step="1" min="5" max="3600" required></label>
        <label>Sefer tepe titreşimi sınırı (g)<input type="number" id="inPeak" step="0.001" min="0.001" max="16" required></label>
        <button type="button" id="fillBtn">Mevcut değerleri doldur</button>
        <button type="submit" class="primary">Manuel değerleri uygula</button>
      </form>
    </div>
  </div>
  <div id="anomMsg"></div>
</section>

<section class="card">
  <div class="row">
    <h2>Grafik</h2>
    <div class="btns" id="ranges">
      <button data-h="1" class="on">1 saat</button>
      <button data-h="6">6 saat</button>
      <button data-h="24">24 saat</button>
      <button data-h="168">7 gün</button>
      <button data-h="0">Tümü</button>
      <a class="btn" href="/log.csv">CSV indir</a>
    </div>
  </div>
  <div class="chartWrap"><canvas id="chart"></canvas></div>
  <div id="chartMsg"></div>
</section>

<section class="card">
  <h2>Set Değerleri</h2>
  <form id="setForm">
    <label>Sıcaklık seti (&deg;C)<input type="number" id="inT" step="0.1" min="-55" max="125" required></label>
    <label>Titreşim seti (g RMS)<input type="number" id="inV" step="0.001" min="0.001" max="16" required></label>
    <label>Kayıt aralığı (s)<input type="number" id="inI" step="1" min="1" max="3600" required></label>
    <button type="submit" class="primary">Kaydet</button>
  </form>
  <div id="setMsg"></div>
</section>

<section class="card">
  <div class="row">
    <h2>Alarm / Olay Geçmişi</h2>
    <a class="btn" href="/events.csv">CSV indir</a>
  </div>
  <div class="tableWrap">
    <table>
      <thead><tr><th>Zaman</th><th>Kod</th><th>Durum</th><th>Değer</th><th>Limit</th><th>Açıklama</th></tr></thead>
      <tbody id="events"><tr><td colspan="6">Yükleniyor...</td></tr></tbody>
    </table>
  </div>
</section>

<section class="card">
  <div class="row">
    <h2>Depolama</h2>
    <button class="danger" id="clearBtn">Tüm kayıtları sil</button>
  </div>
  <div id="storage">-</div>
</section>
</main>

<script>
const $ = id => document.getElementById(id);
const fmt = t => t ? new Date(t * 1000).toLocaleString('tr-TR') : 'saat yok';
const num = (v, d) => (v === null || v === undefined || isNaN(v)) ? '-' : Number(v).toFixed(d);
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const STATE = {BASLADI:'Başladı', BITTI:'Bitti', BILGI:'Bilgi'};

let cfg = {tset:0, vset:0, logint:10};
let rangeH = 1, chart = null, flagsArr = [], formDirty = false;
let anom = null, anomDirty = false;

async function getJSON(url, opt) {
  const r = await fetch(url, opt);
  if (!r.ok) throw new Error(r.status);
  return r.json();
}

function dur(sec) {
  sec = Math.max(0, Math.round(sec));
  const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60);
  if (d) return d + ' gün ' + h + ' saat';
  if (h) return h + ' saat ' + m + ' dk';
  return m + ' dk';
}

function kb(b) { return (b / 1024).toFixed(0) + ' KB'; }

async function updateLive() {
  try {
    const d = await getJSON('/data');
    $('clock').textContent = d.synced ? fmt(d.now) : 'Saat senkronize değil (NTP bekleniyor)';
    $('t').textContent  = d.tempValid ? d.temperature.toFixed(2) : 'HATA';
    $('v').textContent  = d.vibTotal.toFixed(3);
    $('vx').textContent = d.vibX.toFixed(3);
    $('vy').textContent = d.vibY.toFixed(3);
    $('vz').textContent = d.vibZ.toFixed(3);
    cfg = {tset:d.tset, vset:d.vset, logint:d.logint};
    $('tsetLbl').textContent = 'Set: ' + d.tset.toFixed(1) + ' °C';
    $('vsetLbl').textContent = 'Set: ' + d.vset.toFixed(3) + ' g';
    if (!formDirty) { $('inT').value = d.tset.toFixed(1); $('inV').value = d.vset.toFixed(3); $('inI').value = d.logint; }

    const codes = d.alarms.map(a => a.code);
    $('cardT').classList.toggle('alarm', ['E01', 'E03', 'A04'].some(c => codes.includes(c)));
    $('cardV').classList.toggle('alarm', ['E02', 'A01', 'A02', 'A03'].some(c => codes.includes(c)));

    const an = d.anom;
    anom = an;
    const learning = an.mode === 0;
    $('anSource').textContent = an.manual ? 'Manuel değerler' : 'Otomatik (' + an.learnMin + ' dk öğrenme)';
    $('anMode').textContent = learning ? 'Öğreniyor %' + Math.floor(an.learn * 100)
      : an.mode === 2 ? 'Hareket (' + an.rideSec + ' s)' : 'Duruş';
    $('anZ').textContent = learning ? '-' : an.z.toFixed(1);
    $('anMu').textContent = an.mu.toFixed(4);
    $('anSigma').textContent = 'sigma ' + an.sigma.toFixed(4) + ' g';
    $('anRides').textContent = '≤ ' + an.rideMax + ' s';
    $('anRideInfo').textContent = an.manual ? 'tepe ≤ ' + an.peakLim.toFixed(3) + ' g (manuel)'
      : an.rides >= 10 ? an.rides + ' sefer öğrenildi, tepe ≤ ' + an.peakLim.toFixed(3) + ' g'
      : an.rides + '/10 sefer; tepe kontrolü 10 seferden sonra başlar';
    $('anLearnBar').style.display = learning ? '' : 'none';
    $('anLearn').style.width = (an.learn * 100).toFixed(1) + '%';
    if (!anomDirty) $('inLearn').value = an.learnMin;
    $('anomCard').classList.toggle('alarm', codes.some(c => c[0] === 'A'));
    $('alarmCard').classList.toggle('alarm', d.alarms.length > 0);
    $('activeAlarms').innerHTML = d.alarms.length
      ? d.alarms.map(a => `<div class="err"><b>${a.code}</b> ${esc(a.desc)} &mdash; başlangıç: ${fmt(a.since)}</div>`).join('')
      : 'Yok';

    const st = $('status');
    if (d.packetCount === 0) {
      st.textContent = "UYARI: STM32'den hiç geçerli paket gelmedi (alınan bayt: " + d.rxBytes + ', bozuk satır: ' + d.badLines + ')';
      st.className = 'err';
    } else if (d.stale) {
      st.textContent = "UYARI: STM32'den veri gelmiyor (son veri " + (d.ageMs / 1000).toFixed(1) + ' sn önce)';
      st.className = 'err';
    } else {
      let api = '';
      if (d.api && d.api.enabled) {
        api = d.api.online
          ? '  |  Bulut API: bağlı (bekleyen ' + d.api.pending + ')'
          : '  |  Bulut API: ULAŞILAMIYOR, ' + d.api.pending + ' kayıt cihazda bekliyor';
      }
      st.textContent = 'Paket: ' + d.packetCount + '  |  kayıt aralığı: ' + d.logint + ' s' + api;
      st.className = '';
    }
  } catch (e) {
    $('status').textContent = 'ESP32 ile bağlantı yok';
    $('status').className = 'err';
  }
}

function tickFmt(v) {
  const d = new Date(v * 1000);
  const hm = d.toLocaleTimeString('tr-TR', {hour:'2-digit', minute:'2-digit'});
  if (rangeH && rangeH <= 24) return hm;
  return d.toLocaleDateString('tr-TR', {day:'2-digit', month:'2-digit'}) + ' ' + hm;
}

function alarmPt(ctx, mask) {
  return (flagsArr[ctx.dataIndex] & mask) ? 3 : 0;
}

function buildChart() {
  if (typeof Chart === 'undefined') {
    $('chartMsg').textContent = 'Grafik kütüphanesi yüklenemedi; tarayıcının internet erişimi olmalı. Kayıtlar CSV olarak indirilebilir.';
    return;
  }
  const mk = (label, color, axis, extra) => Object.assign(
    {label, borderColor:color, backgroundColor:color, yAxisID:axis, data:[], borderWidth:1.5, pointRadius:0, tension:0.2}, extra || {});
  chart = new Chart($('chart'), {
    type: 'line',
    data: { datasets: [
      mk('Sıcaklık (°C)', '#d35400', 'y', {pointRadius:c => alarmPt(c, 1), pointBackgroundColor:'#c0392b', pointBorderColor:'#c0392b'}),
      mk('Titreşim toplam (g)', '#2471a3', 'y1', {pointRadius:c => alarmPt(c, 2), pointBackgroundColor:'#c0392b', pointBorderColor:'#c0392b'}),
      mk('Titreşim X (g)', '#27ae60', 'y1', {hidden:true}),
      mk('Titreşim Y (g)', '#8e44ad', 'y1', {hidden:true}),
      mk('Titreşim Z (g)', '#7f8c8d', 'y1', {hidden:true}),
      mk('Sıcaklık set', '#d35400', 'y', {borderDash:[6, 4], borderWidth:1, tension:0}),
      mk('Titreşim set', '#2471a3', 'y1', {borderDash:[6, 4], borderWidth:1, tension:0}),
    ]},
    options: {
      animation:false, parsing:false, normalized:true, maintainAspectRatio:false,
      interaction:{mode:'nearest', axis:'x', intersect:false},
      scales: {
        x: {type:'linear', ticks:{callback:tickFmt, maxRotation:0, autoSkipPadding:24}},
        y: {position:'left', title:{display:true, text:'°C'}},
        y1:{position:'right', title:{display:true, text:'g'}, grid:{drawOnChartArea:false}, min:0},
      },
      plugins: { tooltip: { callbacks: { title: items => items.length ? fmt(items[0].parsed.x) : '' } } },
    },
  });
}

async function loadHistory() {
  if (!chart) return;
  try {
    const now = Math.floor(Date.now() / 1000);
    const from = rangeH ? now - rangeH * 3600 : 0;
    const d = await getJSON('/history?from=' + from);
    const rows = d.rows;   // [t, sicaklik|null, vx, vy, vz, v, flags]
    flagsArr = rows.map(r => r[6]);
    const ds = chart.data.datasets;
    ds[0].data = rows.map(r => ({x:r[0], y:r[1]}));
    ds[1].data = rows.map(r => ({x:r[0], y:r[5]}));
    ds[2].data = rows.map(r => ({x:r[0], y:r[2]}));
    ds[3].data = rows.map(r => ({x:r[0], y:r[3]}));
    ds[4].data = rows.map(r => ({x:r[0], y:r[4]}));
    const x0 = rows.length ? rows[0][0] : from, x1 = rows.length ? rows[rows.length - 1][0] : now;
    ds[5].data = [{x:x0, y:cfg.tset}, {x:x1, y:cfg.tset}];
    ds[6].data = [{x:x0, y:cfg.vset}, {x:x1, y:cfg.vset}];
    chart.update();
    $('chartMsg').textContent = rows.length
      ? rows.length + ' nokta' + (d.stride > 1 ? ' (her nokta ' + d.stride + ' kaydın özeti: sıcaklık ortalama, titreşim en yüksek)' : '') + '. Kırmızı noktalar alarm anları.'
      : 'Bu aralıkta kayıt yok.';
  } catch (e) {
    $('chartMsg').textContent = 'Geçmiş veriler alınamadı.';
  }
}

async function loadEvents() {
  try {
    const ev = await getJSON('/events');   // [t, kod, durum, deger|null, limit|null, aciklama], yeniden eskiye
    $('events').innerHTML = ev.length ? ev.map(e => {
      const dec = e[1] === 'E01' ? 2 : 3;
      return `<tr class="${e[2]}"><td>${fmt(e[0])}</td><td><b>${esc(e[1])}</b></td><td>${STATE[e[2]] || esc(e[2])}</td>`
           + `<td>${num(e[3], dec)}</td><td>${num(e[4], dec)}</td><td>${esc(e[5])}</td></tr>`;
    }).join('') : '<tr><td colspan="6">Kayıtlı olay yok</td></tr>';
  } catch (e) {}
}

async function loadStorage() {
  try {
    const s = await getJSON('/storage');
    const pct = s.maxLogBytes ? Math.min(100, s.logBytes / s.maxLogBytes * 100) : 0;
    $('storage').innerHTML =
      `<div class="bar"><div style="width:${pct.toFixed(1)}%"></div></div>`
      + `<p>Log alanı: <b>${kb(s.logBytes)}</b> / ${kb(s.maxLogBytes)} (%${pct.toFixed(1)}) &middot; dosya sistemi: ${kb(s.fsUsed)} / ${kb(s.fsTotal)}</p>`
      + `<p>Kayıt sayısı: <b>${s.records}</b> &middot; en eski kayıt: ${fmt(s.oldest)}</p>`
      + `<p>Kapasite: <b>${s.capacityRecords}</b> kayıt (${s.recSize} bayt/kayıt) &rarr; ${s.logint} s aralıkla <b>${dur(s.capacityRecords * s.logint)}</b> veri tutulur.</p>`
      + `<p>Dolmasına kalan: ${dur(Math.max(0, s.capacityRecords - s.records) * s.logint)}. Dolunca en eski kayıtlar silinerek kayda devam edilir.</p>`;
  } catch (e) {}
}

$('ranges').addEventListener('click', ev => {
  const b = ev.target.closest('button');
  if (!b) return;
  rangeH = Number(b.dataset.h);
  document.querySelectorAll('#ranges button').forEach(x => x.classList.toggle('on', x === b));
  loadHistory();
});

['inT', 'inV', 'inI'].forEach(id => $(id).addEventListener('input', () => { formDirty = true; }));

$('setForm').addEventListener('submit', async ev => {
  ev.preventDefault();
  const body = new URLSearchParams({tset:$('inT').value, vset:$('inV').value, logint:$('inI').value});
  const msg = $('setMsg');
  try {
    const r = await getJSON('/settings', {method:'POST', body});
    if (!r.ok) throw new Error(r.err);
    msg.textContent = 'Kaydedildi. Elektrik kesilse de bu değerler korunur.';
    msg.className = '';
    formDirty = false;
    updateLive(); loadEvents(); loadStorage(); loadHistory();
  } catch (e) {
    msg.textContent = 'Kaydedilemedi: ' + e.message;
    msg.className = 'err';
  }
});

// Hata durumunda sunucunun aciklamasini (err) dondurur
async function postForm(url, body) {
  const r = await fetch(url, {method:'POST', body});
  let j = {};
  try { j = await r.json(); } catch (e) {}
  if (!r.ok || !j.ok) throw new Error(j.err || r.status);
  return j;
}

function anomResult(ok, text) {
  $('anomMsg').textContent = text;
  $('anomMsg').className = ok ? '' : 'err';
  if (ok) { anomDirty = false; updateLive(); loadEvents(); }
}

$('inLearn').addEventListener('input', () => { anomDirty = true; });

$('autoForm').addEventListener('submit', async ev => {
  ev.preventDefault();
  const min = $('inLearn').value;
  if (!confirm('Mevcut normal durum silinip ' + min + ' dk boyunca yeniden öğrenilecek. Bu sürede asansör normal çalışmalı ve karta dokunulmamalı. Devam?')) return;
  try {
    await postForm('/anomaly/auto', new URLSearchParams({minutes:min}));
    anomResult(true, 'Otomatik öğrenme başladı: ' + min + ' dk.');
  } catch (e) { anomResult(false, 'Başlatılamadı: ' + e.message); }
});

$('fillBtn').addEventListener('click', () => {
  if (!anom) return;
  $('inMu').value = anom.mu.toFixed(4);
  $('inSig').value = anom.sigma.toFixed(4);
  $('inRide').value = anom.rideMax;
  $('inPeak').value = (anom.peakLim > 0 ? anom.peakLim : Math.max(anom.mu + 20 * anom.sigma, 0.05)).toFixed(3);
});

$('manForm').addEventListener('submit', async ev => {
  ev.preventDefault();
  const body = new URLSearchParams({mu:$('inMu').value, sigma:$('inSig').value, ridemax:$('inRide').value, peak:$('inPeak').value});
  if (!confirm('Anomali tespiti girdiğiniz değerlerle çalışacak ve otomatik öğrenme kapanacak. Devam?')) return;
  try {
    await postForm('/anomaly/manual', body);
    anomResult(true, 'Manuel değerler uygulandı.');
  } catch (e) { anomResult(false, 'Uygulanamadı: ' + e.message); }
});

$('clearBtn').addEventListener('click', async () => {
  if (!confirm('Tüm veri ve olay kayıtları silinecek. Emin misiniz?')) return;
  try { await getJSON('/clear', {method:'POST'}); } catch (e) {}
  loadHistory(); loadEvents(); loadStorage();
});

buildChart();
updateLive().then(loadHistory);
loadEvents();
loadStorage();
setInterval(updateLive, 1000);
setInterval(loadHistory, 15000);
setInterval(loadEvents, 5000);
setInterval(loadStorage, 30000);
</script>
</body>
</html>
)rawliteral";

/* --------------------------- HTTP uclari --------------------------------- */

static void handleRoot()
{
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleData()
{
  uint32_t age = sensor.packetCount ? millis() - sensor.lastUpdateMs : 0;
  bool stale = sensor.packetCount == 0 || age > TIMEOUT_MS;
  uint32_t now = nowEpoch();
  String j;
  j.reserve(1024);
  char b[320];

  snprintf(b, sizeof(b),
           "{\"temperature\":%.2f,\"tempValid\":%s,\"vibX\":%.3f,\"vibY\":%.3f,\"vibZ\":%.3f,\"vibTotal\":%.3f,",
           sensor.temperature, sensor.tempValid ? "true" : "false",
           sensor.vibX, sensor.vibY, sensor.vibZ, sensor.vibTotal);
  j += b;
  snprintf(b, sizeof(b),
           "\"packetCount\":%lu,\"ageMs\":%lu,\"stale\":%s,\"rxBytes\":%lu,\"badLines\":%lu,",
           (unsigned long)sensor.packetCount, (unsigned long)age, stale ? "true" : "false",
           (unsigned long)sensor.rxBytes, (unsigned long)sensor.badLines);
  j += b;
  snprintf(b, sizeof(b),
           "\"now\":%lu,\"synced\":%s,\"tset\":%.2f,\"vset\":%.3f,\"logint\":%u,\"alarms\":[",
           (unsigned long)now, now ? "true" : "false",
           settings.tempSet, settings.vibSet, settings.logInterval);
  j += b;
  bool first = true;
  for (int i = 0; i < AL_COUNT; i++) {
    if (!alarmActive[i]) continue;
    snprintf(b, sizeof(b), "%s{\"code\":\"%s\",\"desc\":\"%s\",\"since\":%lu}",
             first ? "" : ",", ALARMS[i].code, ALARMS[i].desc, (unsigned long)alarmSince[i]);
    j += b;
    first = false;
  }
  j += "],";
  snprintf(b, sizeof(b),
           "\"anom\":{\"mode\":%u,\"z\":%.2f,\"learn\":%.3f,\"mu\":%.4f,\"sigma\":%.4f,\"rides\":%lu,"
           "\"rideSec\":%lu,\"rideDur\":%.1f,\"ridePeak\":%.3f,\"manual\":%s,\"learnMin\":%u,"
           "\"rideMax\":%.0f,\"peakLim\":%.3f},",
           anom.mode, anom.z, anom.learnProgress(), anom.mu(), anom.sigma(), (unsigned long)anom.p.nRides,
           (unsigned long)anom.rideSeconds(), anom.p.durMean, anom.p.peakMean, anom.isManual() ? "true" : "false",
           anom.p.learnS / 60, anom.rideDurLimitNow(), anom.ridePeakLimitNow());
  j += b;
  snprintf(b, sizeof(b), "\"api\":{\"enabled\":%s,\"online\":%s,\"status\":%d,\"pending\":%lu,\"sent\":%lu,\"lastOk\":%lu}}",
           API_URL[0] ? "true" : "false", apiOnline ? "true" : "false", apiLastStatus,
           (unsigned long)apiPending, (unsigned long)apiSentTotal, (unsigned long)apiLastOk);
  j += b;
  server.send(200, "application/json", j);
}

static bool fileOverlaps(size_t i, uint32_t from, uint32_t to)
{
  uint32_t start = logFiles[i].start;
  uint32_t end = (i + 1 < logFiles.size()) ? logFiles[i + 1].start - 1 : UINT32_MAX;
  return !(end < from || start > to);
}

static void emitBucket(String &out, const Bucket &b, bool &first)
{
  char row[112];
  char temp[12];
  if (b.tN) {
    snprintf(temp, sizeof(temp), "%.2f", b.tSum / b.tN);
  } else {
    strcpy(temp, "null");
  }
  snprintf(row, sizeof(row), "%s[%lu,%s,%.3f,%.3f,%.3f,%.3f,%u]",
           first ? "" : ",", (unsigned long)b.t, temp,
           b.vx / b.n / 1000.0f, b.vy / b.n / 1000.0f, b.vz / b.n / 1000.0f,
           b.vMax / 1000.0f, b.flags);
  out += row;
  first = false;
}

// Grafik verisi: /history?from=<epoch>&to=<epoch>
// En fazla HISTORY_MAX_POINTS nokta; fazla kayit varsa gruplanir
// (sicaklik ve eksenler ortalama, toplam titresim en yuksek, alarm bitleri birlesik)
static void handleHistory()
{
  FsLock lock;
  uint32_t from = server.hasArg("from") ? strtoul(server.arg("from").c_str(), nullptr, 10) : 0;
  uint32_t to   = server.hasArg("to")   ? strtoul(server.arg("to").c_str(), nullptr, 10)   : UINT32_MAX;

  uint32_t est = 0;
  for (size_t i = 0; i < logFiles.size(); i++) {
    if (fileOverlaps(i, from, to)) est += logFiles[i].size / sizeof(LogRec);
  }
  uint32_t stride = (est > HISTORY_MAX_POINTS) ? (est + HISTORY_MAX_POINTS - 1) / HISTORY_MAX_POINTS : 1;

  streaming = true;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String out;
  out.reserve(2048);
  out = "{\"stride\":";
  out += stride;
  out += ",\"rows\":[";
  bool first = true;
  Bucket b = {};
  LogRec buf[32];

  for (size_t i = 0; i < logFiles.size(); i++) {
    if (!fileOverlaps(i, from, to)) continue;
    File f = LittleFS.open(logPath(logFiles[i].start), FILE_READ);
    if (!f) continue;
    size_t len;
    while ((len = f.read((uint8_t *)buf, sizeof(buf))) >= sizeof(LogRec)) {
      size_t cnt = len / sizeof(LogRec);
      for (size_t k = 0; k < cnt; k++) {
        const LogRec &r = buf[k];
        if (r.t < from || r.t > to) continue;
        b.t = r.t;
        if (r.tempC100 != INT16_MIN) {
          b.tSum += r.tempC100 / 100.0f;
          b.tN++;
        }
        b.vx += r.vxMg;
        b.vy += r.vyMg;
        b.vz += r.vzMg;
        if (r.vMg > b.vMax) b.vMax = r.vMg;
        b.flags |= r.flags;
        if (++b.n >= stride) {
          emitBucket(out, b, first);
          b = {};
        }
      }
      if (out.length() > 1500) {
        server.sendContent(out);
        out = "";
        pumpSerial();
      }
    }
    f.close();
  }
  if (b.n) emitBucket(out, b, first);
  out += "]}";
  server.sendContent(out);
  server.sendContent("");
  streaming = false;
}

// Tum kayitlar: noktali virgul ayracli, ondalik virgullu (Turkce Excel dogrudan acar)
static void handleLogCsv()
{
  FsLock lock;
  streaming = true;
  server.sendHeader("Content-Disposition", "attachment; filename=sensor_log.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");

  String out;
  out.reserve(2048);
  out = "\xEF\xBB\xBF"
        "zaman;sicaklik_C;titresim_x_g;titresim_y_g;titresim_z_g;titresim_toplam_max_g;alarmlar\r\n";
  LogRec buf[32];

  for (size_t i = 0; i < logFiles.size(); i++) {
    File f = LittleFS.open(logPath(logFiles[i].start), FILE_READ);
    if (!f) continue;
    size_t len;
    while ((len = f.read((uint8_t *)buf, sizeof(buf))) >= sizeof(LogRec)) {
      size_t cnt = len / sizeof(LogRec);
      for (size_t k = 0; k < cnt; k++) {
        const LogRec &r = buf[k];
        char ts[24], nums[64], al[40] = "";
        fmtTime(r.t, ts, sizeof(ts));
        if (r.tempC100 != INT16_MIN) {
          snprintf(nums, sizeof(nums), "%.2f;%.3f;%.3f;%.3f;%.3f", r.tempC100 / 100.0f,
                   r.vxMg / 1000.0f, r.vyMg / 1000.0f, r.vzMg / 1000.0f, r.vMg / 1000.0f);
        } else {
          snprintf(nums, sizeof(nums), ";%.3f;%.3f;%.3f;%.3f",
                   r.vxMg / 1000.0f, r.vyMg / 1000.0f, r.vzMg / 1000.0f, r.vMg / 1000.0f);
        }
        dotToComma(nums);
        for (int a = 0; a < AL_COUNT; a++) {
          if (r.flags & ALARMS[a].flag) {
            if (al[0]) strlcat(al, " ", sizeof(al));
            strlcat(al, ALARMS[a].code, sizeof(al));
          }
        }
        out += ts;
        out += ';';
        out += nums;
        out += ';';
        out += al;
        out += "\r\n";
      }
      if (out.length() > 1500) {
        server.sendContent(out);
        out = "";
        pumpSerial();
      }
    }
    f.close();
  }
  server.sendContent(out);
  server.sendContent("");
  streaming = false;
}

// Olay satirini alanlara ayir: epoch,kod,durum,deger,limit,aciklama
static bool splitEvent(const String &line, String f[6])
{
  int pos = 0;
  for (int i = 0; i < 5; i++) {
    int c = line.indexOf(',', pos);
    if (c < 0) return false;
    f[i] = line.substring(pos, c);
    pos = c + 1;
  }
  f[5] = line.substring(pos);
  return true;
}

static String jsonNum(const String &s)
{
  return (s == "nan" || s.length() == 0) ? String("null") : s;
}

static void handleEvents()
{
  FsLock lock;
  std::vector<String> lines;
  File f = LittleFS.open(EVENTS_FILE, FILE_READ);
  if (f) {
    while (f.available()) {
      String l = f.readStringUntil('\n');
      l.trim();
      if (!l.length()) continue;
      lines.push_back(l);
      if (lines.size() > EVENTS_MAX_SHOWN) lines.erase(lines.begin());
    }
    f.close();
  }

  String j = "[";
  bool first = true;
  for (int i = (int)lines.size() - 1; i >= 0; i--) {
    String fl[6];
    if (!splitEvent(lines[i], fl)) continue;
    if (!first) j += ',';
    j += '[';
    j += fl[0];
    j += ",\"" + fl[1] + "\",\"" + fl[2] + "\",";
    j += jsonNum(fl[3]);
    j += ',';
    j += jsonNum(fl[4]);
    j += ",\"" + fl[5] + "\"]";
    first = false;
  }
  j += ']';
  server.send(200, "application/json", j);
}

static void handleEventsCsv()
{
  FsLock lock;
  server.sendHeader("Content-Disposition", "attachment; filename=olaylar.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  String out = "\xEF\xBB\xBF" "zaman;kod;durum;deger;limit;aciklama\r\n";

  File f = LittleFS.open(EVENTS_FILE, FILE_READ);
  if (f) {
    while (f.available()) {
      String l = f.readStringUntil('\n');
      l.trim();
      String fl[6];
      if (!splitEvent(l, fl)) continue;
      char ts[24];
      fmtTime(strtoul(fl[0].c_str(), nullptr, 10), ts, sizeof(ts));
      if (fl[3] == "nan") fl[3] = "";
      if (fl[4] == "nan") fl[4] = "";
      fl[3].replace('.', ',');
      fl[4].replace('.', ',');
      out += String(ts) + ';' + fl[1] + ';' + fl[2] + ';' + fl[3] + ';' + fl[4] + ';' + fl[5] + "\r\n";
      if (out.length() > 1500) {
        server.sendContent(out);
        out = "";
      }
    }
    f.close();
  }
  server.sendContent(out);
  server.sendContent("");
}

static void handleStorage()
{
  uint32_t interval = settings.logInterval;
  uint32_t capRec = maxLogBytes() / sizeof(LogRec);
  char j[320];
  snprintf(j, sizeof(j),
           "{\"fsTotal\":%u,\"fsUsed\":%u,\"logBytes\":%lu,\"maxLogBytes\":%lu,\"recSize\":%u,"
           "\"records\":%lu,\"files\":%u,\"capacityRecords\":%lu,\"logint\":%lu,\"oldest\":%lu}",
           (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes(),
           (unsigned long)logBytes, (unsigned long)maxLogBytes(), (unsigned)sizeof(LogRec),
           (unsigned long)(logBytes / sizeof(LogRec)), (unsigned)logFiles.size(),
           (unsigned long)capRec, (unsigned long)interval,
           (unsigned long)(logFiles.empty() ? 0 : logFiles.front().start));
  server.send(200, "application/json", j);
}

// Web ve MQTT'den gelen set degerlerini dogrula, kaydet, olay yaz, yayinla
static bool applySettings(float t, float v, long li, const char *source)
{
  if (isnan(t) || isnan(v) || t < -55 || t > 125 || v < 0.001f || v > 16 || li < 1 || li > 3600) {
    return false;
  }
  char desc[192];
  snprintf(desc, sizeof(desc),
           "Set değerleri güncellendi (%s): sıcaklık %.1f->%.1f C / titreşim %.3f->%.3f g / kayıt aralığı %u->%ld s",
           source, settings.tempSet, t, settings.vibSet, v, settings.logInterval, li);
  settings.tempSet     = t;
  settings.vibSet      = v;
  settings.logInterval = (uint16_t)li;
  saveSettings();
  appendEvent("I01", "BILGI", NAN, NAN, desc);
  mqttPublishSettings();
  return true;
}

static void handleSettings()
{
  if (!server.hasArg("tset") || !server.hasArg("vset") || !server.hasArg("logint")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"eksik parametre\"}");
    return;
  }
  if (!applySettings(server.arg("tset").toFloat(), server.arg("vset").toFloat(),
                     server.arg("logint").toInt(), "web")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"gecersiz deger\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// JSON icinden "anahtar": sayi oku (basit; ic ice nesne beklenmez)
static bool jsonGetNumber(const char *json, const char *key, float &out)
{
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '"') p++;
  char *end;
  float v = strtof(p, &end);
  if (end == p) return false;
  out = v;
  return true;
}

// Ana donguden: MQTT ile gelen set komutlarini uygula, baglaninca duyuru yap
static void mqttProcess()
{
  if (mqttAnnounce && mqttConnected) {
    mqttAnnounce = false;
    mqttPublish("status", "online", 1, true);
    mqttPublishInfo();
    mqttPublishSettings();
    Serial.println("MQTT baglandi");
  }

  MqttMsg m;
  while (mqttRxQueue && xQueueReceive(mqttRxQueue, &m, 0) == pdTRUE) {
    // Anomali modu: {"learnmin":N} otomatik N dk, {"relearn":1} mevcut sureyle,
    // {"man_mu","man_sigma","man_ridemax","man_peak"} manuel degerler
    float learnMin = 0, relearn = 0, mmu, msig, mride, mpeak;
    if (jsonGetNumber(m.payload, "learnmin", learnMin)) {
      if (!startAutoLearning(lroundf(learnMin), "MQTT")) Serial.printf("MQTT ogrenme suresi reddedildi: %s\n", m.payload);
    } else if (jsonGetNumber(m.payload, "relearn", relearn) && relearn > 0) {
      startAutoLearning(anom.p.learnS / 60, "MQTT");
    }
    if (jsonGetNumber(m.payload, "man_mu", mmu) && jsonGetNumber(m.payload, "man_sigma", msig) &&
        jsonGetNumber(m.payload, "man_ridemax", mride) && jsonGetNumber(m.payload, "man_peak", mpeak)) {
      if (!applyManualAnomaly(mmu, msig, lroundf(mride), mpeak, "MQTT")) {
        Serial.printf("MQTT manuel anomali degerleri reddedildi: %s\n", m.payload);
      }
    }
    float t = settings.tempSet, v = settings.vibSet, li = settings.logInterval;
    bool any = jsonGetNumber(m.payload, "tset", t);
    any |= jsonGetNumber(m.payload, "vset", v);
    any |= jsonGetNumber(m.payload, "logint", li);
    if (!any) continue;
    if (!applySettings(t, v, lroundf(li), "MQTT")) {
      Serial.printf("MQTT set komutu reddedildi: %s\n", m.payload);
      mqttPublishSettings();   // panel eski degere geri donsun
    }
  }
}

static void handleClear()
{
  FsLock lock;
  for (const LogFile &lf : logFiles) {
    LittleFS.remove(logPath(lf.start));
  }
  logFiles.clear();
  logBytes = 0;
  LittleFS.remove(EVENTS_FILE);
  appendEvent("I02", "BILGI", NAN, NAN, "Tüm kayıtlar silindi");
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /anomaly/auto  minutes=N   (N bos ise mevcut sure)
static void handleAnomalyAuto()
{
  long minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : anom.p.learnS / 60;
  if (!startAutoLearning(minutes, "web")) {
    char err[96];
    snprintf(err, sizeof(err), "{\"ok\":false,\"err\":\"sure %d-%d dk olmali\"}", ANOM_LEARN_MIN_S / 60, ANOM_LEARN_MAX_S / 60);
    server.send(400, "application/json", err);
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /anomaly/manual  mu, sigma (g), ridemax (s), peak (g)
static void handleAnomalyManual()
{
  if (!server.hasArg("mu") || !server.hasArg("sigma") || !server.hasArg("ridemax") || !server.hasArg("peak")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"eksik parametre\"}");
    return;
  }
  if (!applyManualAnomaly(server.arg("mu").toFloat(), server.arg("sigma").toFloat(), server.arg("ridemax").toInt(),
                          server.arg("peak").toFloat(), "web")) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"gecersiz deger (tepe siniri ortalamadan buyuk olmali)\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

/* --------------------------- Bulut API --------------------------------- */

// HTTPS POST; HTTP durum kodunu, baglanti kurulamazsa -1 dondurur
static int apiPost(const char *path, const String &body)
{
  String url = String(API_URL) + path;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 60000;                       // Render uyku modundan ~1 dk'da uyanir
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t h = esp_http_client_init(&cfg);
  if (!h) return -1;
  esp_http_client_set_header(h, "Content-Type", "application/json");
  esp_http_client_set_header(h, "X-API-Key", API_KEY);
  esp_http_client_set_post_field(h, body.c_str(), body.length());
  esp_err_t err = esp_http_client_perform(h);
  int status = (err == ESP_OK) ? esp_http_client_get_status_code(h) : -1;
  esp_http_client_cleanup(h);
  return status;
}

// cursor'dan sonraki en fazla maxN olcumu oku; bekleyen toplam sayiyi (yaklasik) dondur
static size_t collectRecords(uint32_t cursor, LogRec *out, size_t maxN, uint32_t &pending)
{
  FsLock lock;
  size_t n = 0;
  pending = 0;
  LogRec buf[32];
  for (size_t i = 0; i < logFiles.size(); i++) {
    if (i + 1 < logFiles.size() && logFiles[i + 1].start - 1 <= cursor) continue;  // tamami gonderilmis
    if (n >= maxN) {
      pending += logFiles[i].size / sizeof(LogRec);
      continue;
    }
    File f = LittleFS.open(logPath(logFiles[i].start), FILE_READ);
    if (!f) continue;
    size_t len;
    while ((len = f.read((uint8_t *)buf, sizeof(buf))) >= sizeof(LogRec)) {
      for (size_t k = 0; k < len / sizeof(LogRec); k++) {
        if (buf[k].t <= cursor) continue;
        pending++;
        if (n < maxN) out[n++] = buf[k];
      }
      if (n >= maxN) {
        pending += (f.size() - f.position()) / sizeof(LogRec);
        break;
      }
    }
    f.close();
  }
  return n;
}

static String telemetryJson(const LogRec *r, size_t n)
{
  String j;
  j.reserve(64 + n * 96);
  j = "{\"device\":\"" API_DEVICE "\",\"records\":[";
  char row[128], temp[12];
  for (size_t i = 0; i < n; i++) {
    if (r[i].tempC100 == INT16_MIN) {
      strcpy(temp, "null");
    } else {
      snprintf(temp, sizeof(temp), "%.2f", r[i].tempC100 / 100.0f);
    }
    snprintf(row, sizeof(row), "%s{\"t\":%lu,\"temp\":%s,\"vx\":%.3f,\"vy\":%.3f,\"vz\":%.3f,\"v\":%.3f,\"flags\":%u}",
             i ? "," : "", (unsigned long)r[i].t, temp, r[i].vxMg / 1000.0f, r[i].vyMg / 1000.0f,
             r[i].vzMg / 1000.0f, r[i].vMg / 1000.0f, r[i].flags);
    j += row;
  }
  j += "]}";
  return j;
}

// cursor'dan sonraki olaylari JSON'a cevir; gonderilen son olayin zamanini lastT'ye yaz
static size_t collectEventsJson(uint32_t cursor, String &json, uint32_t &lastT)
{
  FsLock lock;
  File f = LittleFS.open(EVENTS_FILE, FILE_READ);
  if (!f) return 0;
  json = "{\"device\":\"" API_DEVICE "\",\"events\":[";
  size_t n = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    String fl[6];
    if (!splitEvent(l, fl)) continue;
    uint32_t t = strtoul(fl[0].c_str(), nullptr, 10);
    if (t == 0 || t <= cursor) continue;              // saatsiz olaylar gonderilmez
    if (n >= API_EVENT_BATCH && t != lastT) break;     // ayni saniyedeki olaylari bolme
    if (n) json += ',';
    json += "{\"t\":" + fl[0] + ",\"code\":\"" + fl[1] + "\",\"state\":\"" + fl[2] + "\",\"value\":" +
            jsonNum(fl[3]) + ",\"limit\":" + jsonNum(fl[4]) + ",\"desc\":\"" + fl[5] + "\"}";
    lastT = t;
    n++;
  }
  f.close();
  json += "]}";
  return n;
}

static void apiTask(void *)
{
  Preferences p;
  p.begin("api", false);
  uint32_t upT = p.getULong("upT", 0);   // gonderilen son olcumun zamani
  uint32_t upE = p.getULong("upE", 0);   // gonderilen son olayin zamani
  uint32_t lastSave = millis();
  bool dirty = false;
  static LogRec batch[API_BATCH];

  for (;;) {
    uint32_t wait = API_PERIOD_MS;
    if (WiFi.status() == WL_CONNECTED && nowEpoch() && fsOk) {
      uint32_t pending;
      size_t n = collectRecords(upT, batch, API_BATCH, pending);
      apiPending = pending;
      bool ok = true;
      if (n) {
        int st = apiPost("/api/v1/telemetry", telemetryJson(batch, n));
        apiLastStatus = st;
        if (st >= 200 && st < 300) {
          upT = batch[n - 1].t;
          apiSentTotal += n;
          apiPending = pending - n;
          dirty = true;
          if (apiPending) wait = 200;                 // birikmis veri: hemen devam
        } else if (st == 400 || st == 413) {
          Serial.printf("API kayitlari reddetti (%d), parti atlaniyor\n", st);
          upT = batch[n - 1].t;
          dirty = true;
        } else {
          ok = false;
        }
      }
      if (ok) {
        String ej;
        uint32_t lastT = 0;
        if (collectEventsJson(upE, ej, lastT)) {
          int st = apiPost("/api/v1/events", ej);
          apiLastStatus = st;
          if ((st >= 200 && st < 300) || st == 400 || st == 413) {
            upE = lastT;
            dirty = true;
            wait = 200;
          } else {
            ok = false;
          }
        }
      }
      if (ok && (n || apiLastStatus >= 200)) {
        if (!apiOnline) Serial.println("API: sunucuya ulasildi");
        apiOnline = true;
        apiLastOk = nowEpoch();
      } else if (!ok) {
        if (apiOnline) Serial.printf("API: sunucuya ulasilamiyor (%d), veriler cihazda birikiyor\n", apiLastStatus);
        apiOnline = false;
        wait = API_RETRY_MS;
      }
    }
    // NVS'yi her kayitta degil, dakikada bir guncelle
    if (dirty && (millis() - lastSave > 60000 || wait > API_PERIOD_MS)) {
      p.putULong("upT", upT);
      p.putULong("upE", upE);
      dirty = false;
      lastSave = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

static void apiStart()
{
  if (!API_URL[0]) {
    Serial.println("API: adres tanimli degil, buluta gonderim kapali");
    return;
  }
  xTaskCreatePinnedToCore(apiTask, "api", 10240, nullptr, 1, nullptr, 0);
  Serial.printf("API: %s (cihaz %s)\n", API_URL, API_DEVICE);
}

/* --------------------------- Kurulum ------------------------------------- */

static void printCapacity()
{
  uint32_t capRec = maxLogBytes() / sizeof(LogRec);
  Serial.printf("Depolama: dosya sistemi %u KB, loglara %lu KB -> %lu kayit (%u bayt/kayit)\n",
                (unsigned)(LittleFS.totalBytes() / 1024), (unsigned long)(maxLogBytes() / 1024),
                (unsigned long)capRec, (unsigned)sizeof(LogRec));
  const uint16_t iv[] = {1, 5, 10, 30, 60};
  for (uint16_t s : iv) {
    float hours = capRec * (float)s / 3600.0f;
    Serial.printf("  %2us aralik: %.1f saat (%.1f gun)\n", s, hours, hours / 24.0f);
  }
  Serial.printf("Mevcut: %lu kayit, %u dosya\n",
                (unsigned long)(logBytes / sizeof(LogRec)), (unsigned)logFiles.size());
}

static void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Wi-Fi'ye baglaniliyor: %s", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Baglandi! IP adresi: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi baglanamadi, arka planda denemeye devam edilecek");
  }
}

void setup()
{
  Serial.begin(115200);
  fsMutex = xSemaphoreCreateRecursiveMutex();
  Serial2.setRxBufferSize(4096);
  Serial2.begin(STM_BAUD, SERIAL_8N1, STM_RX_PIN, STM_TX_PIN);
  Serial.println("ESP32 sensor alicisi basladi");

  loadSettings();
  Serial.printf("Set degerleri: sicaklik %.1f C, titresim %.3f g, kayit araligi %u s\n",
                settings.tempSet, settings.vibSet, settings.logInterval);
  loadAnomaly();
  lastAnomSaveMs = millis();

  fsOk = LittleFS.begin(true);
  if (fsOk) {
    scanLogs();
    printCapacity();
  } else {
    Serial.println("HATA: LittleFS baslatilamadi, kayit yapilmayacak");
  }

  connectWiFi();
  configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
  struct tm tmv;
  if (getLocalTime(&tmv, 10000)) {
    char ts[24];
    fmtTime(nowEpoch(), ts, sizeof(ts));
    Serial.printf("Saat ayarlandi: %s\n", ts);
  } else {
    Serial.println("Saat henuz ayarlanamadi (NTP), ayarlanana kadar kayit yapilmaz");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/events", handleEvents);
  server.on("/storage", handleStorage);
  server.on("/log.csv", handleLogCsv);
  server.on("/events.csv", handleEventsCsv);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/anomaly/auto", HTTP_POST, handleAnomalyAuto);
  server.on("/anomaly/reset", HTTP_POST, handleAnomalyAuto);   // eski ad, mevcut sureyle ogrenir
  server.on("/anomaly/manual", HTTP_POST, handleAnomalyManual);
  server.begin();
  Serial.println("Web sunucusu basladi");

  mqttStart();
  apiStart();
  lastLogMs = millis();
}

void loop()
{
  server.handleClient();

  // Wi-Fi koparsa yeniden baglan, IP degisirse yaz
  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi koptu, yeniden baglaniliyor...");
      WiFi.reconnect();
    } else {
      static IPAddress lastIp;
      if (WiFi.localIP() != lastIp) {
        lastIp = WiFi.localIP();
        Serial.print("IP adresi: http://");
        Serial.println(lastIp);
      }
    }
  }

#if LOOPBACK_TEST
  static uint32_t lastFake = 0;
  if (millis() - lastFake > 1000) {
    lastFake = millis();
    Serial2.print("T:12.34,VX:0.001,VY:0.002,VZ:0.003,V:0.004\r\n");
  }
#endif

  pumpSerial();

  if (sensor.packetCount > 0 && millis() - sensor.lastUpdateMs > TIMEOUT_MS) {
    setAlarm(AL_LINK, true, NAN, NAN);
  }

  flushLog();
  mqttProcess();

  static uint32_t lastDiag = 0;
  if (sensor.packetCount == 0 && millis() - lastDiag > 2000) {
    lastDiag = millis();
    Serial.printf("Tani: Serial2 alinan bayt=%lu, bozuk satir=%lu\n",
                  (unsigned long)sensor.rxBytes, (unsigned long)sensor.badLines);
  }
}
