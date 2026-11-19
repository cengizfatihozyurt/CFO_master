# Uzun süreli normal çalışma: gun_2026-11-19

Aralık: 2026-11-19 00:05:00 – 2026-11-19 21:50:00 (21 sa 45 dk) · Koşul: masa üzerinde, sabit; ESP32 ev ağında (Superbox), FW 1.10 · Oluşturma: 2026-11-19 21:55:36

## Yanlış alarm

| Ölçüt | Değer |
| --- | --- |
| Anomali başlangıcı (A01–A04) | 4 |
| Yanlış alarm / saat | 0,18 |
| Eşik alarmı başlangıcı (E01–E04) | 3 |
| Alarm bayraklı 10 s kayıt | 4 |

| Zaman | Kod | Değer (g) | Kaynak |
| --- | --- | --- | --- |
| 2026-11-19 00:05:04 | A03 | 0,118 | olay listesi |
| 2026-11-19 13:55:14 | A01 | 0,542 | olay listesi |
| 2026-11-19 13:55:14 | E02 | 0,542 | olay listesi |
| 2026-11-19 18:34:58 | A01 | 0,678 | olay listesi |
| 2026-11-19 18:34:58 | E02 | 0,678 | olay listesi |
| 2026-11-19 18:47:46 | A01 | 0,542 | olay listesi |
| 2026-11-19 18:47:46 | E02 | 0,542 | olay listesi |

## Kayıt bütünlüğü (cihazın yerel kaydı, buluta ulaşan)

| Ölçüt | Değer |
| --- | --- |
| Beklenen kayıt (10 s aralık) | 7830 |
| Buluta ulaşan kayıt | 7830 (100,0 %) |
| 20 s üstü boşluk | 0 |

## Canlı MQTT yolu (InfluxDB, 1 s)

| Ölçüt | Değer |
| --- | --- |
| Gelen ölçüm / beklenen | 6 / 78300 |
| 60 s üstü kesinti | 1 (toplam 21 sa 45 dk) |
| · 2026-11-19 00:05:05 – 2026-11-19 21:50:00 | 21 sa 45 dk |
| Kesinti sırasında yerelde kaydedilip sonradan buluta ulaşan kayıt | 7829 |
| Bu kayıtların en geç ulaşma gecikmesi | 8 dk |

## Ölçüm istatistikleri (10 s kayıtlar)

| Ölçüt | Değer |
| --- | --- |
| Titreşim (pencere en yükseği, g): medyan / %99 / en yüksek | 0,013 / 0,014 / 0,678 |
| Sıcaklık (°C): en düşük / en yüksek | 17,38 / 17,76 |
| En hızlı sıcaklık artışı (°C/dk, ≥4 dk fark) | 0,065 (A04 eşiği 0,5) |

Yanlış alarm: bu aralıkta uyaran uygulanmadığı için her anomali başlangıcı yanlış alarm sayılır.
Kayıt bütünlüğü cihazın yerel kaydına dayanır; canlı yol kesildiğinde kayıtlar cihazda bekletilip bağlantı gelince gönderilir (depola-ilet).
Olaylar, cihazın olay listesi ve 10 s kayıtlardaki alarm bitleri birleştirilerek sayılır (olay listesinin bir yolda eksik kalması sonucu etkilemez).
