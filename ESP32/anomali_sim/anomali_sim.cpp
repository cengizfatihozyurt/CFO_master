/*
 * anomali.h icin bilgisayarda calisan simulator (ESP32'deki kodun aynisi)
 *
 * Kullanim:
 *   anomali_sim kayit.csv [cikti.csv]   gercek 1 sn kayitlarini oynatir (t,v,temp)
 *   anomali_sim --senaryo [cikti.csv]   gercek durus gurultusu + bilinen olaylar
 *                                        (sefer, darbe, uzun titresim, isinma) ile test
 *     GERCEK_VERI=<1 sn kayit dosyasi>  arka plan gurultusunun alinacagi dosya (zorunlu)
 *     TOHUM=<sayi>                      rastgele senaryo tohumu (varsayilan 42)
 *   Her iki kullanimda:
 *     OGRENME_DK=<dk>                   otomatik ogrenme suresi (varsayilan 10)
 *     MANUEL=mu,sigma,sefer_s,tepe_g    ogrenme yerine elle girilen degerler
 *
 * cikti.csv: her saniye t,v,temp,mod,z,bayraklar (grafik/analiz icin)
 *
 * Derleme (CodeBlocks MinGW):
 *   g++ -O2 -std=c++17 -o anomali_sim.exe anomali_sim.cpp
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../esp32_sensor_rx/anomali.h"

static const char *CODES[AN_COUNT] = {"A01", "A02", "A03", "A04"};
static const char *NAMES[AN_COUNT] = {"Ani darbe", "Durusta anormal titresim", "Anormal sefer", "Hizli isinma"};
static const char *MODES[] = {"ogreniyor", "durus", "hareket"};

struct Sample {
  long  t;
  float v;
  bool  tempValid;
  float temp;
  int   label;   // senaryo: beklenen anomali (AN_*), -1 = yok
};

struct Event {
  long t;
  int  id;
  bool on;
  float value, limit;
};

static std::vector<Sample> readCsv(const char *path)
{
  std::vector<Sample> s;
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "Dosya acilamadi: %s\n", path);
    exit(1);
  }
  char line[256];
  fgets(line, sizeof(line), f);   // baslik
  while (fgets(line, sizeof(line), f)) {
    Sample x = {};
    char tempStr[32] = "";
    if (sscanf(line, "%ld,%f,%31[^\r\n]", &x.t, &x.v, tempStr) < 2) continue;
    x.tempValid = tempStr[0] != '\0';
    x.temp = x.tempValid ? (float)atof(tempStr) : 0;
    x.label = -1;
    s.push_back(x);
  }
  fclose(f);
  return s;
}

// Senaryo: gercek durus olcumlerinden rastgele secilerek (bootstrap) olusturulan
// arka plan uzerine bilinen olaylar eklenir.
static std::vector<Sample> buildScenario(const std::vector<Sample> &real, std::vector<std::string> &plan)
{
  std::vector<float> idle;
  for (const Sample &x : real) {
    if (x.v < 0.016f) idle.push_back(x.v);   // gercek verinin %95'i bu seviyenin altinda
  }
  const char *seed = getenv("TOHUM");   // farkli rastgele senaryolar icin
  std::mt19937 rng(seed ? (unsigned)atoi(seed) : 42u);
  std::uniform_int_distribution<size_t> pick(0, idle.size() - 1);
  std::normal_distribution<float> noise(0.0f, 1.0f);

  std::vector<Sample> s;
  long t = 1792900000;
  float temp = 21.0f;
  auto push = [&](float v, int label) {
    s.push_back({t++, v, true, temp + noise(rng) * 0.03f, label});
  };
  auto idleFor = [&](int sec) { for (int i = 0; i < sec; i++) push(idle[pick(rng)], -1); };
  auto ride = [&](int sec, float level, int label) {
    for (int i = 0; i < sec; i++) {
      float ramp = (i < 2 || i >= sec - 2) ? 0.6f : 1.0f;          // kalkis/durus
      push(idle[pick(rng)] + level * ramp * (1.0f + 0.15f * noise(rng)), label);
    }
  };
  auto note = [&](const char *what) {
    char b[128];
    snprintf(b, sizeof(b), "t=%6ld s  %s", t - s.front().t, what);
    plan.push_back(b);
  };

  s.reserve(20000);
  s.push_back({t++, idle[pick(rng)], true, temp, -1});
  note("ogrenme (sadece durus)");
  const char *learnMin = getenv("OGRENME_DK");
  idleFor((learnMin ? atoi(learnMin) * 60 : ANOM_LEARN_S) + 60);

  note("20 normal sefer (12-20 s, ~0.04 g)");
  std::uniform_int_distribution<int> rideLen(12, 20), gap(60, 180);
  for (int i = 0; i < 20; i++) {
    ride(rideLen(rng), 0.040f, -1);
    idleFor(gap(rng));
    if (i == 4) {
      // Canli testte goruldu (2026-10-25): karta vurus 0.76/0.12/0.83 g uretti; 3 s
      // "hareket" sayildi ve sefer profiline girip tepe sinirini sisirdi
      note("BEKLENEN A01: cift vurus (canli testteki 0.76 / 0.12 / 0.83 g)");
      push(0.764f, AN_SHOCK);
      push(0.120f, AN_SHOCK);
      push(0.832f, AN_SHOCK);
      idleFor(gap(rng));
    }
  }

  note("BEKLENEN A01: ani darbe (0.35 g, 1 s)");
  push(0.35f, AN_SHOCK);
  idleFor(120);

  note("BEKLENEN A02: durusta 60 s hafif titresim (+0.009 g, ~4.5 sigma)");
  for (int i = 0; i < 60; i++) push(idle[pick(rng)] + 0.009f, AN_IDLE);
  idleFor(180);

  note("BEKLENEN A03: 200 s suren sefer (normal 12-20 s)");
  ride(200, 0.040f, AN_RIDE);
  idleFor(180);

  note("BEKLENEN A03: normal surede ama 3 kat titresimli sefer");
  ride(15, 0.120f, AN_RIDE);
  idleFor(180);

  note("5 normal sefer daha (yanlis alarm olmamali)");
  for (int i = 0; i < 5; i++) {
    ride(rideLen(rng), 0.040f, -1);
    idleFor(gap(rng));
  }

  note("BEKLENEN A04: 10 dk boyunca 1 C/dk isinma");
  for (int i = 0; i < 600; i++) {
    temp += 1.0f / 60.0f;
    push(idle[pick(rng)], AN_TEMP);
  }
  idleFor(900);
  note("son");
  return s;
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    fprintf(stderr, "Kullanim: %s kayit.csv|--senaryo [cikti.csv]\n", argv[0]);
    return 1;
  }
  bool scenario = strcmp(argv[1], "--senaryo") == 0;
  std::vector<std::string> plan;
  std::vector<Sample> data;
  if (scenario) {
    const char *src = getenv("GERCEK_VERI");
    if (!src) {
      fprintf(stderr, "Senaryo icin GERCEK_VERI ortam degiskenine gercek 1 sn kayit dosyasini verin\n");
      return 1;
    }
    data = buildScenario(readCsv(src), plan);
  } else {
    data = readCsv(argv[1]);
  }
  FILE *out = argc > 2 ? fopen(argv[2], "w") : nullptr;
  if (out) fprintf(out, "t,v,temp,mod,z,bayraklar,beklenen\n");

  AnomalyDetector det;
  if (const char *m = getenv("OGRENME_DK")) det.startAuto((uint32_t)atoi(m) * 60);
  if (const char *m = getenv("MANUEL")) {
    float mu, sig, peak;
    int rideMax;
    if (sscanf(m, "%f,%f,%d,%f", &mu, &sig, &rideMax, &peak) != 4) {
      fprintf(stderr, "MANUEL bicimi: mu,sigma,en_uzun_sefer_s,tepe_g\n");
      return 1;
    }
    det.setManual(mu, sig, (uint32_t)rideMax, peak);
    printf("Manuel mod: mu=%.4f sigma=%.4f sefer<=%d s tepe<=%.3f g\n", mu, sig, rideMax, peak);
  }
  bool prev[AN_COUNT] = {};
  std::vector<Event> events;
  long learnedAt = -1;
  uint32_t rides = 0;

  for (const Sample &x : data) {
    det.update(x.v, x.tempValid, x.temp);
    if (learnedAt < 0 && det.p.learned) learnedAt = x.t;
    int flags = 0;
    for (int i = 0; i < AN_COUNT; i++) {
      if (det.active[i]) flags |= 1 << i;
      if (det.active[i] != prev[i]) {
        events.push_back({x.t, i, det.active[i], det.value[i], det.limit[i]});
        prev[i] = det.active[i];
      }
    }
    if (out) {
      fprintf(out, "%ld,%.4f,%.2f,%d,%.2f,%d,%d\n", x.t, x.v, x.temp, det.mode, det.z, flags, x.label);
    }
  }
  rides = det.p.nRides;
  if (out) fclose(out);

  long t0 = data.empty() ? 0 : data.front().t;
  printf("Olcum: %zu s (%.1f saat)\n", data.size(), data.size() / 3600.0);
  if (learnedAt >= 0) {
    printf("Ogrenme bitti: t=%ld s, durus mu=%.4f g, sigma=%.4f g (esikler: hareket %.4f g, durus anomalisi %.4f g)\n",
           learnedAt - t0, det.mu(), det.sigma(), det.mu() + ANOM_Z_MOVE * det.sigma(), det.mu() + ANOM_Z_IDLE * det.sigma());
  } else {
    printf("Ogrenme bitmedi (%.0f%%)\n", det.learnProgress() * 100);
  }
  printf("Profile giren normal sefer: %u", rides);
  if (rides > 1) {
    printf(" (sure ort %.1f s, tepe ort %.3f g)", det.p.durMean, det.p.peakMean);
  }
  printf("\nSon durum: %s\n\n", MODES[det.mode]);

  if (scenario) {
    printf("Senaryo:\n");
    for (const std::string &l : plan) printf("  %s\n", l.c_str());
    printf("\n");
  }

  printf("Olaylar (%zu):\n", events.size());
  for (const Event &e : events) {
    printf("  t=%6ld s  %s %-7s %-26s deger=%.3f esik=%.3f\n", e.t - t0, CODES[e.id], e.on ? "BASLADI" : "BITTI",
           NAMES[e.id], e.value, e.limit);
  }

  if (scenario) {
    // Degerlendirme: her beklenen olay grubu yakalandi mi, gecikme ne kadar,
    // beklenmeyen yerde alarm var mi
    printf("\nDegerlendirme:\n");
    int tp = 0, fn = 0, fp = 0;
    size_t i = 0;
    while (i < data.size()) {
      int lab = data[i].label;
      size_t j = i;
      while (j < data.size() && data[j].label == lab) j++;
      if (lab >= 0) {
        long start = data[i].t, end = data[j - 1].t + 15;   // olay bittikten sonra 15 s tolerans
        long hit = -1;
        for (const Event &e : events) {
          if (e.on && e.id == lab && e.t >= start && e.t <= end) { hit = e.t; break; }
        }
        if (hit >= 0) {
          tp++;
          printf("  YAKALANDI  %s %-26s gecikme %ld s\n", CODES[lab], NAMES[lab], hit - start);
        } else {
          fn++;
          printf("  KACIRILDI  %s %s\n", CODES[lab], NAMES[lab]);
        }
      }
      i = j;
    }
    for (const Event &e : events) {
      if (!e.on) continue;
      bool expected = false;
      for (const Sample &x : data) {
        if (x.label == e.id && e.t >= x.t && e.t <= x.t + 15) { expected = true; break; }
      }
      if (!expected) {
        fp++;
        printf("  YANLIS ALARM t=%ld s %s %s\n", e.t - t0, CODES[e.id], NAMES[e.id]);
      }
    }
    printf("\nSonuc: %d/%d olay yakalandi, %d kacirildi, %d yanlis alarm\n", tp, tp + fn, fn, fp);
  }
  return 0;
}
