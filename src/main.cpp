/*
 * ============================================================
 *   КОРОНА  —  ARGB-лента как лампа в Home Assistant (Zigbee)
 * ============================================================
 *
 *   Плата: ESP32-H2 SuperMini
 *   Роль:  Zigbee Router (постоянное питание, ретранслирует чужие пакеты)
 *
 * ------------------------------------------------------------
 *   ПОДКЛЮЧЕНИЕ  (схема — korona-esp32h2-schema.svg)
 * ------------------------------------------------------------
 *
 *     GPIO0 -- зелёный провод ленты (DATA), 3.3 В напрямую, без резистора
 *     GPIO14  -- выход ИК-приёмника (VS1838 и т.п.); его питание — от 3V3, не от 5 В
 *     GPIO5 -- кнопка 1 (вкл/выкл)      \
 *     GPIO4 -- кнопка 2 (цвет / режим)   > вторым контактом каждой кнопки на GND
 *     GPIO1 -- кнопка 3 (яркость)       /
 *     Красный провод ленты — +12 В (или +5 В, если лента 5-вольтовая)
 *     Белый провод ленты   — GND, тянуть вместе с DATA с одного разъёма
 *
 *     Плата запитана от 5 В (DC-DC понижайка). GND платы, ленты и БП общие.
 *
 * ------------------------------------------------------------
 *   ЧТО ПОЯВИТСЯ В HOME ASSISTANT  (всего две сущности)
 * ------------------------------------------------------------
 *   Свет      — лампа: вкл/выкл, цвет, яркость          (endpoint 10)
 *   Эффект    — выбор из списка: Свой цвет, Радуга,     (endpoint 11)
 *               Дыхание, Перелив, Огонёк, Мерцание
 *
 *   Цвет и яркость лампы запоминаются и переживают пропадание питания.
 *
 * ------------------------------------------------------------
 *   МОНИТОР ПОРТА (115200) — клавиши, '?' — справка
 * ------------------------------------------------------------
 * ============================================================
 */

#ifndef ZIGBEE_MODE_ZCZR
#error "Нужен флаг -DZIGBEE_MODE_ZCZR в platformio.ini"
#endif

#include <Arduino.h>
#include "Zigbee.h"
#include <Preferences.h>
#include "driver/gpio.h"

// ============================================================
//   НАСТРОЙКИ
// ============================================================

#define PIN_DATA           0     // GPIO0
#define PIN_IR            14     // выход ИК-приёмника
#define PIN_BTN_PWR        5     // кнопка 1: вкл/выкл (удержание 5 с — сброс сети Zigbee)
#define PIN_BTN_COL        4     // кнопка 2: цвет (удержание 3 с — режимы)
#define PIN_BTN_BRI        1     // кнопка 3: яркость (удержание 2 с — темнее)

// Сколько адресов (не физических светодиодов!) на ленте.
// У 12 В ARGB-ленты один адрес обычно = 3 физических светодиода.
// Длина задаётся с клавиатуры в мониторе порта (клавиша L) и запоминается.
// Здесь только значение по умолчанию и потолок буфера.
#define DEFAULT_LEDS     100
#define MAX_LEDS         500

#define MIN_LEVEL         10     // ниже этого яркость считаем ошибкой (лента не светит)
#define MAX_BRIGHT       255     // потолок яркости, 255 = без ограничения

#define ZB_TX_POWER_DBM   20     // мощность радио, максимум
#define HEARTBEAT      20000     // мс — «я жива» для Z2M

// Порядок цветов у этой ленты BRG (проверено: байты идут синий, красный, зелёный).
// Если цвета снова перепутаны — поменяй порядок в ledsShow(): строку с col[3].

enum { EP_LIGHT = 10, EP_EFFECT = 11 };
#define N_EFFECTS 10
#define EFF_STEP  25          // номер эффекта кодируется уровнем яркости: 25, 50, ... 250

// ============================================================
//   ZIGBEE
// ============================================================

ZigbeeColorDimmableLight zbLight(EP_LIGHT);
ZigbeeDimmableLight      zbEffect(EP_EFFECT);   // уровень = номер эффекта (40, 80, ...)
Preferences prefs;

// ============================================================
//   СОСТОЯНИЕ
// ============================================================
// Колбэки Zigbee только пишут эти переменные; всю работу делает loop().

static volatile int numLeds = DEFAULT_LEDS;   // реальная длина ленты (адресов)
static uint8_t speedIdx = 4;                    // скорость эффектов 1..7 (только в плате, в HA не уходит)
static const uint8_t ANIM_MS[8] = { 25, 60, 45, 35, 25, 18, 12, 8 };   // период кадра эффекта по ступеням
static volatile bool     lightOn = false;
static volatile uint8_t  effect  = 0;                 // 0 = свой цвет, 1..10 = эффект
static volatile uint8_t  lr = 255, lg = 160, lb = 60, lLevel = 200;
static volatile bool     dirty = true, needSave = false;
static volatile uint32_t tChanged = 0;
static volatile bool     selfReport = false;          // это мы сами докладываем состояние

static void changed() { dirty = true; needSave = true; tChanged = millis(); }

// Библиотека Zigbee переводит цвет из HA (xy) в RGB с ошибкой: лишнее деление на 100,
// цвет приходит в 5-10 раз тусклее. Чиним: снимаем гамму, которую навесила библиотека,
// и растягиваем так, чтобы самый яркий канал был 255. Оттенок сохраняется,
// яркость задаёт только ползунок яркости.
static float unGamma(uint8_t c) {
  float v = c / 255.0f;
  return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}
static void fixColor(uint8_t &r, uint8_t &g, uint8_t &b) {
  float fr = unGamma(r), fg = unGamma(g), fb = unGamma(b);
  float mx = fmaxf(fr, fmaxf(fg, fb));
  if (mx <= 0) { r = g = b = 255; return; }           // пришёл чёрный — пусть будет белый
  r = (uint8_t)(fr / mx * 255 + 0.5f);
  g = (uint8_t)(fg / mx * 255 + 0.5f);
  b = (uint8_t)(fb / mx * 255 + 0.5f);
}

static void onLight(bool s, uint8_t r, uint8_t g, uint8_t b, uint8_t lvl) {
  if (selfReport) return;
  fixColor(r, g, b);
  lightOn = s; lr = r; lg = g; lb = b;
  if (lvl >= MIN_LEVEL) lLevel = lvl;                  // яркость 0 от HA не принимаем: лента бы просто не светила
  changed();
}

static uint8_t levelToEffect(uint8_t lvl) {            // 25,50,...,250 -> 1..10
  int e = (lvl + EFF_STEP / 2) / EFF_STEP;
  return e < 1 ? 1 : (e > N_EFFECTS ? N_EFFECTS : e);
}

static void onEffect(bool s, uint8_t lvl) {
  if (selfReport) return;
  effect = s ? levelToEffect(lvl) : 0;
  if (effect) lightOn = true;                          // выбрали эффект — лампа включается
  changed();
}

// Явный отчёт об атрибуте прямо координатору (адрес 0x0000, эндпоинт 1). Не зависит от
// того, настроил ли Z2M привязку и репортинг: Z2M принимает такие отчёты всегда.
static void sendAttrReport(uint8_t ep, uint16_t cluster, uint16_t attr) {
  esp_zb_zcl_report_attr_cmd_t cmd = {};
  cmd.zcl_basic_cmd.src_endpoint = ep;
  cmd.zcl_basic_cmd.dst_endpoint = 1;
  cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
  cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  cmd.clusterID = cluster;
  cmd.attributeID = attr;
  cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  cmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_report_attr_cmd_req(&cmd);
  esp_zb_lock_release();
  delay(15);                                           // не слать пачку отчётов разом: часть теряется
}

// Сообщить Z2M текущее состояние (после изменения с клавиатуры, кнопок, пульта или при подключении)
static void reportAll() {
  selfReport = true;
  // Библиотека считает насыщенность из RGB и на чистых цветах (255,0,0) выходит 255,
  // а допустимо максимум 254 — отсюда ошибка 0x87. Поднимаем нули до 1: на глаз не видно.
  uint8_t rr = lr ? lr : 1, gg = lg ? lg : 1, bb = lb ? lb : 1;
  zbLight.setLight(lightOn, lLevel, rr, gg, bb);
  zbEffect.setLight(effect > 0, effect ? effect * EFF_STEP : 254);
  selfReport = false;

  // атрибуты обновлены — теперь отправляем их Home Assistant
  sendAttrReport(EP_LIGHT,  ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
  sendAttrReport(EP_LIGHT,  ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
  sendAttrReport(EP_LIGHT,  ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID);
  sendAttrReport(EP_LIGHT,  ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID);
  sendAttrReport(EP_EFFECT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
  sendAttrReport(EP_EFFECT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
}

// ============================================================
//   РАДИО
// ============================================================
// SuperMini плохо принимает на чип-антенне, поэтому передатчик на максимум.
// Вызов обязательно под блокировкой стека.

static void applyTxPower() {
  esp_zb_lock_acquire(portMAX_DELAY);
  int8_t before = 0, after = 0;
  esp_zb_get_tx_power(&before);
  esp_zb_set_tx_power(ZB_TX_POWER_DBM);
  esp_zb_get_tx_power(&after);
  esp_zb_lock_release();
  Serial.printf("[ZB] moshchnost radio: bylo %d dBm, stalo %d dBm\n", before, after);
}

// ============================================================
//   ПАМЯТЬ (переживает отключение питания)
// ============================================================

static void loadState() {
  prefs.begin("korona", false);
  lightOn = prefs.getBool("on", false);
  lr      = prefs.getUChar("r", 255);
  lg      = prefs.getUChar("g", 160);
  lb      = prefs.getUChar("b", 60);
  lLevel  = prefs.getUChar("lvl", 200);
  if (lLevel < MIN_LEVEL) lLevel = 200;
  uint8_t sp = prefs.getUChar("spd", 4);
  speedIdx = sp < 1 ? 1 : (sp > 7 ? 7 : sp);
  uint8_t e = prefs.getUChar("eff", 0);
  effect  = e > N_EFFECTS ? 0 : e;
  int n   = prefs.getUShort("n", DEFAULT_LEDS);
  numLeds = n < 1 ? 1 : (n > MAX_LEDS ? MAX_LEDS : n);
}

static void saveState() {
  prefs.putBool("on", lightOn);
  prefs.putUChar("r", lr);
  prefs.putUChar("g", lg);
  prefs.putUChar("b", lb);
  prefs.putUChar("lvl", lLevel);
  prefs.putUChar("eff", effect);
  prefs.putUChar("spd", speedIdx);
  prefs.putUShort("n", (uint16_t)numLeds);
}

// ============================================================
//   ЛЕНТА  (RMT: 800 кГц, порядок GRB)
// ============================================================

static uint8_t    px[MAX_LEDS][3];
static rmt_data_t rmtBuf[MAX_LEDS * 24];
static bool       rmtOk = false;

static void ledsShow(int count = -1) {
  if (!rmtOk) return;
  if (count < 0) count = numLeds;
  int i = 0;
  for (int n = 0; n < count; n++) {
    uint8_t col[3] = { px[n][2], px[n][0], px[n][1] };   // B, R, G (порядок этой ленты)
    for (int c = 0; c < 3; c++)
      for (int bit = 7; bit >= 0; bit--) {
        bool one = col[c] & (1 << bit);
        rmtBuf[i].level0 = 1; rmtBuf[i].duration0 = one ? 8 : 4;   // тик = 0.1 мкс
        rmtBuf[i].level1 = 0; rmtBuf[i].duration1 = one ? 4 : 8;
        i++;
      }
  }
  rmtWrite(PIN_DATA, rmtBuf, count * 24, RMT_WAIT_FOR_EVER);
}

static uint8_t bright() { return lightOn ? (uint16_t)lLevel * MAX_BRIGHT / 255 : 0; }
static inline uint8_t sc(uint8_t v, uint8_t br) { return (uint16_t)v * br / 255; }

static void wheel(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (h < 85)       { r = 255 - h * 3; g = h * 3; b = 0; }
  else if (h < 170) { h -= 85;  r = 0; g = 255 - h * 3; b = h * 3; }
  else              { h -= 170; r = h * 3; g = 0; b = 255 - h * 3; }
}

static void drawSolid() {
  uint8_t br = bright();
  for (int n = 0; n < numLeds; n++) {
    px[n][0] = sc(lr, br); px[n][1] = sc(lg, br); px[n][2] = sc(lb, br);
  }
  ledsShow();
}

// ---------- эффекты (кадр каждые 25 мс) ----------
static uint32_t frame = 0;
#define TAIL 12                 // длина хвоста у «огонька», в адресах

static void drawEffect() {
  frame++;
  uint8_t br = bright();
  switch (effect) {
    case 1: {                                            // Радуга — бежит по ленте
      for (int n = 0; n < numLeds; n++) {
        uint8_t r, g, b; wheel(frame * 2 + n * 4, r, g, b);
        px[n][0] = sc(r, br); px[n][1] = sc(g, br); px[n][2] = sc(b, br);
      }
      break;
    }
    case 2: {                                            // Дыхание — свой цвет плавно гаснет и загорается
      uint16_t t = frame % 120;                          // цикл 3 с
      uint8_t k = t < 60 ? t * 255 / 60 : (120 - t) * 255 / 60;
      k = (uint16_t)k * k / 255;                         // мягче у нуля
      uint8_t b2 = (uint16_t)br * k / 255;
      for (int n = 0; n < numLeds; n++) {
        px[n][0] = sc(lr, b2); px[n][1] = sc(lg, b2); px[n][2] = sc(lb, b2);
      }
      break;
    }
    case 3: {                                            // Перелив — вся лента одним цветом, цвет плывёт
      uint8_t r, g, b; wheel(frame / 2, r, g, b);
      for (int n = 0; n < numLeds; n++) {
        px[n][0] = sc(r, br); px[n][1] = sc(g, br); px[n][2] = sc(b, br);
      }
      break;
    }
    case 4: {                                            // Огонёк — бежит вдоль ленты своим цветом с хвостом
      int head = (frame / 2) % numLeds;
      for (int n = 0; n < numLeds; n++) {
        int d = (head - n + numLeds) % numLeds;        // расстояние до головы
        uint8_t k = d < TAIL ? 255 - d * (255 / TAIL) : 0;
        uint8_t b2 = (uint16_t)br * k / 255;
        px[n][0] = sc(lr, b2); px[n][1] = sc(lg, b2); px[n][2] = sc(lb, b2);
      }
      break;
    }
    case 5: {                                            // Мерцание — случайные вспышки своим цветом
      for (int n = 0; n < numLeds; n++)
        for (int c = 0; c < 3; c++) px[n][c] = px[n][c] * 7 / 8;   // плавно гаснут
      for (int i = 0; i < numLeds / 20 + 1; i++) {
        int n = random(numLeds);
        px[n][0] = sc(lr, br); px[n][1] = sc(lg, br); px[n][2] = sc(lb, br);
      }
      break;
    }
    case 6: {                                            // Ночник — белый, 20% яркости (ползунок яркости не влияет)
      for (int n = 0; n < numLeds; n++) { px[n][0] = 51; px[n][1] = 51; px[n][2] = 51; }
      break;
    }
    case 7: {                                            // Камин — живое мерцание ВЫБРАННЫМ цветом
      static uint8_t fireK[MAX_LEDS];
      for (int n = 0; n < numLeds; n++) {
        if (random(8) == 0) fireK[n] = random(90, 256);   // каждый адрес время от времени берёт новую яркость
        uint8_t b2 = (uint16_t)br * fireK[n] / 255;
        uint8_t tr = sc(lr, b2), tg = sc(lg, b2), tb = sc(lb, b2);
        px[n][0] = (px[n][0] * 5 + tr * 3) / 8;           // и плавно к ней движется
        px[n][1] = (px[n][1] * 5 + tg * 3) / 8;
        px[n][2] = (px[n][2] * 5 + tb * 3) / 8;
      }
      break;
    }
    case 9: {                                            // Северное сияние — медленные зелёно-синие переливы
      uint8_t k = br;
      for (int n = 0; n < numLeds; n++) {
        float w = (sinf(n * 0.07f + frame * 0.02f) + 1.0f) * 0.5f;     // 0..1
        float v = (sinf(n * 0.13f - frame * 0.03f) + 1.0f) * 0.5f;
        uint8_t r, g, b; wheel(85 + (uint8_t)(70 * w), r, g, b);      // от зелёного к синему
        uint8_t b2 = (uint16_t)k * (60 + (uint8_t)(195 * v)) / 255;
        px[n][0] = sc(r, b2); px[n][1] = sc(g, b2); px[n][2] = sc(b, b2);
      }
      break;
    }
    case 10: {                                           // Полиция — красное/синее по половинам с паузой
      bool seg = (frame / 5) % 2;
      bool gap = (frame % 5) == 4;
      for (int n = 0; n < numLeds; n++) {
        bool left = n < numLeds / 2;
        bool red = (left != seg);
        uint8_t v = gap ? 0 : br;
        px[n][0] = red ? v : 0; px[n][1] = 0; px[n][2] = red ? 0 : v;
      }
      break;
    }
    case 8: {                                            // Волна — свой цвет, по ленте бежит волна яркости
      for (int n = 0; n < numLeds; n++) {
        float w = (sinf(n * 0.25f - frame * 0.15f) + 1.0f) * 0.5f;     // 0..1
        uint8_t k = 40 + (uint8_t)(215 * w);
        uint8_t b2 = (uint16_t)br * k / 255;
        px[n][0] = sc(lr, b2); px[n][1] = sc(lg, b2); px[n][2] = sc(lb, b2);
      }
      break;
    }
  }
  ledsShow();
}

// ---------- настройка длины ленты (клавиша L) ----------
// Горит первый адрес (зелёный), последний (красный) и всё между ними слабо-белым.
// Жмёшь + — лента «удлиняется», красная точка уезжает дальше. Когда она ушла
// за конец ленты и пропала — жми -, пока не появится снова. Это и есть длина.
static bool lenMode = false, lenDirty = false;

static void drawLen() {
  memset(px, 0, sizeof(px));
  for (int n = 0; n < numLeds; n++) { px[n][0] = 12; px[n][1] = 12; px[n][2] = 12; }
  px[0][0] = 0;   px[0][1] = 255; px[0][2] = 0;                       // первый — зелёный
  px[numLeds - 1][0] = 255; px[numLeds - 1][1] = 0; px[numLeds - 1][2] = 0;   // последний — красный
  if (numLeds == 1) { px[0][0] = 255; px[0][1] = 255; px[0][2] = 0; }
  ledsShow(MAX_LEDS);      // шлём весь буфер, чтобы хвост за последним гас
}

static void lenChange(int d) {
  int n = numLeds + d;
  numLeds = n < 1 ? 1 : (n > MAX_LEDS ? MAX_LEDS : n);
  lenDirty = true;
  Serial.printf("[DLINA] adresov: %d\n", numLeds);
}

static void lenKey(char c) {
  switch (c) {
    case '+': case '=': lenChange(+1);  break;
    case '-': case '_': lenChange(-1);  break;
    case '>': case '.': lenChange(+10); break;
    case '<': case ',': lenChange(-10); break;
    case 'L': case 'l': {
      lenMode = false;
      prefs.putUShort("n", (uint16_t)numLeds);
      memset(px, 0, sizeof(px));
      ledsShow(MAX_LEDS);                              // погасить всё
      Serial.printf("[DLINA] sohraneno: %d adresov. Rezhim zakryt.\n", numLeds);
      dirty = true;
      break;
    }
    case '\n': case '\r': case ' ': break;
    default: Serial.println("[DLINA] + - = 1 adres, > < = 10 adresov, L = sohranit i vyjti"); break;
  }
}

// ============================================================
//   МОНИТОР ПОРТА
// ============================================================

static const char *EFFECT_NAMES[] = { "svoi cvet", "raduga", "dyhanie", "pereliv", "ogonek", "mercanie", "nochnik", "kamin", "volna", "siyanie", "policiya" };

static void status() {
  Serial.printf("[KORONA] lampa %s | effekt: %s | yarkost %u/254 | cvet %u,%u,%u | adresov %d | Zigbee %s | skorost %u/7\n",
                lightOn ? "VKL" : "vykl", EFFECT_NAMES[effect], lLevel, lr, lg, lb, numLeds,
                Zigbee.connected() ? "v seti" : "net seti", speedIdx);
}

static void printHelp() {
  Serial.println();
  Serial.println("=========== MONITOR PORTA ===========");
  Serial.println("  0     vkl / vykl lampu");
  Serial.println("  1..9  effekt: 1 raduga, 2 dyhanie, 3 pereliv, 4 ogonek, 5 mercanie,");
  Serial.println("        6 nochnik (belyi 20%), 7 kamin, 8 volna, 9 siyanie");
  Serial.println("  p     effekt 10: policiya");
  Serial.println("  c     svoi cvet (bez effekta)");
  Serial.println("  r g b w   krasnyi / zelenyi / sinii / belyi");
  Serial.println("  + -   yarche / tishe");
  Serial.println("  L     DLINA lenty: + - (1 adres), > < (10 adresov), L = sohranit");
  Serial.println("  I     ZAPIS IK-PULTA (20 knopok po 2 raza), q = otmena");
  Serial.println("  S     status");
  Serial.println("  X     SBROS seti Zigbee (potom dobavlyat zanovo)");
  Serial.println("  ?     spravka");
  Serial.println("=====================================");
  Serial.println();
}

static void setColor(uint8_t r, uint8_t g, uint8_t b) {
  lr = r; lg = g; lb = b; effect = 0; lightOn = true;
  if (lLevel < MIN_LEVEL) lLevel = 200;
  changed();
}

// ===== BEGIN-NEW-MODULE =====
// ============================================================
//   ФИЗИЧЕСКОЕ УПРАВЛЕНИЕ: 3 КНОПКИ + ИК-ПУЛЬТ
// ============================================================
// Всё это только локально; в Home Assistant уходит лишь состояние лампы
// (вкл, цвет, яркость, эффект) — так же, как при нажатии клавиш в мониторе.

// Любое изменение с кнопок или пульта уходит в Home Assistant: лампа (вкл, цвет, яркость)
// и эффект. Отчёт отправляется через 250 мс после последнего изменения, чтобы при
// удержании кнопки яркости не заваливать сеть пакетами. Скорость и таймер в HA не уходят.
static volatile bool reportPending = false;
static uint32_t      reportAt = 0;
static void reportIfOnline() { reportPending = true; reportAt = millis() + 250; }
static void localChange()    { changed(); reportIfOnline(); }
static void reportLoop() {
  if (reportPending && (int32_t)(millis() - reportAt) >= 0 && Zigbee.connected()) {
    reportPending = false;
    reportAll();
  }
}

// ---------- яркость: 7 ступеней ----------
static const uint8_t BR_STEPS[7] = { 25, 60, 95, 130, 170, 210, 254 };

static int briIndex() {
  int best = 0, bd = 999;
  for (int i = 0; i < 7; i++) {
    int d = abs((int)lLevel - (int)BR_STEPS[i]);
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

static void briStep(int dir) {
  int i = briIndex() + dir;
  if (i < 0) i = 0;
  if (i > 6) i = 6;
  lLevel = BR_STEPS[i];
  if (dir > 0) lightOn = true;
  Serial.printf("[YARKOST] stupen %d/7\n", i + 1);
  localChange();
}

// ---------- скорость эффектов: 7 ступеней (только в самой плате) ----------
static void speedStep(int d) {
  int s = (int)speedIdx + d;
  if (s < 1) s = 1;
  if (s > 7) s = 7;
  speedIdx = s;
  needSave = true; tChanged = millis();
  Serial.printf("[SKOROST] stupen %d/7\n", s);
}

// ---------- цвета ----------
struct Col { uint8_t r, g, b; const char *name; };
#define N_COLORS 10
static const Col COLORS[N_COLORS] = {
  { 255,   0,   0, "krasnyi" },
  { 255,  80,   0, "oranzhevyi" },
  { 255, 190,   0, "zheltyi" },
  {   0, 255,   0, "zelenyi" },
  {   0, 170, 255, "goluboi" },
  {   0,   0, 255, "sinii" },
  { 140,   0, 255, "fioletovyi" },
  { 255,   0, 110, "rozovyi" },
  { 255, 160,  60, "teplyi belyi" },
  { 255, 255, 255, "holodnyi belyi" },
};
static int colorIdx = -1;

// Эффекты, в которых виден выбранный цвет
static bool effectUsesColor(uint8_t e) { return e == 0 || e == 2 || e == 4 || e == 5 || e == 7 || e == 8; }

static void nextColor() {
  colorIdx = (colorIdx + 1) % N_COLORS;
  lr = COLORS[colorIdx].r; lg = COLORS[colorIdx].g; lb = COLORS[colorIdx].b;
  if (!effectUsesColor(effect)) effect = 0;       // у радуги и т.п. цвета нет — уходим в свой цвет
  lightOn = true;
  if (lLevel < MIN_LEVEL) lLevel = 200;
  Serial.printf("[CVET] %s\n", COLORS[colorIdx].name);
  localChange();
}

static void stepMode(int dir) {
  int e = ((int)effect + dir + N_EFFECTS + 1) % (N_EFFECTS + 1);
  effect = e;
  lightOn = true;
  if (lLevel < MIN_LEVEL) lLevel = 200;
  Serial.printf("[REZHIM] %d: %s\n", e, EFFECT_NAMES[e]);
  localChange();
}

// ---------- мигание-подтверждение ----------
static void blinkN(int n) {
  for (int k = 0; k < n; k++) {
    for (int i = 0; i < numLeds; i++) { px[i][0] = 90; px[i][1] = 90; px[i][2] = 90; }
    ledsShow(); delay(150);
    memset(px, 0, sizeof(px));
    ledsShow(); delay(150);
  }
  dirty = true;
}

// ---------- таймер «выключить через N часов» ----------
static bool     timerOn = false;
static uint32_t offAtMs = 0;

static void timerStart(int hours) {
  if (!lightOn) { lightOn = true; if (lLevel < MIN_LEVEL) lLevel = 200; }
  offAtMs = millis() + (uint32_t)hours * 3600000UL;
  timerOn = true;
  Serial.printf("[TAIMER] vyklyuchu cherez %d ch\n", hours);
  blinkN(hours);                                   // сколько миганий — столько часов
  localChange();
}

static void timerLoop() {
  if (timerOn && (int32_t)(millis() - offAtMs) >= 0) {
    timerOn = false;
    lightOn = false;
    Serial.println("[TAIMER] vremya vyshlo, lenta vyklyuchena");
    localChange();
  }
}

static void powerToggle() {
  lightOn = !lightOn;
  if (lightOn && lLevel < MIN_LEVEL) lLevel = 200;
  if (!lightOn) timerOn = false;
  Serial.printf("[KNOPKA] lampa %s\n", lightOn ? "VKL" : "VYKL");
  localChange();
}

// ============================================================
//   3 КНОПКИ
// ============================================================

struct Btn {
  uint8_t  pin;
  bool     raw, stable, handled;
  uint32_t tRaw, tDown, tRepeat;
};
static Btn btnPwr = { PIN_BTN_PWR, false, false, false, 0, 0, 0 };
static Btn btnCol = { PIN_BTN_COL, false, false, false, 0, 0, 0 };
static Btn btnBri = { PIN_BTN_BRI, false, false, false, 0, 0, 0 };

// +1 — только что нажата, -1 — только что отпущена, 0 — без изменений
static int btnPoll(Btn &b) {
  bool r = digitalRead(b.pin) == LOW;                // кнопка замыкает пин на GND
  uint32_t now = millis();
  if (r != b.raw) { b.raw = r; b.tRaw = now; }
  if (r != b.stable && now - b.tRaw >= 30) {         // антидребезг 30 мс
    b.stable = r;
    if (r) { b.tDown = now; b.handled = false; return 1; }
    return -1;
  }
  return 0;
}
static uint32_t btnHeld(const Btn &b) { return b.stable ? millis() - b.tDown : 0; }

static uint32_t modeWindowUntil = 0;                 // до этого момента короткий клик = смена РЕЖИМА

static void buttonsLoop() {
  int e;

  // Кнопка 1: клик — вкл/выкл, удержание 5 с — сброс сети Zigbee
  e = btnPoll(btnPwr);
  if (btnPwr.stable && !btnPwr.handled && btnHeld(btnPwr) >= 5000) {
    btnPwr.handled = true;
    Serial.println("[KNOPKA] 5 s: sbros seti Zigbee");
    blinkN(3);
    delay(300);
    Zigbee.factoryReset();
  }
  if (e == -1 && !btnPwr.handled) powerToggle();

  // Кнопка 2: клик — следующий цвет; удержание 3 с — следующий режим и 3 с «окно режимов»,
  // в котором каждый клик тоже меняет режим (окно продлевается после каждой смены)
  e = btnPoll(btnCol);
  if (btnCol.stable && !btnCol.handled && btnHeld(btnCol) >= 3000) {
    btnCol.handled = true;
    stepMode(+1);
    modeWindowUntil = millis() + 3000;
  }
  if (e == -1 && !btnCol.handled) {
    if ((int32_t)(modeWindowUntil - millis()) > 0) { stepMode(+1); modeWindowUntil = millis() + 3000; }
    else nextColor();
  }

  // Кнопка 3: клик — ярче на 1 ступень; удержание 2 с — темнее на 1 ступень в секунду
  e = btnPoll(btnBri);
  if (btnBri.stable && btnHeld(btnBri) >= 2000) {
    if (!btnBri.handled) { btnBri.handled = true; btnBri.tRepeat = millis(); briStep(-1); }
    else if (millis() - btnBri.tRepeat >= 1000) { btnBri.tRepeat += 1000; briStep(-1); }
  }
  if (e == -1 && !btnBri.handled) briStep(+1);
}

// ============================================================
//   ИК-ПУЛЬТ  (протокол NEC; без библиотек)
// ============================================================

#define IR_MAX 140
static volatile uint16_t irBuf[IR_MAX];
static volatile uint16_t irLen = 0;
static volatile uint32_t irLastUs = 0;
static volatile bool     irActive = false;

// ИК-приёмник держит линию в 1 и опускает в 0 на время несущей.
// Записываем длительности между перепадами; пауза > 20 мс = начало нового кадра.
static void IRAM_ATTR irIsr() {
  uint32_t now = micros();
  uint32_t dt = now - irLastUs;
  irLastUs = now;
  if (dt > 20000) { irLen = 0; irActive = true; return; }
  if (!irActive) return;
  uint16_t k = irLen;
  if (k < IR_MAX) { irBuf[k] = dt > 65000 ? 65000 : (uint16_t)dt; irLen = k + 1; }
}

static bool irFrame(uint16_t *out, uint16_t &n) {
  if (!irActive || irLen == 0) return false;
  if (micros() - irLastUs < 15000) return false;     // кадр ещё не закончился
  noInterrupts();
  n = irLen;
  for (uint16_t i = 0; i < n; i++) out[i] = irBuf[i];
  irLen = 0; irActive = false;
  interrupts();
  return n > 0;
}

enum { IR_UNKNOWN = 0, IR_NEC, IR_REPEAT };

// NEC: старт 9 мс + 4.5 мс, 32 бита (мл. вперёд): 0 = 560+560, 1 = 560+1690 мкс.
// Повтор при удержании: 9 мс + 2.25 мс + 560 мкс.
static int irDecode(const uint16_t *b, uint16_t n, uint32_t &code) {
  if (n >= 3 && n <= 4 && b[0] > 7500 && b[0] < 10500 && b[1] > 1700 && b[1] < 2800) return IR_REPEAT;
  if (n < 66 || b[0] < 7500 || b[0] > 10500 || b[1] < 3500 || b[1] > 5500) return IR_UNKNOWN;
  uint32_t v = 0;
  for (int i = 0; i < 32; i++) {
    uint16_t mk = b[2 + 2 * i], sp = b[3 + 2 * i];
    if (mk < 300 || mk > 900) return IR_UNKNOWN;
    if (sp >= 300 && sp < 1100) { /* 0 */ }
    else if (sp >= 1100 && sp < 2300) v |= (1UL << i);
    else return IR_UNKNOWN;
  }
  code = v;
  return IR_NEC;
}

// ---------- кнопки пульта ----------
enum {
  A_POWER, A_AUTO, A_SPEED_UP, A_SPEED_DN, A_MODE_UP, A_MODE_DN, A_BRI_UP, A_BRI_DN, A_NIGHT,
  A_RED, A_GREEN, A_BLUE, A_YELLOW, A_ORANGE, A_VIOLET, A_CYAN,
  A_T1, A_T2, A_T3, A_T4, N_IR
};
static const char *IR_NAMES[N_IR] = {
  "VKL/VYKL", "AUTO (raduga)", "SPEED +", "SPEED -", "MODE +", "MODE -", "YARKOST +", "YARKOST -",
  "W (nochnik)", "KRASNYI", "ZELENYI", "SINII", "ZHELTYI", "ORANZHEVYI", "FIOLETOVYI", "GOLUBOI",
  "TAIMER 1 ch", "TAIMER 2 ch", "TAIMER 3 ch", "TAIMER 4 ch"
};

// Коды кнопок пульта (NEC, адрес 0xFF00), записаны клавишей I. Нули = не записано.
static uint32_t IR_CODES[N_IR] = {
  0xBF40FF00,  // VKL/VYKL
  0xA35CFF00,  // AUTO (raduga)
  0xAA55FF00,  // SPEED +
  0xB649FF00,  // SPEED -
  0xAE51FF00,  // MODE +
  0xB24DFF00,  // MODE -
  0xE718FF00,  // YARKOST +
  0xE41BFF00,  // YARKOST -
  0xEB14FF00,  // W (nochnik)
  0xEA15FF00,  // KRASNYI
  0xE916FF00,  // ZELENYI
  0xE817FF00,  // SINII
  0xEF10FF00,  // ZHELTYI
  0xEE11FF00,  // ORANZHEVYI
  0xED12FF00,  // FIOLETOVYI
  0xEC13FF00,  // GOLUBOI
  0xF30CFF00,  // TAIMER 1 ch
  0xF20DFF00,  // TAIMER 2 ch
  0xF10EFF00,  // TAIMER 3 ch
  0xF00FFF00,  // TAIMER 4 ch
};

static void irAct(int a) {
  switch (a) {
    case A_POWER:    powerToggle(); return;
    case A_AUTO:     effect = 1; lightOn = true; break;
    case A_SPEED_UP: speedStep(+1); return;
    case A_SPEED_DN: speedStep(-1); return;
    case A_MODE_UP:  stepMode(+1); return;
    case A_MODE_DN:  stepMode(-1); return;
    case A_BRI_UP:   briStep(+1); return;
    case A_BRI_DN:   briStep(-1); return;
    case A_NIGHT:    effect = 6; lightOn = true; break;
    case A_RED:      setColor(255, 0, 0);     reportIfOnline(); return;
    case A_GREEN:    setColor(0, 255, 0);     reportIfOnline(); return;
    case A_BLUE:     setColor(0, 0, 255);     reportIfOnline(); return;
    case A_YELLOW:   setColor(255, 190, 0);   reportIfOnline(); return;
    case A_ORANGE:   setColor(255, 80, 0);    reportIfOnline(); return;
    case A_VIOLET:   setColor(140, 0, 255);   reportIfOnline(); return;
    case A_CYAN:     setColor(0, 170, 255);   reportIfOnline(); return;
    case A_T1: timerStart(1); return;
    case A_T2: timerStart(2); return;
    case A_T3: timerStart(3); return;
    case A_T4: timerStart(4); return;
    default: return;
  }
  if (lLevel < MIN_LEVEL) lLevel = 200;
  Serial.printf("[IR] %s\n", IR_NAMES[a]);
  localChange();
}

static int      irLastAct = -1;
static uint32_t irLastFrameMs = 0;
static uint32_t irRepeatMs = 0;

static void irNormalFrame(int kind, uint32_t code) {
  uint32_t now = millis();
  if (kind == IR_UNKNOWN) return;
  if (kind == IR_REPEAT) {
    // удержание: повторяем только яркость и скорость, не чаще 4 раз в секунду
    bool holdable = irLastAct == A_BRI_UP || irLastAct == A_BRI_DN || irLastAct == A_SPEED_UP || irLastAct == A_SPEED_DN;
    if (holdable && now - irLastFrameMs < 300 && now - irRepeatMs >= 250) { irRepeatMs = now; irAct(irLastAct); }
    irLastFrameMs = now;
    return;
  }
  irLastFrameMs = now;
  int a = -1;
  for (int i = 0; i < N_IR; i++) if (IR_CODES[i] && IR_CODES[i] == code) { a = i; break; }
  if (a < 0) { Serial.printf("[IR] kod 0x%08lX ne naznachen\n", (unsigned long)code); irLastAct = -1; return; }
  irLastAct = a;
  irRepeatMs = now;
  irAct(a);
}

// ---------- запись пульта (клавиша I в мониторе) ----------
static bool     irLearn = false;
static int      learnStep = 0, learnPass = 0;
static uint32_t learnFirst = 0, learnCodes[N_IR], learnIgnoreUntil = 0;

static void learnPrompt() {
  Serial.printf("\n[IR] knopka %d/%d: %s  -- nazhmi (%s raz)\n", learnStep + 1, N_IR, IR_NAMES[learnStep], learnPass == 0 ? "1-i" : "2-oi");
}

static void irLearnStart() {
  irLearn = true; learnStep = 0; learnPass = 0;
  Serial.println("\n===== ZAPIS IK-PULTA =====");
  Serial.println("Nazhimai kazhduyu knopku po 2 raza, kak prosit. q = otmena.");
  Serial.println("Kopki so znachkom mikrofona/muzyki ne nuzhny.");
  learnPrompt();
}

static void irLearnFinish() {
  irLearn = false;
  Serial.println("\n========== IK: GOTOVO. Skopiruj vse nizhe i prishli ==========");
  Serial.println("protokol: NEC");
  for (int i = 0; i < N_IR; i++)
    Serial.printf("  %-14s adres=0x%04X komanda=0x%02X   kod=0x%08lX\n", IR_NAMES[i],
                  (unsigned)(learnCodes[i] & 0xFFFF), (unsigned)((learnCodes[i] >> 16) & 0xFF), (unsigned long)learnCodes[i]);
  Serial.println("\nstatic uint32_t IR_CODES[N_IR] = {");
  for (int i = 0; i < N_IR; i++) Serial.printf("  0x%08lX,  // %s\n", (unsigned long)learnCodes[i], IR_NAMES[i]);
  Serial.println("};");
  Serial.println("==============================================================\n");
}

static void irLearnFrame(int kind, uint32_t code, const uint16_t *b, uint16_t n) {
  if (millis() < learnIgnoreUntil) return;
  if (kind == IR_REPEAT) return;                     // кнопку держат — повтор пропускаем
  if (kind == IR_UNKNOWN) {
    Serial.printf("[IR] ne NEC, %u impulsov. Syrye (mks):", (unsigned)n);
    for (uint16_t i = 0; i < n && i < 80; i++) Serial.printf(" %u", (unsigned)b[i]);
    Serial.println();
    return;
  }
  learnIgnoreUntil = millis() + 350;
  for (int i = 0; i < learnStep; i++) if (learnCodes[i] == code) {
    Serial.printf("[IR] etot kod uzhe zapisan dlya knopki '%s' — nazhmi nuzhnuyu knopku\n", IR_NAMES[i]);
    learnPass = 0; learnPrompt();
    return;
  }
  if (learnPass == 0) {
    learnFirst = code; learnPass = 1;
    Serial.printf("[IR] prinyal 0x%08lX. Teper tu zhe knopku eshche raz.\n", (unsigned long)code);
  } else if (code == learnFirst) {
    learnCodes[learnStep] = code;
    Serial.printf("[IR] OK: %s = 0x%08lX\n", IR_NAMES[learnStep], (unsigned long)code);
    learnStep++; learnPass = 0;
    if (learnStep >= N_IR) irLearnFinish(); else learnPrompt();
  } else {
    Serial.println("[IR] ne sovpalo — nachnem etu knopku zanovo");
    learnPass = 0; learnPrompt();
  }
}

static void irLoop() {
  static uint16_t buf[IR_MAX];
  uint16_t n;
  if (!irFrame(buf, n)) return;
  uint32_t code = 0;
  int kind = irDecode(buf, n, code);
  if (irLearn) irLearnFrame(kind, code, buf, n);
  else irNormalFrame(kind, code);
}
// ===== END-NEW-MODULE =====

static void key(char c) {
  if (lenMode) { lenKey(c); return; }
  if (irLearn && (c == 'q' || c == 'Q')) { irLearn = false; Serial.println("[IR] zapis otmenena"); return; }
  if (c == '\n' || c == '\r' || c == ' ') return;

  switch (c) {
    case '0': lightOn = !lightOn; changed(); break;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
      effect = c - '0'; lightOn = true; changed(); break;
    case 'p': effect = 10; lightOn = true; changed(); break;
    case 'c': effect = 0; lightOn = true; changed(); break;
    case 'r': setColor(255, 0, 0);     break;
    case 'g': setColor(0, 255, 0);     break;
    case 'b': setColor(0, 0, 255);     break;
    case 'w': setColor(255, 160, 60);  break;
    case '+': { int v = lLevel + 26; lLevel = v > 254 ? 254 : v; lightOn = true; changed(); break; }
    case '-': { int v = lLevel - 26; lLevel = v < 8 ? 8 : v;     lightOn = true; changed(); break; }
    case 'L': case 'l':
      lenMode = true; lenDirty = true;
      Serial.println("\n[DLINA] Rezhim dliny. Zelenyi = pervyi adres, krasnyi = poslednii.");
      Serial.println("[DLINA] + dobavit adres, - ubrat, > < po 10, L = sohranit i vyjti.");
      Serial.printf("[DLINA] seichas: %d adresov\n\n", numLeds);
      return;
    case 'I': irLearnStart(); return;
    case 'S': status(); return;
    case 'X':
      Serial.println("\n!!! Sbros seti Zigbee. Plata perezagruzitsya.");
      Serial.println("!!! Dobav ee zanovo v Z2M (Permit join).\n");
      delay(500);
      Zigbee.factoryReset();
      return;
    case '?': case 'h': case 'H': printHelp(); return;
    default: Serial.printf("Neizvestnaya klavisha '%c'. Nazhmi ? dlya spravki.\n", c); return;
  }
  if (Zigbee.connected()) reportAll();   // чтобы HA увидел изменение
}

// ============================================================
//   SETUP
// ============================================================

static void addEp(ZigbeeEP &ep) {
  ep.setPowerSource(ZB_POWER_SOURCE_MAINS);            // иначе Z2M считает устройство спящим
  Zigbee.addEndpoint(&ep);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("#####################################################");
  Serial.println("#   KORONA  -  ARGB-lenta, Zigbee Router            #");
  Serial.println("#####################################################");

  rmtOk = rmtInit(PIN_DATA, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);   // тик 0.1 мкс
  gpio_set_drive_capability((gpio_num_t)PIN_DATA, GPIO_DRIVE_CAP_3);
  loadState();
  pinMode(PIN_BTN_PWR, INPUT_PULLUP);                  // кнопки замыкают пин на GND
  pinMode(PIN_BTN_COL, INPUT_PULLUP);
  pinMode(PIN_BTN_BRI, INPUT_PULLUP);
  pinMode(PIN_IR, INPUT_PULLUP);
  attachInterrupt(PIN_IR, irIsr, CHANGE);
  Serial.printf("[LED] GPIO%d, %d adresov, RMT %s\n", PIN_DATA, numLeds, rmtOk ? "OK" : "OSHIBKA");
  if (effect && lightOn) drawEffect(); else drawSolid();

  zbLight.setManufacturerAndModel("DIY", "Korona-ARGB");
  zbLight.onLightChange(onLight);
  zbEffect.onLightChange(onEffect);
  addEp(zbLight);
  addEp(zbEffect);

  // Роутер без детей: спящие устройства не цепляются к слабому SuperMini,
  // сам он при этом пересылает пакеты как обычно.
  esp_zb_cfg_t zbCfg = ZIGBEE_DEFAULT_ROUTER_CONFIG();
  zbCfg.nwk_cfg.zczr_cfg.max_children = 0;
  if (!Zigbee.begin(&zbCfg))
    Serial.println("[ZB] Zigbee ne startoval — rabotayu bez seti");

  applyTxPower();     // после старта стека

  printHelp();
  status();
}

// ============================================================
//   LOOP
// ============================================================

void loop() {
  static uint32_t tAnim = 0, tRefresh = 0, tBeat = 0, tLog = 0;
  static bool zbOnline = false;

  while (Serial.available()) key(Serial.read());
  buttonsLoop();
  irLoop();
  timerLoop();
  reportLoop();

  if (lenMode) {                                       // настройка длины: Zigbee и эффекты не мешают
    static uint32_t tLen = 0;
    if (lenDirty || millis() - tLen >= 1000) { lenDirty = false; tLen = millis(); drawLen(); }
    delay(5);
    return;
  }

  // следим за подключением к сети
  bool online = Zigbee.connected();
  if (online != zbOnline) {
    zbOnline = online;
    if (online) {
      Serial.println("[ZB] podklyuchilis k seti");
      applyTxPower();     // после входа в сеть стек мог сбросить мощность
      reportAll();
    } else {
      Serial.println("[ZB] svyaz s setyu poteryana");
    }
  }

  if (dirty) {
    dirty = false;
    if (!(effect && lightOn)) drawSolid();
    status();
  }

  if (effect && lightOn) {
    if (millis() - tAnim >= ANIM_MS[speedIdx]) { tAnim = millis(); drawEffect(); }
  } else if (millis() - tRefresh >= 1000) {
    tRefresh = millis();
    drawSolid();                                       // подхват светодиодов, воткнутых позже
  }

  // «я жива»: Z2M обновляет last_seen
  if (online && millis() - tBeat >= HEARTBEAT) {
    tBeat = millis();
    selfReport = true;
    zbEffect.setLight(effect > 0, effect ? effect * EFF_STEP : 254);
    selfReport = false;
    sendAttrReport(EP_EFFECT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);   // обновляет last_seen
  }

  if (needSave && millis() - tChanged >= 3000) { needSave = false; saveState(); }

  if (!online && millis() - tLog >= 30000) {
    tLog = millis();
    Serial.println("[ZB] net seti — otkroj Permit join v Z2M (esli plata staraya — klavisha X)");
  }

  delay(2);
}
