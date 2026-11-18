# Uzun süreli normal çalışma: gece_2026-11-18

Aralık: 2026-11-18 00:25:00 – 2026-11-18 22:00:00 (21 sa 35 dk) · Koşul: masa üzerinde, sabit; ESP32 telefon hotspot'unda · Oluşturma: 2026-11-18 22:49:17

## Yanlış alarm

| Ölçüt | Değer |
| --- | --- |
| Anomali başlangıcı (A01–A04) | 1 |
| Yanlış alarm / saat | 0,05 |
| Eşik alarmı başlangıcı (E01–E04) | 1 |
| Alarm bayraklı 10 s kayıt | 1 |

| Zaman | Kod | Değer (g) | Kaynak |
| --- | --- | --- | --- |
| 2026-11-18 11:20:37 | A01 | 0,539 | olay listesi |
| 2026-11-18 11:20:37 | E02 | 0,539 | olay listesi |

## Kayıt bütünlüğü (cihazın yerel kaydı, buluta ulaşan)

| Ölçüt | Değer |
| --- | --- |
| Beklenen kayıt (10 s aralık) | 7770 |
| Buluta ulaşan kayıt | 7770 (100,0 %) |
| 20 s üstü boşluk | 0 |

## Canlı MQTT yolu (InfluxDB, 1 s)

| Ölçüt | Değer |
| --- | --- |
| Gelen ölçüm / beklenen | 0 / 77700 |
| 60 s üstü kesinti | 1 (toplam 21 sa 35 dk) |
| · 2026-11-18 00:25:00 – 2026-11-18 22:00:00 | 21 sa 35 dk |
| Kesinti sırasında yerelde kaydedilip sonradan buluta ulaşan kayıt | 7770 |
| Bu kayıtların en geç ulaşma gecikmesi | 15 sa 11 dk |

## Ölçüm istatistikleri (10 s kayıtlar)

| Ölçüt | Değer |
| --- | --- |
| Titreşim (pencere en yükseği, g): medyan / %99 / en yüksek | 0,012 / 0,013 / 0,539 |
| Sıcaklık (°C): en düşük / en yüksek | 16,71 / 17,63 |
| En hızlı sıcaklık artışı (°C/dk, ≥4 dk fark) | 0,014 (A04 eşiği 0,5) |

Yanlış alarm: bu aralıkta uyaran uygulanmadığı için her anomali başlangıcı yanlış alarm sayılır.
Kayıt bütünlüğü cihazın yerel kaydına dayanır; canlı yol kesildiğinde kayıtlar cihazda bekletilip bağlantı gelince gönderilir (depola-ilet).
Olaylar, cihazın olay listesi ve 10 s kayıtlardaki alarm bitleri birleştirilerek sayılır (olay listesinin bir yolda eksik kalması sonucu etkilemez).

**Not:** 11:20:37'deki A01 (0,54 g) gündüz, evde kişiler varken oluştu; masaya dokunulmuş ya da çarpılmış olabilir (kullanıcı: olabilir), ancak doğrulanamadı. Bu nedenle tabloda yanlış alarm olarak sayılmıştır (en kötü durum 0,05/saat); dış etki kabul edilirse yanlış alarm 0'dır.
