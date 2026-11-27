# Asansör motoru: ilk gün ve eşik kalibrasyonu

Deney 100 (NORMAL), cihaz asansör motoru gövdesinde. Montaj ve deney başlangıcı: 26.11.2026 20:59.

## Zaman çizelgesi

| Zaman | Olay |
|---|---|
| 26.11.2026 20:48:58 | 30 dk otomatik öğrenme başlatıldı (masa modeli yerine) |
| 26.11.2026 20:59:03 | Deney 100 başlatıldı |
| 26.11.2026 21:20 | Öğrenme bitti: μ 0,0111 g, σ 0,002 g |
| 26–27.11.2026 gece | İlk 10 normal seferle profil oluştu: süre sınırı 42 s, tepe sınırı 0,121 g (A01 eşiği 0,243 g) |
| 27.11.2026 12:56 – ~20:55 | Powerbank bitti, cihaz kapalı; bu aralıkta veri yok |
| 27.11.2026 20:58:54 | Eşikler manuel moda alındı (aşağıda) |

## İlk gün (26.11 21:20 – 27.11 12:56, ~15,6 sa, otomatik profil)

| Ölçüt | Değer |
|---|---|
| 10 s kayıt (buluta ulaşan) | 5620, boşluk yok |
| Sefer (tepe ≥ 0,1 g) | 46 |
| Sefer tepesi medyan / %95 / en yüksek normal | 0,24 / 0,29 / 0,29 g |
| Sefer süresi medyan / %95 (10 s çözünürlük) | ~40 / ~80 s |
| Kısa hareket (~0,06 g, ≤ 10 s) | 16 |
| Sıcaklık | 20,1–25,1 °C |
| Yanlış alarm | A01: 32, A03: 5 (≈ 2,4 / saat) |
| Olağan dışı | 27.11 03:23, 0,606 g (A01 + E02), nedeni bilinmiyor |

**Neden:** Motor her normal seferde kalkışta 0,2–0,29 g'lik kısa bir titreşim üretiyor. Yöntem, darbe görülen seferi profile almıyor (masa deneylerinde gerçek darbelerin profili bozmaması için). Bu motorda neredeyse her sefer kalkış darbesi içerdiğinden profil yalnızca yumuşak hareketlerden oluştu, tepe sınırı düşük kaldı (0,121 g) ve normal seferler A01/A03 olarak işaretlendi.

## Kalibrasyon (27.11.2026 20:58:54, MQTT `settings/set`, manuel mod)

| Değer | Önce | Sonra | Gerekçe |
|---|---|---|---|
| Sefer tepe sınırı | 0,121 g | 0,35 g | Normal seferlerin en yükseği 0,29 g + %20 |
| A01 eşiği (2 × tepe) | 0,243 g | 0,70 g | Kalkış darbeleri normal sayılır |
| Sefer süre sınırı | 42 s | 120 s | Süre verisi 10 s çözünürlükte belirsiz; güvenli üst sınır |
| μ / σ | 0,0130 / 0,002 g | 0,012 / 0,002 g | Öğrenilen duruş değeri |

Değerler tek günün 46 seferinden belirlendi; sonraki günler bu ayarın doğrulamasıdır. Kalıcı çözüm (yazılım): kalkış anını sefer profiline dahil etmek; cihaz geri geldiğinde eklenecek.
