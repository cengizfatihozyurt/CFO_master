# Deney protokolü

Bu protokol, geliştirilen sistemin anomali tespit başarımını tekrarlanabilir ve kayıt altına alınabilir biçimde ölçmek içindir. Deneyler sırasında ESP32 ve STM32 yazılımı değiştirilmez; her deney raporunda kullanılan yazılım sürümü (ESP32 panelindeki FW ve son Git commit) belirtilir.

## Araçlar

| Araç | Görev |
|---|---|
| Node-RED paneli → **Deney Kaydı** | Deney başlatma, uyaran işaretleme, bitirme. İşaretler InfluxDB `experiment` ölçümüne (ms çözünürlük) ve `~/stm32iot_kayit/deneyler.csv` dosyasına yazılır. |
| ESP32 | Anomali kararı (A01–A04); olaylar cihaz saatine göre (NTP) zaman damgalı. |
| `node deney/analiz.js` | İşaretleri olaylarla eşleştirir; `deney/sonuclar/` altında deneme tablosu, yanlış alarm listesi ve özet ölçütler üretir. |

## Her deneyden önce

1. ESP32 panelinde (veya Node-RED'de) saatin ayarlı olduğunu doğrula: **"Saat senkronize değil" uyarısı olmamalı.** Saat yoksa cihaz ölçüm kaydı tutmaz ve olaylar zamansız kalır; deney geçersizdir.
2. Anomali modelinin öğrenilmiş olduğunu doğrula (mod: Duruş). Deney serisi boyunca model değiştirilmez; değişirse not edilir.
3. Set değerleri: 40 °C / 0,300 g / 10 s (E01/E02 sabit eşikleri anomali sonuçlarını etkilemez, ama kayıt aralığı sabit kalmalı).
4. Deney no: her deney için yeni bir numara (1, 2, 3 …). **900 ve üzeri test sayılır**, analize girmez.

## İşaretleme kuralı

- **Önce "Uyaran uygulandı (şimdi)" düğmesine bas, hemen ardından (1–2 s içinde) uyaranı uygula.** Böylece ölçülen gecikme (karar − işaret) negatif olmaz.
- Her uyaran için **tek** işaret. 1,5 s içinde gelen ikinci tıklama yok sayılır (çift tıklama koruması).
- Uyaranlar arasında en az bir önceki olayın bitmesini bekle (A01: ~5 s, A02: ~30 s, A03: sefer bitimi + 10 s, A04: sıcaklık normale dönene kadar).
- Elle işaretleme ±2 s belirsizlik getirir. A01'de gecikme bu belirsizlik düzeyindedir; A02–A04'te yöntemin kendi gecikmesi (onlarca saniye) baskındır.

## Deneyler

| No | Tür | Uygulama | Tekrar | Kaydedilecek ek bilgi |
|---|---|---|---|---|
| 1 | **A01** Ani darbe | Kartın aynı noktasına kontrollü vuruş; üç şiddet (hafif/orta/sert) | Her şiddet için ≥ 10 (hedef 30) | Şiddet alanına seviye; koşula vuruş noktası ve yöntem |
| 2 | **A02** Duruşta titreşim | Karta sabit, küçük bir titreşim kaynağı 60 s uygulanır (ör. titreşen telefon kartın yanında) | ≥ 10 | Kaynak ve mesafe |
| 3 | **A03** Anormal sefer | (a) Normalden uzun sürekli sallama (> 2 dk), (b) normal sürede ama 3 kat güçlü sallama | Her biri ≥ 10 | Süre ve yaklaşık şiddet |
| 4 | **A04** Hızlı ısınma | DS18B20'ye kontrollü ısıtma (ör. sabit mesafeden sıcak hava) ~1 dk; sonra soğumaya bırakılır | ≥ 5 | Isı kaynağı, mesafe; E01 ile karşılaştırma |
| 5 | **NORMAL** Uzun süreli çalışma | Karta hiç dokunulmadan 24–72 saat | 1 (uzun) | Ortam (oda, masa) ve olağan dışı durumlar (kapı çarpması vb.) |
| 6 | **KESİNTİ** İnternet kesintisi | Başlat = Wi-Fi kesildiği an; kesinti boyunca 5 kontrollü vuruş (her biri işaretlenir); Bitir = bağlantı geri geldiği an | 1–3 (farklı süreler: 10 / 30 / 60 dk) | Kesinti yöntemi (modem Wi-Fi kapatma) |

Normal sefer verisi masa üstünde yoktur; A03 için "normal sefer" sallamalarıyla önce sefer profili öğretilmelidir (10 normal sefer) ve bu öğretim ayrı bir deney numarasıyla kaydedilir.

## Kesinti deneyi ayrıntısı

1. Saat ayarlı ve sistem normal çalışırken (ESP32 paneli "Bulut API: bağlı, bekleyen 0") deneyi **KESİNTİ** türüyle başlat ve hemen modemin Wi-Fi'sini kapat.
2. Kesinti boyunca 5 vuruş yap; her birini işaretle. (İşaretler bilgisayar tarafında tutulur; bilgisayar ESP32 ile aynı ağda olmak zorunda değildir.)
3. Wi-Fi'yi aç ve aynı anda **Deneyi bitir**.
4. ESP32 panelinde "bekleyen" sayısı 0'a inene kadar bekle (biriken kayıtlar gönderilir).
5. `node deney/analiz.js --kesinti <no>` çalıştır. Rapor: kesintide tespit edilen darbe sayısı, yerel ve bulut kayıt sayıları, eksik ve çift kayıt, aktarım gecikmesi (`received_at − ölçüm`), son kaydın buluta ulaşma süresi, canlı MQTT akışındaki boşluk.

**Not:** Kesinti sırasında ESP32 yeniden başlarsa saat kaybolur ve yeni ölçüm kaydı tutulmaz (cihazın saati yalnızca NTP ile ayarlanır). Bu, sistemin bilinen bir sınırlılığıdır; deneyde cihazın elektriği kesilmemelidir.

## Analiz

```
node deney/analiz.js                 tüm deneyler → deney/sonuclar/ozet.md, denemeler.csv, yanlis_alarmlar.csv
node deney/analiz.js --deney 3       tek deney
node deney/analiz.js --kesinti 6     kesinti raporu → deney/sonuclar/kesinti_6.md
```

Eşleştirme: her uyaran işaretinden sonraki pencerede (A01 −5/+15 s, A02 −5/+120 s, A03 −5/+300 s, A04 −5/+600 s) beklenen koddaki ilk anomali başlangıcı **doğru tespit** (TP) sayılır; yoksa **kaçırılan** (FN). Deney süresince hiçbir uyaranla eşleşmeyen her anomali başlangıcı **yanlış alarm** (FP) sayılır.

Ölçütler: Precision = TP / (TP + FP), Recall = TP / (TP + FN), F1 = 2·P·R / (P + R), yanlış alarm / saat, tespit gecikmesi (ortalama, medyan, en çok).

## Raporlama

- Masa üstü kontrollü deneyler ile gerçek asansörden toplanan normal çalışma verileri **ayrı doğrulama grupları** olarak raporlanır.
- Simülasyon sonuçları (30 senaryo) ön doğrulamadır; genel başarı oranı olarak sunulmaz.
- Gerçek asansörde kontrollü arıza oluşturulmaz; ölçümler yetkili personel gözetiminde, güvenlik sistemlerine müdahale edilmeden yapılır.
