# Kesinti deneyi 17

Kesinti: 2026-11-22 12:05:09 → 2026-11-22 12:35:00 (29,9 dk) · Koşul: Modemi kapattım 

| Ölçüt | Değer |
| --- | --- |
| Uyaran (kontrollü darbe) sayısı | 6 |
| Kesintide tespit edilen darbe (A01) | 6 / 6 |
| ESP32 yerel kayıt sayısı (pencerede) | 180 |
| Buluta ulaşan kayıt | 180 (100,0 %) |
| Eksik kayıt | 0 |
| Tekrar (çift) kayıt | 0 |
| Anomali olayı: yerel / bulut | 7 / 7 |
| Aktarım gecikmesi (received_at − ölçüm), medyan / en çok | 1485 s / 1992 s |
| Bağlantı gelince son kaydın buluta ulaşması | 601 s |
| Canlı MQTT telemetrisi (InfluxDB) | 12 / ~1791 s (QoS0 akış kesintide birikmez) |

**Not:** Modem yaklaşık 12:35'te takıldı; bilgisayar en geç 12:38'de internetteydi, ESP32'nin canlı MQTT akışı ise 12:44:28'de, bulut yüklemesi 12:45'te geri geldi. Yeniden bağlanma ~6–9 dk sürdü; nedeni 60 dk kesinti deneyinde seri çıktıyla incelenecek.
