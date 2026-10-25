# STM32 IoT – MQTT + Node-RED Kurulumu

```
STM32F103 --UART--> ESP32 --Wi-Fi/MQTT--> Bulut broker --MQTT--> Node-RED (dashboard + CSV kayıt)
                      ^                                              |
                      +-------------- settings/set (set değerleri) --+
```

## 1. Node-RED kurulumu (bilgisayar)

```bash
npm install -g --unsafe-perm node-red
```

```bash
node-red
```

Tarayıcıda editör: http://localhost:1880

Dashboard eklentisi: sağ üst menü → **Manage palette** → **Install** → `@flowfuse/node-red-dashboard` (Dashboard 2.0).

## 2. Akışı içe aktarma

Menü → **Import** → `stm32iot_flow.json` dosyasını seç → **Import** → **Deploy**.

İçe aktardıktan sonra "STM32 IoT İzleme" sekmesine çift tıkla → **Environment Variables**: `INFLUX_TOKEN` değeri repoda `INFLUX_TOKEN_BURAYA` olarak durur; InfluxDB'de oluşturulan token'ı yaz. MQTT broker düğümüne de `nodered` kullanıcısının şifresini gir. Sonra **Deploy**.

Panel: http://localhost:1880/dashboard/izleme

Aynı ağdaki telefondan: `http://<bilgisayarın IP adresi>:1880/dashboard/izleme`

## 3. Panelde neler var

| Bölüm | İçerik |
|---|---|
| Canlı Değerler | Sıcaklık ve toplam titreşim göstergeleri, anlık değerler |
| Durum ve Alarmlar | ESP32 çevrimiçi/çevrimdışı, veri akışı (5 sn veri gelmezse uyarı), aktif alarmlar, son veri zamanı, IP / Wi-Fi gücü |
| Sıcaklık / Titreşim Grafiği | Son 30 dakika, set çizgisiyle birlikte; X/Y/Z eksenleri lejanttan açılıp kapatılır |
| Set Değerleri | Cihazdaki güncel değerler + form; "Cihaza gönder" MQTT ile ESP32'ye yollar, ESP32 kaydeder (elektrik kesilse de kalır) |
| Alarm / Olay Geçmişi | Son 200 olay, aranabilir tablo; alarm başlayınca kırmızı bildirim |

## 4. Kayıtlar (veri toplama)

Node-RED her telemetri mesajını (saniyede 1) günlük CSV dosyasına yazar:

- `C:\Users\<kullanıcı>\stm32iot_kayit\YYYY-MM-DD.csv`
- `C:\Users\<kullanıcı>\stm32iot_kayit\olaylar.csv`

Dosyalar `;` ayraçlı ve ondalıklar virgüllü olduğu için Türkçe Excel'de doğrudan açılır. Günde yaklaşık 5 MB yer kaplar.

ESP32 kendi üzerinde de 16 bayt/kayıtla ~8.9 gün (10 s aralık) veri tutar. İnternet kesilse bile bu yerel kayıt devam eder.

## 5. MQTT konuları

Önek: `stm32iot-fa6dbb/cihaz01` (ESP32 kodunda `MQTT_BASE`, Node-RED'de MQTT düğümlerinin konusu)

| Konu | Yön | İçerik |
|---|---|---|
| `<önek>/telemetry` | ESP32 → | Her saniye: `{"ts","temp","vx","vy","vz","v","flags","alarms","tset","vset"}` |
| `<önek>/event` | ESP32 → | Olay: `{"ts","code","state","value","limit","desc"}` (QoS 1) |
| `<önek>/status` | ESP32 → | `online` / `offline` (retained, ESP32 koparsa broker `offline` yayınlar) |
| `<önek>/info` | ESP32 → | `{"ip","rssi","fw"}` (retained) |
| `<önek>/settings` | ESP32 → | Güncel set değerleri (retained) |
| `<önek>/settings/set` | → ESP32 | `{"tset":40,"vset":0.5,"logint":10}` |

Hata kodları: E01 sıcaklık seti aşıldı, E02 titreşim seti aşıldı, E03 DS18B20 hatası, E04 STM32 iletişimi kesildi, I01 set değiştirildi, I02 kayıtlar silindi.

## 6. Kullanılan broker: Mosquitto (bu bilgisayar)

Şu anki kurulumda broker, Node-RED ile aynı bilgisayarda çalışan **Mosquitto 2.1.2**:

- Windows servisi olarak çalışır, bilgisayar açılınca kendiliğinden başlar.
- Şifresiz bağlantı kapalıdır. Kullanıcılar `esp32` ve `nodered`; şifreler `mqtt_kullanicilar.txt` dosyasında.
- ESP32 broker'a `mqtt://192.168.1.115:1883` adresinden bağlanır. Bilgisayarın IP adresi değişirse ESP32 bağlanamaz; modemde bu bilgisayara sabit IP (DHCP rezervasyonu) verin ya da `MQTT_URI`'yi güncelleyip ESP32'yi yeniden yükleyin.
- Windows Güvenlik Duvarı'nda "Mosquitto MQTT (yerel ag)" kuralı 1883 portunu yalnızca ev ağına açar.

## 7. Grafana + InfluxDB (uzun süreli geçmiş)

```
Mosquitto --> Node-RED --HTTP--> InfluxDB 2.7 (bucket "sensor", 1 yıl) --> Grafana 13
```

- Node-RED, her telemetri ve olay mesajını InfluxDB'ye de yazar ("InfluxDB'ye yaz" düğümü; durumu düğümün altında görünür).
- Grafana paneli: http://localhost:3000 → "STM32 IoT Sensör İzleme" (ana sayfa). İçerik: son değerler, aktif alarm, son veri süresi, sıcaklık ve titreşim grafikleri (set çizgileriyle), alarm zaman çizelgesi, olay tablosu. 5 saniyede bir yenilenir; sağ üstten zaman aralığı seçilebilir.
- Panel tanımı: `ESP32/grafana/stm32iot_dashboard.json` (Grafana → Dashboards → Import ile geri yüklenebilir).
- Giriş bilgileri ve anahtarlar: `mqtt_kullanicilar.txt`.
- Mosquitto ve Grafana Windows servisidir, kendiliğinden başlar. InfluxDB ve Node-RED için `ESP32/baslat.bat` dosyasına çift tıkla.

Aşağıdaki bölüm, ileride bir bulut broker'a geçmek istersen geçerlidir.

## 8. Broker: test → kalıcı kullanım

Varsayılan broker **broker.hivemq.com** herkese açıktır ve şifre istemez. Kurulumu hemen denemek için uygundur. Ancak konu önekini bilen herkes verileri görebilir ve set değerlerini değiştirebilir. Kalıcı kullanım için ücretsiz **HiveMQ Cloud** önerilir (TLS + kullanıcı adı/şifre):

1. https://www.hivemq.com/mqtt/ → ücretsiz "Serverless" cluster oluştur.
2. **Access Management** bölümünde bir kullanıcı adı/şifre oluştur.
3. ESP32 kodunda:
   ```cpp
   #define MQTT_URI   "mqtts://<cluster-id>.s1.eu.hivemq.cloud:8883"
   #define MQTT_USER  "<kullanıcı>"
   #define MQTT_PASS  "<şifre>"
   ```
4. Node-RED'de **MQTT Broker** yapılandırma düğümünü aç:
   - Server: `<cluster-id>.s1.eu.hivemq.cloud`, Port: `8883`
   - **Use TLS** işaretle (varsayılan TLS ayarı yeterli)
   - **Security** sekmesi: kullanıcı adı/şifre
   - Deploy

Node-RED'i de bulutta çalıştırmak istersen (bilgisayar kapalıyken de izleme) FlowFuse Cloud ya da bir VPS kullanılabilir. Akış dosyası aynen içe aktarılır.
