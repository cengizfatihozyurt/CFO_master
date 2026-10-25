/*
 * Uc birimde (ESP32) basit anomali tespiti
 *
 * Saf C++ (Arduino bagimliligi yok): ayni kod bilgisayarda gercek kayitlarla
 * da calistirilabilir (ESP32/anomali_sim).
 *
 * Girdi: STM32'den saniyede bir gelen toplam titresim RMS'i v (g) ve sicaklik.
 *
 * 1. Normal durum: iki yol
 *    a) Otomatik: kullanicinin sectigi sure (varsayilan ANOM_LEARN_S) boyunca durus
 *       (bosta) titresiminin ortalamasi (mu) ve standart sapmasi (sigma) hesaplanir.
 *       Ogrenme bitince mu/sigma EWMA ile yavasca guncellenmeye devam eder; yalnizca
 *       durusta ve olcum normalken (z < ANOM_Z_UPDATE), boylece anomaliler "normal"i bozmaz.
 *    b) Manuel: mu, sigma, en uzun sefer suresi ve sefer tepe siniri kullanici
 *       tarafindan girilir; sistem bunlari kendiliginden degistirmez (EWMA ve sefer
 *       profili ogrenmesi kapali).
 *
 * 2. Durum (durus / hareket)
 *    z = (v - mu) / sigma
 *    z > ANOM_Z_MOVE en az ANOM_MOVE_ON_S sn  -> HAREKET (sefer basladi)
 *    z < ANOM_Z_STOP en az ANOM_MOVE_OFF_S sn -> DURUS   (sefer bitti)
 *    Asansor hareket ederken titresim dogal olarak artar; durum ayrimi her
 *    seferin alarm vermesini onler. Her normal seferin suresi ve tepe
 *    titresimi ayrica ogrenilir (sefer profili).
 *
 * 3. Anomaliler
 *    A01 Ani darbe        : v > mu + ANOM_Z_SHOCK*sigma ve v > ANOM_SHOCK_MIN_G
 *    A02 Durusta titresim : durusta son ANOM_IDLE_HOLD_S sn'nin medyani > mu + ANOM_Z_IDLE*sigma
 *                           (daha yuksek surekli titresim "hareket" sayilir; uzun surerse A03)
 *    A03 Anormal sefer    : sefer / titresim cok uzun surdu ya da tepe titresimi
 *                           ogrenilen sefer profilinin disinda (ort + ANOM_Z_RIDE*std)
 *    A04 Hizli isinma     : son ANOM_TEMP_WIN_MIN dakikadaki artis hizi > ANOM_TEMP_RATE C/dk
 */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ANOM_LEARN_S        600       // varsayilan otomatik ogrenme suresi (s)
#define ANOM_LEARN_MIN_S    60        // kullanicinin secebilecegi en kisa / en uzun ogrenme
#define ANOM_LEARN_MAX_S    14400
#define ANOM_SIGMA_MIN      0.002f    // sigma alt siniri (g); sensor gurultusu cok dusukken asiri hassasiyeti onler
#define ANOM_EWMA_ALPHA     (1.0f / 1800.0f)  // ogrenme sonrasi guncelleme (~30 dk zaman sabiti)
#define ANOM_Z_UPDATE       3.0f      // bunun altindaki durus olcumleri normali gunceller

#define ANOM_Z_MOVE         6.0f
#define ANOM_MOVE_ON_S      2
#define ANOM_Z_STOP         3.0f
#define ANOM_MOVE_OFF_S     3

#define ANOM_Z_SHOCK        30.0f
#define ANOM_SHOCK_MIN_G    0.150f
#define ANOM_SHOCK_RIDE_X   2.0f      // profil varsa darbe esigi >= 2 x sefer tepe siniri
#define ANOM_SHOCK_RIDE_S   8         // darbeli ve bundan kisa "sefer" yalnizca darbedir
#define ANOM_Z_IDLE         4.0f
#define ANOM_IDLE_HOLD_S    10

#define ANOM_RIDE_MIN_S     3         // bundan kisa "seferler" darbe/temas sayilir, profile girmez
#define ANOM_RIDE_LEARN     10        // sefer profili bu kadar seferden sonra kullanilir
#define ANOM_RIDE_MAX_S     120       // profil olusmadan once de gecerli ust sinir
#define ANOM_Z_RIDE         4.0f
#define ANOM_RIDE_PEAK_MIN  0.010f    // tepe std alt siniri (g)
#define ANOM_RIDE_DUR_MIN   5.0f      // sure std alt siniri (s)

#define ANOM_TEMP_WIN_MIN   5
#define ANOM_TEMP_RATE      0.5f      // C/dk

#define ANOM_STATE_VERSION  2

enum AnomId { AN_SHOCK, AN_IDLE, AN_RIDE, AN_TEMP, AN_COUNT };

enum AnomMode : uint8_t { AM_LEARNING = 0, AM_IDLE = 1, AM_MOVING = 2 };

// Kalici durum (NVS'de saklanir; elektrik kesilince yeniden ogrenmeye gerek kalmaz)
struct AnomPersist {
  uint8_t  version;
  uint8_t  learned;
  uint8_t  manual;      // 1: degerler kullanici tarafindan girildi
  uint8_t  reserved;
  uint32_t nLearn;      // ogrenmede kullanilan olcum sayisi
  float    mu, var;     // durus titresimi (g, g^2)
  uint32_t nRides;      // profile giren normal sefer sayisi
  float    durMean, durM2;    // sefer suresi (s) - Welford
  float    peakMean, peakM2;  // sefer tepe titresimi (g) - Welford
  // --- surum 2 ---
  uint16_t learnS;      // otomatik ogrenme suresi (s)
  uint16_t manRideMaxS; // manuel: en uzun normal sefer (s)
  float    manPeakG;    // manuel: sefer tepe titresimi siniri (g)
};
#define ANOM_PERSIST_V1_SIZE  offsetof(AnomPersist, learnS)

class AnomalyDetector {
public:
  AnomPersist p;

  // Son degerlendirmenin sonucu
  uint8_t mode = AM_LEARNING;
  float   z = 0;
  bool    active[AN_COUNT] = {};
  float   value[AN_COUNT] = {};   // olay kaydi icin olculen deger
  float   limit[AN_COUNT] = {};   // olay kaydi icin esik

  AnomalyDetector() { reset(); }

  // Otomatik ogrenmeyi bastan baslat (sure: saniye, sinirlar icine cekilir)
  void startAuto(uint32_t learnSeconds)
  {
    if (learnSeconds < ANOM_LEARN_MIN_S) learnSeconds = ANOM_LEARN_MIN_S;
    if (learnSeconds > ANOM_LEARN_MAX_S) learnSeconds = ANOM_LEARN_MAX_S;
    reset();
    p.learnS = (uint16_t)learnSeconds;
  }

  // Normal durum degerlerini elle gir; ogrenme yapilmaz
  void setManual(float mu, float sigma, uint32_t rideMaxS, float ridePeakG)
  {
    uint16_t keepLearnS = p.learnS;
    reset();
    p.learnS = keepLearnS;
    p.manual = 1;
    p.learned = 1;
    p.mu = mu;
    p.var = sigma * sigma;
    p.manRideMaxS = (uint16_t)rideMaxS;
    p.manPeakG = ridePeakG;
    mode = AM_IDLE;
  }

  void reset()
  {
    memset(&p, 0, sizeof(p));
    p.version = ANOM_STATE_VERSION;
    p.learnS = ANOM_LEARN_S;
    mode = AM_LEARNING;
    z = 0;
    memset(active, 0, sizeof(active));
    rideS = rideOnCnt = rideOffCnt = idleCnt = 0;
    sinceShock = UINT32_MAX;
    ridePeak = 0;
    rideFlagged = false;
    minuteSum = 0;
    minuteN = 0;
    tempHistN = 0;
  }

  // Kayitli durumu yukle (n: okunan bayt). Surum 1 kayitlari otomatik modla tasinir;
  // taninmayan kayitta false doner (sifirdan ogrenilir).
  bool load(const void *data, size_t n)
  {
    AnomPersist saved;
    memset(&saved, 0, sizeof(saved));
    memcpy(&saved, data, n < sizeof(saved) ? n : sizeof(saved));
    if (saved.version == 1 && n == ANOM_PERSIST_V1_SIZE) {
      saved.version = ANOM_STATE_VERSION;
      saved.manual = 0;
      saved.reserved = 0;
      saved.learnS = ANOM_LEARN_S;
    } else if (saved.version != ANOM_STATE_VERSION || n != sizeof(saved)) {
      return false;
    }
    reset();
    p = saved;
    if (p.learnS < ANOM_LEARN_MIN_S) p.learnS = ANOM_LEARN_S;
    mode = p.learned ? AM_IDLE : AM_LEARNING;
    return true;
  }

  bool  isManual() const { return p.manual != 0; }
  float mu() const    { return p.mu; }
  float sigma() const
  {
    // ogrenme sirasinda p.var Welford toplami (M2), sonrasinda varyanstir
    float var = p.learned ? p.var : (p.nLearn > 1 ? p.var / (p.nLearn - 1) : 0);
    float s = sqrtf(var > 0 ? var : 0);
    return s < ANOM_SIGMA_MIN ? ANOM_SIGMA_MIN : s;
  }
  float learnProgress() const { return p.learned ? 1.0f : (float)p.nLearn / p.learnS; }

  // Gecerli sefer sinirlari (otomatik modda profil olusmadan once varsayilan)
  bool  hasRideProfile() const { return p.manual || p.nRides >= ANOM_RIDE_LEARN; }
  float rideDurLimitNow() const { return rideDurLimit(); }
  float ridePeakLimitNow() const { return hasRideProfile() ? ridePeakLimit() : 0.0f; }
  uint32_t rideSeconds() const { return mode == AM_MOVING ? rideS : 0; }

  // Saniyede bir cagrilir
  void update(float v, bool tempValid, float tempC)
  {
    updateTemp(tempValid, tempC);

    if (!p.learned) {
      learn(v);
      return;
    }

    float s = sigma();
    z = (v - p.mu) / s;

    // A01: ani darbe (her durumda). Sefer profili varsa esik, en titresimli
    // normal seferin de belirgin ustunde olmali (anormal sefer A03'tur, darbe degil)
    float shockThr = p.mu + ANOM_Z_SHOCK * s;
    if (shockThr < ANOM_SHOCK_MIN_G) shockThr = ANOM_SHOCK_MIN_G;
    if (hasRideProfile() && shockThr < ANOM_SHOCK_RIDE_X * ridePeakLimit()) {
      shockThr = ANOM_SHOCK_RIDE_X * ridePeakLimit();
    }
    if (!active[AN_SHOCK] && v > shockThr) {
      set(AN_SHOCK, true, v, shockThr);
    } else if (active[AN_SHOCK] && v < shockThr * 0.7f) {
      set(AN_SHOCK, false, v, shockThr);
    }
    if (active[AN_SHOCK]) {
      sinceShock = 0;
    } else if (sinceShock < UINT32_MAX) {
      sinceShock++;
    }

    if (mode == AM_IDLE) {
      updateIdle(v, s);
    } else {
      updateMoving(v, s);
    }
  }

private:
  uint32_t rideS, rideOnCnt, rideOffCnt, idleCnt;
  uint32_t sinceShock;   // son darbeden bu yana gecen olcum sayisi
  float    idleWin[ANOM_IDLE_HOLD_S];
  float    ridePeak;
  bool     rideFlagged;

  // Sicaklik: dakikalik ortalamalar halka tamponu
  float    minuteSum;
  uint32_t minuteN;
  float    tempHist[ANOM_TEMP_WIN_MIN + 1];
  uint32_t tempHistN;

  void set(int id, bool on, float val, float lim)
  {
    active[id] = on;
    value[id] = val;
    limit[id] = lim;
  }

  // Ogrenme: ilk 30 olcumden sonra asiri degerler (temas, darbe) ortalamaya katilmaz
  void learn(float v)
  {
    z = 0;
    if (p.nLearn >= 30 && v > p.mu + 5.0f * sigma()) return;
    p.nLearn++;
    float d = v - p.mu;
    p.mu += d / p.nLearn;
    p.var += d * (v - p.mu);           // Welford M2
    if (p.nLearn >= p.learnS) {
      p.var /= (p.nLearn - 1);         // M2 -> varyans
      p.learned = 1;
      mode = AM_IDLE;
    }
  }

  void updateIdle(float v, float s)
  {
    // Normali yavasca guncelle (sadece normal durus olcumleriyle; manuel modda asla)
    if (!p.manual && z < ANOM_Z_UPDATE) {
      float d = v - p.mu;
      p.mu += ANOM_EWMA_ALPHA * d;
      p.var = (1.0f - ANOM_EWMA_ALPHA) * (p.var + ANOM_EWMA_ALPHA * d * d);
    }

    // A02: durusta son ANOM_IDLE_HOLD_S saniyenin medyani yuksek. Tek tek olcumler
    // gurultulu; kalici kaymayi pencere medyani yakalar, tekil darbeler etkilemez.
    idleWin[idleCnt % ANOM_IDLE_HOLD_S] = v;
    idleCnt++;
    float idleThr = p.mu + ANOM_Z_IDLE * s;
    if (idleCnt >= ANOM_IDLE_HOLD_S) {
      float m = windowMedian();
      if (!active[AN_IDLE] && m > idleThr) {
        set(AN_IDLE, true, m, idleThr);
      } else if (active[AN_IDLE] && m < p.mu + ANOM_Z_STOP * s) {
        set(AN_IDLE, false, m, idleThr);
      }
    }

    // Onceki seferden kalan A03 (tepe asimi) bir olcum sonra biter
    if (active[AN_RIDE]) set(AN_RIDE, false, value[AN_RIDE], limit[AN_RIDE]);

    // Sefer baslangici
    if (z > ANOM_Z_MOVE) {
      if (++rideOnCnt >= ANOM_MOVE_ON_S) {
        mode = AM_MOVING;
        rideS = rideOnCnt;
        ridePeak = v;
        rideOffCnt = 0;
        rideFlagged = false;
        if (active[AN_IDLE]) set(AN_IDLE, false, v, idleThr);
        idleCnt = 0;
      }
    } else {
      rideOnCnt = 0;
    }
  }

  float windowMedian() const
  {
    float w[ANOM_IDLE_HOLD_S];
    memcpy(w, idleWin, sizeof(w));
    for (int i = 1; i < ANOM_IDLE_HOLD_S; i++) {   // 10 eleman: eklemeli siralama yeterli
      float x = w[i];
      int j = i - 1;
      while (j >= 0 && w[j] > x) { w[j + 1] = w[j]; j--; }
      w[j + 1] = x;
    }
    return 0.5f * (w[(ANOM_IDLE_HOLD_S - 1) / 2] + w[ANOM_IDLE_HOLD_S / 2]);
  }

  float rideDurLimit() const
  {
    if (p.manual) return (float)p.manRideMaxS;
    if (p.nRides < ANOM_RIDE_LEARN) return (float)ANOM_RIDE_MAX_S;
    float sd = sqrtf(p.durM2 / (p.nRides - 1));
    if (sd < ANOM_RIDE_DUR_MIN) sd = ANOM_RIDE_DUR_MIN;
    float lim = p.durMean + ANOM_Z_RIDE * sd;
    return lim < (float)ANOM_RIDE_MAX_S ? lim : (float)ANOM_RIDE_MAX_S;
  }

  float ridePeakLimit() const
  {
    if (p.manual) return p.manPeakG;
    float sd = sqrtf(p.peakM2 / (p.nRides - 1));
    if (sd < ANOM_RIDE_PEAK_MIN) sd = ANOM_RIDE_PEAK_MIN;
    return p.peakMean + ANOM_Z_RIDE * sd;
  }

  void updateMoving(float v, float s)
  {
    (void)s;
    rideS++;
    if (v > ridePeak) ridePeak = v;

    // A03: sefer beklenenden uzun suruyor
    float durLim = rideDurLimit();
    if (!active[AN_RIDE] && rideS > durLim) {
      set(AN_RIDE, true, (float)rideS, durLim);
      rideFlagged = true;
    }
    if (active[AN_RIDE]) value[AN_RIDE] = (float)rideS;   // olay bitince gercek sure yazilsin

    // Sefer bitisi
    if (z < ANOM_Z_STOP) {
      if (++rideOffCnt >= ANOM_MOVE_OFF_S) endRide(v);
    } else {
      rideOffCnt = 0;
    }
  }

  void endRide(float v)
  {
    (void)v;
    mode = AM_IDLE;
    rideOnCnt = 0;
    uint32_t dur = rideS - rideOffCnt;   // son durus saniyeleri sefere dahil degil
    float dur_f = (float)dur;
    // Sefer sirasinda ya da hemen oncesinde darbe (A01) olduysa sefer profile girmez;
    // girerse tepe sinirini sisirip sonraki anormal seferleri gizler (canli test, 2026-10-25).
    // Kisa "sefer" yalnizca darbenin titresimidir: tepe kontrolu de yapilmaz (darbe zaten A01).
    bool shockInRide = sinceShock <= rideS + ANOM_MOVE_ON_S;
    bool shockOnly = shockInRide && dur <= ANOM_SHOCK_RIDE_S;

    if (active[AN_RIDE]) {
      set(AN_RIDE, false, dur_f, limit[AN_RIDE]);
    } else if (!shockOnly && dur >= ANOM_RIDE_MIN_S && hasRideProfile() && ridePeak > ridePeakLimit()) {
      // A03: tepe titresimi profilin disinda (bir sonraki durus olcumunde biter)
      set(AN_RIDE, true, ridePeak, ridePeakLimit());
      rideFlagged = true;
    }

    // Normal seferler profili gunceller (manuel modda profil sabittir)
    if (!p.manual && dur >= ANOM_RIDE_MIN_S && !rideFlagged && !shockInRide) {
      p.nRides++;
      float d = dur_f - p.durMean;
      p.durMean += d / p.nRides;
      p.durM2 += d * (dur_f - p.durMean);
      d = ridePeak - p.peakMean;
      p.peakMean += d / p.nRides;
      p.peakM2 += d * (ridePeak - p.peakMean);
    }
    rideS = 0;
    ridePeak = 0;
    rideOffCnt = 0;
  }

  // A04: dakikalik ortalamalarla isinma hizi
  void updateTemp(bool valid, float t)
  {
    if (!valid) return;
    minuteSum += t;
    if (++minuteN < 60) return;
    float avg = minuteSum / minuteN;
    minuteSum = 0;
    minuteN = 0;

    if (tempHistN <= ANOM_TEMP_WIN_MIN) {
      tempHist[tempHistN++] = avg;
    } else {
      memmove(tempHist, tempHist + 1, ANOM_TEMP_WIN_MIN * sizeof(float));
      tempHist[ANOM_TEMP_WIN_MIN] = avg;
    }
    if (tempHistN <= ANOM_TEMP_WIN_MIN) return;

    float rate = (tempHist[ANOM_TEMP_WIN_MIN] - tempHist[0]) / ANOM_TEMP_WIN_MIN;
    if (!active[AN_TEMP] && rate > ANOM_TEMP_RATE) {
      set(AN_TEMP, true, rate, ANOM_TEMP_RATE);
    } else if (active[AN_TEMP] && rate < ANOM_TEMP_RATE * 0.5f) {
      set(AN_TEMP, false, rate, ANOM_TEMP_RATE);
    }
  }
};
