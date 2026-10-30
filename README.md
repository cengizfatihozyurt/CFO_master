# STM32 IoT Sensör İzleme

STM32F103C8T6 ile sıcaklık (DS18B20) ve titreşim (ADXL345) ölçümü; ESP32 üzerinden yerel web paneli, MQTT (Node-RED / Grafana) ve bulut API'sine aktarım.

```
DS18B20 (PA1) ┐
              ├─ STM32F103 ──UART2──> ESP32 ──┬─ Web paneli + yerel kayıt (LittleFS)
ADXL345 (SPI1)┘                               ├─ MQTT (TLS) ──> HiveMQ Cloud ──köprü──> Mosquitto ──> Node-RED ──> InfluxDB ──> Grafana
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

**Wi-Fi:** `secrets.h` içinde iki ağ tanımlanabilir (`SECRET_WIFI_SSID` ve isteğe bağlı `SECRET_WIFI2_SSID`, ör. telefon hotspotu). Bağlantı yoksa ESP32 bilinen ağları 20 s arayla sırayla dener. **ESP32 yalnızca 2,4 GHz ağlara bağlanır**; telefon hotspotunda 2,4 GHz / "Uyumluluğu en üst düzeye çıkar" seçilmelidir.

**Saat:** ESP32 saati yalnızca internetten (NTP) alır. Açılışta internet yoksa saat ayarlanana kadar ölçüm kaydı tutulmaz ve olaylar "saat yok" olarak yazılır (bilinen sınırlılık).

## Bulut MQTT (HiveMQ Cloud)

ESP32 doğrudan HiveMQ Cloud'a TLS (8883) ile bağlanır; böylece hangi ağda olursa olsun (ev, hotspot, asansör) veri buluta gider. Bilgisayardaki Mosquitto, kalıcı oturumlu (`cleansession false`) bir köprüyle cihaz mesajlarını buluttan alır; Node-RED, InfluxDB ve Grafana yerel broker'dan okumaya devam eder.

| Yön | Konular |
|---|---|
| Bulut → yerel (köprü) | `telemetry`, `event`, `status`, `info`, `settings`, `anomaly` |
| Yerel → bulut (köprü) | `settings/set` (Node-RED panelinden set ve anomali komutları) |

Kurulum (Windows, yönetici):

1. HiveMQ Cloud → cluster → **Access Management** → Publish and Subscribe yetkili kullanıcı (`stm32iot-kopru`).
2. [`ESP32/mosquitto/kopru_hivemq.conf.example`](ESP32/mosquitto/kopru_hivemq.conf.example) dosyasını `C:\ProgramData\STM32IoT\mosquitto\conf.d\kopru_hivemq.conf` olarak kopyala, `remote_password`'u doldur; dosyayı yalnızca SYSTEM/Administrators okuyacak şekilde kısıtla.
3. Let's Encrypt kök sertifikasını (ISRG Root X1, Windows sertifika deposunda var) `C:\ProgramData\STM32IoT\mosquitto\isrg_root_x1.pem` olarak dışa aktar.
4. `mosquitto.conf` sonuna `include_dir C:\ProgramData\STM32IoT\mosquitto\conf.d` ekle ve Mosquitto servisini yeniden başlat.

Doğrulama (2026-10-25, köprünün ilk hâli: yerel → bulut): HiveMQ Cloud'a doğrudan abone olunarak telemetri/durum/bilgi mesajları görüldü; buluttan gönderilen `settings/set` komutu ESP32'de I01 olayı üretti. 2026-10-30'da ESP32'nin evden farklı ağlarda da çalışabilmesi için yön tersine çevrildi (ESP32 → HiveMQ → köprü → yerel).

**Dikkat:** HiveMQ Cloud Serverless (ücretsiz) 30.09.2026'dan beri yeni cluster kabul etmiyor ve **31.12.2026'da kapanıyor** (veriler 07.01.2027'de silinir). Öncesinde köprü ücretsiz bir başka bulut broker'a (ör. EMQX Cloud Serverless) taşınmalı; yalnızca `address`, kullanıcı ve sertifika değişir.

## Bulut API (Render)

ESP32 her kayıt aralığında (varsayılan 10 s) ölçümleri ve olayları HTTPS ile API'ye toplu gönderir.

- Sunucuya ulaşılamazsa veriler ESP32'nin hafızasında (LittleFS, ~76.000 ölçüm) birikir.
- Sunucu açılınca biriken veriler eskiden yeniye gönderilir. Gönderilen son kaydın zamanı ESP32'nin kalıcı belleğinde tutulduğu için elektrik kesilse de kaldığı yerden devam eder.
- API aynı kaydı iki kez yazmaz.

Yayına alma: Render → **New → Blueprint** → bu repo. `API_KEY` sorulduğunda ESP32'deki `SECRET_API_KEY` ile aynı değeri gir. Servis adresini (`https://<ad>.onrender.com`) `secrets.h` içindeki `SECRET_API_URL`'ye yazıp ESP32'yi yeniden yükle.

### Bulut paneli

https://stm32iot-api-fh72.onrender.com adresi tarayıcıda bir panel açar: canlı değerler, aktif alarmlar, seçilebilir zaman aralığında grafik (1 saat – tümü), CSV indirme ve olay geçmişi. Girişte erişim anahtarı sorulur:

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
| A01 | Anomali: ani darbe |
| A02 | Anomali: duruşta anormal titreşim |
| A03 | Anomali: anormal sefer / uzun süreli titreşim |
| A04 | Anomali: hızlı ısınma |
| I01 | Set değerleri güncellendi |
| I02 | Kayıtlar silindi |
| I03 | Anomali modeli öğrenildi |
| I04 | Otomatik öğrenme başlatıldı |
| I05 | Anomali değerleri elle girildi |

## Anomali tespiti (uç birim)

Sabit eşik alarmlarına (E01/E02) ek olarak ESP32, cihazın **normal davranışını öğrenip ondan sapmayı** yakalar. Kod: [`ESP32/esp32_sensor_rx/anomali.h`](ESP32/esp32_sensor_rx/anomali.h) (Arduino'dan bağımsız saf C++).

1. **Normal durum (iki yol; ESP32 web panelindeki "Anomali Tespiti" kartından ya da Node-RED panelindeki "Anomali Ayarları" grubundan seçilir):**
   - **Otomatik öğrenme:** süre kullanıcı tarafından seçilir (1–240 dk, varsayılan 10). Bu sürede duruş titreşiminin ortalaması (μ) ve standart sapması (σ) hesaplanır; temas/darbe gibi aşırı değerler dışarıda bırakılır. Sonra μ/σ yalnızca normal duruş ölçümleriyle yavaşça (EWMA, ~30 dk) güncellenir ve normal seferlerden sefer profili öğrenilir.
   - **Manuel giriş:** duruş ortalaması μ, sapma σ, en uzun normal sefer (s) ve sefer tepe sınırı (g) elle girilir; sistem bunları kendiliğinden değiştirmez (EWMA ve sefer profili kapalı). "Mevcut değerleri doldur" düğmesi öğrenilmiş değerleri forma getirir. Ani darbe eşiği, sefer tepe sınırının 2 katıdır (en az 0,15 g).

   Model NVS'de saklanır, elektrik kesilince kaybolmaz. MQTT `settings/set` ile de: `{"learnmin":N}` (otomatik, N dk), `{"relearn":1}` (mevcut süreyle), `{"man_mu":..,"man_sigma":..,"man_ridemax":..,"man_peak":..}` (manuel).
2. **Durum ayrımı:** z = (v − μ) / σ. z > 6 (2 s) → **hareket** (sefer), z < 3 (3 s) → **duruş**. Asansör hareket ederken titreşim doğal olarak arttığı için her sefer alarm vermez. Normal seferlerin süresi ve tepe titreşimi ayrıca öğrenilir (sefer profili, 10 seferden sonra devreye girer).
3. **Anomaliler:**

| Kod | Koşul |
|---|---|
| A01 | Tek ölçümde v > max(0,15 g, μ + 30σ, 2 × sefer tepe sınırı) |
| A02 | Duruşta son 10 s'nin **medyanı** > μ + 4σ (tekil darbeler etkilemez) |
| A03 | Sefer süresi > ort + 4·std (en fazla 120 s) ya da sefer tepesi > ort + 4·std |
| A04 | Son 5 dakikadaki sıcaklık artışı > 0,5 °C/dk |

Durum ve skor MQTT telemetrisinde `ast` (0 öğreniyor, 1 duruş, 2 hareket) ve `az` (z-skoru) alanlarıyla yayınlanır; Grafana'da "Anomali skoru" ve "Anomali zaman çizelgesi" panellerinde görünür. Cihaz asansöre takıldıktan sonra bir kez **otomatik öğrenme** başlatılmalı ya da değerler **manuel** girilmelidir.

### Simülatör ile doğrulama

[`ESP32/anomali_sim`](ESP32/anomali_sim) aynı `anomali.h`'yi bilgisayarda çalıştırır:

```
g++ -O2 -std=c++17 -o anomali_sim.exe anomali_sim.cpp
anomali_sim.exe kayit.csv [cikti.csv]                     # gerçek 1 s kayıtları (t,v,temp)
set GERCEK_VERI=kayit.csv & anomali_sim.exe --senaryo     # gerçek gürültü + bilinen olaylar
```

Senaryo; gerçek duruş gürültüsünden örneklenen arka plana 25 normal sefer, iki darbe (biri canlı testteki çift vuruş), duruşta 60 s hafif titreşim (+4,5σ), 200 s süren bir sefer, 3 kat titreşimli bir sefer ve 10 dk ısınma ekler. Sonuçlar (2026-10-25, her satır 30 farklı rastgele senaryo):

| Mod | Sonuç |
|---|---|
| Otomatik, 3 / 10 / 30 dk öğrenme | 6/6 olay, 0 yanlış alarm |
| Manuel (μ 0,0123, σ 0,002, sefer ≤ 40 s, tepe ≤ 0,10 g) | 6/6 olay, 0 yanlış alarm |
| Manuel, tepe ≤ 0,08 g (sıkı) | 6/6 olay; 11 senaryoda güçlü anormal sefer A03'e ek olarak A01 de üretti (darbe eşiği = 2 × tepe sınırı) |

Gerçek 41 dakikalık masa üstü kaydında, her iki modda da yalnızca bir gerçek darbe (0,265 g) A01 olarak işaretlendi.

### Canlı test (2026-10-25, kart masa üstünde)

| Yapılan | Sonuç |
|---|---|
| Karta sert vuruş | A01, 0,764 g (eşik 0,150 g), anında |
| ~24 s ve ~11 s sürekli sallama | "Hareket" (sefer) olarak ayrıldı, alarm yok |
| Duruş süreleri | z ≈ 0,5–1,5, A02 yanlış alarmı yok |

Test bir hata da ortaya çıkardı: vuruş 3 s "hareket" sayılıp **sefer profiline girdi** ve öğrenilen sefer tepesini 0,32 g'ye şişirdi (gerçek sallamalar ~0,07 g). Bu, sonraki anormal seferleri ve darbeleri gizler; simülatörde yeniden üretildi (6 olaydan 2'si kaçırıldı). Düzeltme: sefer sırasında ya da hemen öncesinde A01 olduysa o sefer profile alınmaz.
