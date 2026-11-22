# Deney sonuçları

Oluşturma: 2026-11-22 14:27:48 · Deney sayısı: 15

| Tür | Deney | Deneme | TP | FN | FP | Precision | Recall | F1 | Gecikme ort. / medyan / en çok (s) | Yanlış alarm / saat |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A01 Ani darbe | 6 | 53 | 45 | 8 | 0 | 100,0 % | 84,9 % | 91,8 % | 1,3 / 1,0 / 5 | 0,00 |
| A02 Duruşta anormal titreşim | 1 | 10 | 10 | 0 | 0 | 100,0 % | 100,0 % | 100,0 % | 7,7 / 8,0 / 11 | 0,00 |
| A03 Anormal sefer | 4 | 31 | 26 | 5 | 0 | 100,0 % | 83,9 % | 91,2 % | 22,8 / 17,0 / 41 | 0,00 |
| A04 Hızlı ısınma | 1 | 9 | 9 | 0 | 1 | 90,0 % | 100,0 % | 94,7 % | 113,4 / 84,0 / 311 | 0,34 |
| KESINTI İnternet kesintisi | 3 | 18 | 0 | 18 | 0 | - | 0,0 % | - | - / - / - | 0,00 |

**Veri düzeltmeleri ve iletim notları:**

- Oturum 1-1 analiz dışı bırakıldı.
- Oturum 4 analiz dışı bırakıldı.
- Oturum 5 analiz dışı bırakıldı.
- Oturum 6 analiz dışı bırakıldı.
- Oturum 8 analiz dışı bırakıldı.
- Oturum 13-1 analiz dışı bırakıldı.
- Oturum 1-2, uyaran 1: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 9, uyaran 9: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 9, uyaran 10: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 11, uyaran 1: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 11, uyaran 2: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 11, uyaran 9: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 15, uyaran 2: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 15, uyaran 3: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 15, uyaran 5: uyaransız işaret olarak analiz dışı bırakıldı.
- Oturum 3: bitiş işareti verilmediği için bitiş 23:41:30 alındı (Bitir'e basilmadi; 10. denemeden sonraki tespitler masanin oynamasindan (kullanici dogruladi)).
- Oturum 17: bitiş işareti verilmediği için bitiş 12:35:00 alındı (Bitir isareti kaydedilmedi; kesinti sonu kullanicinin modemi yeniden taktigi zaman (yaklasik 12:35) alindi.).
- Oturum 11: panelden 1 uyaran işareti iptal edilmiş.
- Oturum 1-3, 23:29:31: yanlış alarm sayılmadı (Ilk isaretten once yapilan deneme vurusu (kullanici dogruladi, tepe 12,2 g)).
- Oturum 3, 23:40:59: yanlış alarm sayılmadı (Deneme 5-6 vuruslarinin gec islenmesi: ESP32 dongusu 23:40:35-23:41:00 arasi takildi, biriken satirlar topluca islendi (yerel kayitta 39 s bosluk); denemeler kacirma sayildi).
- Oturum 3, 23:41:00: yanlış alarm sayılmadı (Deneme 5-6 vuruslarinin gec islenmesi (ayni takilma)).
- Uyarı: yanlis_alarm_haric 3@2026-11-17 23:41:01 eşleşmedi.
- Oturum 11, 23:20:36: yanlış alarm sayılmadı (Dislanan uyaran 1in (46 s sefer) sure kaynakli A03 tespiti).
- Oturum 11, 23:21:27: yanlış alarm sayılmadı (Dislanan uyaran 2deki ani bastirmanin A01 tespiti (0,695 g)).
- Oturum 11, 23:21:32: yanlış alarm sayılmadı (Dislanan uyaran 2nin (44 s sefer) sure kaynakli A03 tespiti).
- Oturum 11, 23:25:28: yanlış alarm sayılmadı (Dislanan uyaran 9daki ani bastirmanin A01 tespiti (2,17 g; dogru tespit, yanlis alarm degil)).
- Oturum 11, 23:25:32: yanlış alarm sayılmadı (Dislanan uyaran 9un A03 tespiti (tepe darbeden etkilendi)).
- Oturum 13-2, 23:48:53: yanlış alarm sayılmadı (Ilk uyarandan once yapilan isaretsiz deneme seferi (kullanici dogruladi)).
- Oturum 15, 10:05:36: yanlış alarm sayılmadı (Dislanan uyaran 5in (onceki A04 surerken verilen) isitmasinin tespiti).
- Oturum 15, 10:26:30: yanlış alarm sayılmadı (Isaretsiz deneme: uyaran isareti kaydedilmeden yapilan isitmanin tespiti (paneldeki son isaret 7; 8. deneme 11:53te yeniden yapildi)).
- Not: bulut API kaynağına ulaşılamadı; olaylar diğer kaynaklardan alındı.
- Not: ESP32 yerel kaydı kaynağına ulaşılamadı; olaylar diğer kaynaklardan alındı.

TP: doğru tespit · FN: kaçırılan · FP: yanlış alarm (deney süresince eşleşmeyen anomali başlangıcı).
Precision = TP / (TP + FP) · Recall = TP / (TP + FN) · F1 = 2·P·R / (P + R).
Gecikme: uyaran işaretinden ESP32 kararına (s, ESP32 olay zamanı 1 s çözünürlüklü; dakikalık ortalama kullanan A04 ve sefer sonunda karar veren A03 için doğal gecikme vardır).
Arama pencereleri (s): A01 −5/+15, A02 −5/+120, A03 −5/+300, A04 −5/+600.
