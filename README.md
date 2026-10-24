# STM32 IoT Sensör İzleme

STM32F103C8T6 ile sıcaklık (DS18B20) ve titreşim (ADXL345) ölçümü; ESP32 üzerinden yerel web paneli, MQTT (Node-RED / Grafana) ve bulut API'sine aktarım.

```
DS18B20 (PA1) ┐
              ├─ STM32F103 ──UART2──> ESP32 ──┬─ Web paneli + yerel kayıt (LittleFS)
ADXL345 (SPI1)┘                               ├─ MQTT ──> Mosquitto ──> Node-RED ──> InfluxDB ──> Grafana
                                              └─ HTTPS ──> Bulut API (Render) ──> Postgres
```

## Klasörler

| Klasör | İçerik |
|---|---|
| `Core/` | STM32 uygulaması: `main.c`, `ds18b20.c`, `adxl345.c` (CubeMX'siz, elle yazıldı) |
| `ESP32/esp32_sensor_rx/` | ESP32 Arduino kodu: web paneli, kayıt, alarmlar, MQTT, API gönderimi |
| `ESP32/node-red/` | Node-RED akışı ve kurulum notları (`KURULUM.md`) |
| `ESP32/grafana/` | Grafana panel tanımı |
| `api/` | Bulut veri API'si (Node.js + Postgres) |
| `render.yaml` | Render Blueprint (API + veritabanı) |
| `clone_f103*.cfg`, `*.launch` | Klon STM32F103 için OpenOCD / CubeIDE debug ayarları |

## Bağlantılar

| STM32 | Görev |
|---|---|
| PA1 | DS18B20 DATA (4.7k pull-up) |
| PA4 / PA5 / PA6 / PA7 | ADXL345 CS / SCL / SDO / SDA (SPI1, mod 3, 1 MHz) |
| PA2 / PA3 | USART2 TX / RX → ESP32 GPIO16 (RX2) / GPIO17 (TX2), 115200 |

## ESP32 ilk kurulum

1. `ESP32/esp32_sensor_rx/secrets.example.h` dosyasını aynı klasöre `secrets.h` adıyla kopyala ve Wi-Fi, MQTT ve API bilgilerini doldur. (`secrets.h` GitHub'a gönderilmez.)
2. Arduino IDE → kart: **DOIT ESP32 DEVKIT V1** → yükle.

## Bulut API (Render)

ESP32 her kayıt aralığında (varsayılan 10 s) ölçümleri ve olayları HTTPS ile API'ye toplu gönderir.

- Sunucuya ulaşılamazsa veriler ESP32'nin hafızasında (LittleFS, ~76.000 ölçüm) birikir.
- Sunucu açılınca biriken veriler eskiden yeniye gönderilir. Gönderilen son kaydın zamanı ESP32'nin kalıcı belleğinde tutulduğu için elektrik kesilse de kaldığı yerden devam eder.
- API aynı kaydı iki kez yazmaz.

Yayına alma: Render → **New → Blueprint** → bu repo. `API_KEY` sorulduğunda ESP32'deki `SECRET_API_KEY` ile aynı değeri gir. Servis adresini (`https://<ad>.onrender.com`) `secrets.h` içindeki `SECRET_API_URL`'ye yazıp ESP32'yi yeniden yükle.

### Bulut paneli

https://stm32iot-api.onrender.com adresi tarayıcıda bir panel açar: canlı değerler, aktif alarmlar, seçilebilir zaman aralığında grafik (1 saat – tümü), CSV indirme ve olay geçmişi. Girişte erişim anahtarı sorulur:

- `VIEW_KEY` (önerilen): Render → servis → **Environment** bölümüne eklenen, **sadece okuma** yetkili panel şifresi. Arkadaşlarla bu paylaşılır; bu şifreyle veri yazılamaz.
- `VIEW_KEY` tanımlı değilse `API_KEY` ile de giriş yapılabilir (yazma yetkili, paylaşmayın).

| Uç | Açıklama |
|---|---|
| `GET /` | Bulut paneli |
| `GET /health` | Sağlık kontrolü |
| `GET /api/v1/series` | `?device=&from=&to=&points=` grafik için gruplanmış veri |
| `POST /api/v1/telemetry` | `{"device","records":[{"t","temp","vx","vy","vz","v","flags"}]}` |
| `POST /api/v1/events` | `{"device","events":[{"t","code","state","value","limit","desc"}]}` |
| `GET /api/v1/telemetry` | `?device=&from=&to=&limit=` |
| `GET /api/v1/events` | `?device=&from=&to=&limit=` |
| `GET /api/v1/latest` | Cihaz başına son kayıt ve sayılar |

`/` ve `/health` dışındaki tüm uçlar `X-API-Key` başlığı ister. Yazma uçları sadece `API_KEY`, okuma uçları `API_KEY` ya da `VIEW_KEY` kabul eder.

**Render ücretsiz plan notları:** Web servisi ayda 750 saate kadar ücretsizdir (7/24 tek servis için yeterli). 15 dakika istek gelmezse uyur; ESP32 sürekli gönderdiği için uyumaz. **Ücretsiz Postgres 30 gün sonra silinir.** Kalıcı saklama için veritabanını ücretli plana yükselt ya da `DATABASE_URL`'yi süresiz ücretsiz bir Postgres'e yönlendir.

## Hata kodları

| Kod | Anlamı |
|---|---|
| E01 | Sıcaklık set değerinin üzerinde |
| E02 | Titreşim set değerinin üzerinde |
| E03 | DS18B20 sensör hatası |
| E04 | STM32 veri iletişimi kesildi |
| I01 | Set değerleri güncellendi |
| I02 | Kayıtlar silindi |
