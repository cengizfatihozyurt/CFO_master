# Deney sonuçları

Oluşturma: 2026-11-17 23:34:57 · Deney sayısı: 2

| Tür | Deney | Deneme | TP | FN | FP | Precision | Recall | F1 | Gecikme ort. / medyan / en çok (s) | Yanlış alarm / saat |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A01 Ani darbe | 2 | 14 | 14 | 0 | 0 | 100,0 % | 100,0 % | 100,0 % | 1,8 / 1,5 / 5 | 0,00 |

**Veri düzeltmeleri ve iletim notları:**

- Oturum 1-1 analiz dışı bırakıldı.
- Oturum 1-2, uyaran 1: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 1-3, 23:29:31: işaretlenmemiş deneme uyaranının tespiti (yanlış alarm değil) olarak dışlandı.
- İletim: 14 anomali olayından 1 tanesi canlı MQTT/InfluxDB yolunda yok (diğer kaynaklardan tamamlandı): A01 2026-11-17 23:30:48

TP: doğru tespit · FN: kaçırılan · FP: yanlış alarm (deney süresince eşleşmeyen anomali başlangıcı).
Precision = TP / (TP + FP) · Recall = TP / (TP + FN) · F1 = 2·P·R / (P + R).
Gecikme: uyaran işaretinden ESP32 kararına (s, ESP32 olay zamanı 1 s çözünürlüklü; dakikalık ortalama kullanan A04 ve sefer sonunda karar veren A03 için doğal gecikme vardır).
Arama pencereleri (s): A01 −5/+15, A02 −5/+120, A03 −5/+300, A04 −5/+600.
