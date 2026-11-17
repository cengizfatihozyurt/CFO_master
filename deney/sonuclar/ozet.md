# Deney sonuçları

Oluşturma: 2026-11-17 23:49:33 · Deney sayısı: 4

| Tür | Deney | Deneme | TP | FN | FP | Precision | Recall | F1 | Gecikme ort. / medyan / en çok (s) | Yanlış alarm / saat |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A01 Ani darbe | 4 | 35 | 32 | 3 | 0 | 100,0 % | 91,4 % | 95,5 % | 1,7 / 1,5 / 5 | 0,00 |

**Veri düzeltmeleri ve iletim notları:**

- Oturum 1-1 analiz dışı bırakıldı.
- Oturum 1-2, uyaran 1: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 3: bitiş işareti verilmediği için bitiş 23:41:30 alındı (Bitir'e basilmadi; 10. denemeden sonraki tespitler masanin oynamasindan (kullanici dogruladi)).
- Oturum 1-3, 23:29:31: yanlış alarm sayılmadı (Ilk isaretten once yapilan deneme vurusu (kullanici dogruladi, tepe 12,2 g)).
- Oturum 3, 23:40:59: yanlış alarm sayılmadı (Deneme 5-6 vuruslarinin gec islenmesi: ESP32 dongusu 23:40:35-23:41:00 arasi takildi, biriken satirlar topluca islendi (yerel kayitta 39 s bosluk); denemeler kacirma sayildi).
- Oturum 3, 23:41:00: yanlış alarm sayılmadı (Deneme 5-6 vuruslarinin gec islenmesi (ayni takilma)).
- Oturum 3, 23:41:01: yanlış alarm sayılmadı (Deneme 5-6 vuruslarinin gec islenmesi (ayni takilma)).
- İletim: 32 anomali olayından 3 tanesi canlı MQTT/InfluxDB yolunda yok (diğer kaynaklardan tamamlandı): A01 2026-11-17 23:30:48, A01 2026-11-17 23:40:14, A01 2026-11-17 23:41:13

TP: doğru tespit · FN: kaçırılan · FP: yanlış alarm (deney süresince eşleşmeyen anomali başlangıcı).
Precision = TP / (TP + FP) · Recall = TP / (TP + FN) · F1 = 2·P·R / (P + R).
Gecikme: uyaran işaretinden ESP32 kararına (s, ESP32 olay zamanı 1 s çözünürlüklü; dakikalık ortalama kullanan A04 ve sefer sonunda karar veren A03 için doğal gecikme vardır).
Arama pencereleri (s): A01 −5/+15, A02 −5/+120, A03 −5/+300, A04 −5/+600.
