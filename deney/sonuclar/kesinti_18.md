# Kesinti deneyi 18

Kesinti: 2026-11-22 13:27:20 → 2026-11-22 14:27:44 (60,4 dk) · Koşul: Modemi kapattım 

| Ölçüt | Değer |
| --- | --- |
| Uyaran (kontrollü darbe) sayısı | 7 |
| Kesintide tespit edilen darbe (A01) | 7 / 7 |
| ESP32 yerel kayıt sayısı (pencerede) | 363 |
| Buluta ulaşan kayıt | 363 (100,0 %) |
| Eksik kayıt | 0 |
| Tekrar (çift) kayıt | 0 |
| Anomali olayı: yerel / bulut | 10 / 10 |
| Aktarım gecikmesi (received_at − ölçüm), medyan / en çok | 1993 s / 3760 s |
| Bağlantı gelince son kaydın buluta ulaşması | 236 s |
| Canlı MQTT telemetrisi (InfluxDB) | 12 / ~3624 s (QoS0 akış kesintide birikmez) |

**Yeniden bağlanma (seri çıktı):** Wi-Fi 13:27:41'de koptu; cihaz bilinen 3 ağı 20'şer s sırayla denedi (Superbox her 60 s'de bir). Modem 14:27:43'te takıldı; 14:28:42'deki Superbox denemesi başarısız (modem açılıyordu), 14:29:42'deki denemede bağlandı, MQTT 14:29:49'da bağlandı. Yeniden bağlanma ~2 dk; süreyi modemin açılışı ve sıralı deneme aralığı belirliyor.
