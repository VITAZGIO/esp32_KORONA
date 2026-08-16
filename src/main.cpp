/*
 * ============================================================
 *   КОРОНА  —  ARGB-лента как Zigbee-устройство для Home Assistant
 * ============================================================
 *
 *   Плата: ESP32-H2 SuperMini
 *   Роль:  Zigbee Router (постоянное питание, ретранслирует чужие пакеты)
 *
 * ------------------------------------------------------------
 *   ПОДКЛЮЧЕНИЕ  (см. схему korona-esp32h2-schema.svg из чата)
 * ------------------------------------------------------------
 *
 *   ARGB-лента (3 провода: красный +12V, зелёный DATA, белый GND)
 *
 *     GPIO4 -- резистор 330 Ом -- [сюда, если нужен, преобразователь
 *              уровня 74AHCT125 3.3V->5V] -- зелёный провод (DATA)
 *
 *     Плата запитана от 5V, полученных DC-DC понижайкой 12V->5V.
 *     GND платы, GND ленты и минус БП — общие, соединены вместе.
 *
 *   Родная WiFi/Tuya-плата ленты (LF686C20) не используется вообще —
 *   ESP32 подключается напрямую к трём проводам ленты.
 *
 * ------------------------------------------------------------
 *   ЧТО ПОЯВИТСЯ В HOME ASSISTANT
 * ------------------------------------------------------------
 *   endpoint 10      — «Корона»: главный выключатель
 *   endpoints 11..20 — 10 кнопок-пресетов цвета (нажал -> сама погасла)
 *   endpoint  21     — кнопка «Ярче»
 *   endpoint  22     — кнопка «Тише»
 *
 * ------------------------------------------------------------
 *   ПРОШИВКА (ESP32-H2 SuperMini, встроенный USB не работает)
 * ------------------------------------------------------------
 *   GPIO8 подтянуть к 3V3 через 10 кОм — иначе плата не входит
 *   в режим загрузки. Затем: зажать BOOT -> нажать и отпустить
 *   RESET -> отпустить BOOT -> закрыть монитор порта -> Upload.
 *
 *   Отладка: монитор порта 115200, клавиша '?' — справка.
 * ============================================================
 */

#ifndef ZIGBEE_MODE_ZCZR
#error "Нужен флаг -DZIGBEE_MODE_ZCZR в platformio.ini"
#endif

#include <Arduino.h>
#include "Zigbee.h"
#include <Adafruit_NeoPixel.h>

// ============================================================
//   НАСТРОЙКИ
// ============================================================

#define PIN_DATA           4     // GPIO4 — чистый пин, есть на плате слева

// Сколько адресов (не физических светодиодов!) на ленте.
// У 12V ARGB-ленты один адрес обычно = 3 физических светодиода.
// Лучше взять с запасом: "лишние" адреса за концом ленты просто
// ни на что не влияют. Если конец ленты НЕ загорается — увеличь.
// Если хочешь точное число — раздели длину ленты (мм) на шаг между
// линиями разреза с ножницами на самой ленте.
#define NUM_LEDS          150

// Порядок каналов у большинства WS2811/SK6812-based 12V лент — GRB.
// Если после прошивки красный показывает зелёным (или наоборот) —
// поменяй NEO_GRB на NEO_RGB или NEO_BRG.
Adafruit_NeoPixel strip(NUM_LEDS, PIN_DATA, NEO_GRB + NEO_KHZ800);

// --- Поведение кнопок ---
#define BUTTON_HOLD_MS   1000    // через сколько кнопка в HA сама гаснет
#define ZB_JOIN_HINT_MS 30000    // как часто напоминать, что нет сети

#define BRIGHT_STEP        26    // шаг +/- (~10% от 255)
#define BRIGHT_MIN           8
#define BRIGHT_MAX         255
#define BRIGHT_DEFAULT     160

// --- Эндпоинты ---
#define EP_MAIN            10
#define EP_PRESET_FIRST    11
#define EP_BRIGHT_UP       21
#define EP_BRIGHT_DOWN     22

// ============================================================
//   ПРЕСЕТЫ ЦВЕТА — редактируй смело, любое число пунктов
// ============================================================

struct PresetDef {
  const char *name;   // имя в Home Assistant (латиницей — см. пояснение ниже)
  uint8_t r, g, b;
};

static const PresetDef PRESETS[] = {
  { "Krasnyi",        255,   0,   0 },
  { "Oranzhevyi",      255,  70,   0 },
  { "Zheltyi",         255, 190,   0 },
  { "Zelenyi",           0, 255,   0 },
  { "Goluboi",            0, 200, 255 },
  { "Sinii",              0,  60, 255 },
  { "Fioletovyi",       160,   0, 255 },
  { "Rozovyi",          255,   0, 120 },
  { "Teply belyi",      255, 150,  40 },
  { "Holodnyi belyi",   255, 255, 255 },
};

static const uint8_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);
#define PRESET_DEFAULT_ON  8   // индекс пресета, который включается по умолчанию
                                // (если главный выключатель включили, а цвет ещё
                                // ни разу не выбирали) — сейчас "Teply belyi"

// ============================================================
//   ОБЪЕКТЫ ZIGBEE
// ============================================================

static ZigbeeLight  zbMain(EP_MAIN);
static ZigbeeLight  zbBrightUp(EP_BRIGHT_UP);
static ZigbeeLight  zbBrightDown(EP_BRIGHT_DOWN);
static ZigbeeLight *zbPreset[PRESET_COUNT];

// ============================================================
//   ОЧЕРЕДЬ ЗАПРОСОВ
// ============================================================
//
// Колбэки Zigbee выполняются в задаче стека — долгую работу в них
// делать нельзя. Поэтому колбэк только кладёт запрос в очередь,
// а разбирает её loop(). Один писатель / один читатель — без блокировок.

enum ReqType : uint8_t { REQ_MAIN, REQ_PRESET, REQ_BRIGHT_UP, REQ_BRIGHT_DOWN };

struct Req {
  ReqType type;
  uint8_t idx;    // номер пресета (для REQ_PRESET)
  bool    value;  // запрошенное состояние (для REQ_MAIN)
};

#define QUEUE_SIZE 16
static volatile Req      reqQueue[QUEUE_SIZE];
static volatile uint8_t  qHead = 0;
static volatile uint8_t  qTail = 0;

static void queuePush(ReqType type, uint8_t idx, bool value) {
  uint8_t next = (uint8_t)((qHead + 1) % QUEUE_SIZE);
  if (next == qTail) return;               // очередь переполнена — теряем запрос
  reqQueue[qHead].type  = type;
  reqQueue[qHead].idx   = idx;
  reqQueue[qHead].value = value;
  qHead = next;
}

static bool queuePop(Req &out) {
  if (qTail == qHead) return false;
  out.type  = reqQueue[qTail].type;
  out.idx   = reqQueue[qTail].idx;
  out.value = reqQueue[qTail].value;
  qTail = (uint8_t)((qTail + 1) % QUEUE_SIZE);
  return true;
}

// ============================================================
//   СОСТОЯНИЕ
// ============================================================

static bool     stripOn      = false;
static int8_t   curPreset    = -1;              // -1 = ещё ни разу не выбирали
static uint8_t  curBrightness = BRIGHT_DEFAULT;
static bool     zbOnline     = false;

static unsigned long btnResetAt[PRESET_COUNT] = {0};
static unsigned long brightUpResetAt          = 0;
static unsigned long brightDownResetAt        = 0;

/*
 * Защита от обратной петли.
 *
 * setLight() в библиотеке устроен так, что СНАЧАЛА зовёт наш колбэк,
 * и только потом пишет атрибут. Каждый наш доклад состояния выглядит
 * для программы как «пришла команда из Home Assistant». Без защиты —
 * бесконечный цикл. Колбэк вызывается синхронно, поэтому достаточно
 * поднять флаг на время вызова.
 */
static volatile bool selfReport = false;

static void reportLight(ZigbeeLight *ep, bool value) {
  selfReport = true;
  ep->setLight(value);
  selfReport = false;
}

// ============================================================
//   ЛЕНТА
// ============================================================

static void stripShowOff() {
  strip.clear();
  strip.show();
}

static void stripShowPreset(uint8_t idx) {
  if (idx >= PRESET_COUNT) return;
  const PresetDef &p = PRESETS[idx];
  strip.setBrightness(curBrightness);
  strip.fill(strip.Color(p.r, p.g, p.b));
  strip.show();
}

// Применить текущее состояние (stripOn/curPreset/curBrightness) к ленте
static void applyStrip() {
  if (!stripOn) { stripShowOff(); return; }
  if (curPreset < 0) curPreset = PRESET_DEFAULT_ON;
  stripShowPreset((uint8_t)curPreset);
}

// ============================================================
//   ЛОГИКА
// ============================================================

static void applyMain(bool wanted) {
  stripOn = wanted;
  applyStrip();
  reportLight(&zbMain, stripOn);
  Serial.printf("[MAIN] %s\n", stripOn ? "ВКЛ" : "ВЫКЛ");
}

static void applyPreset(uint8_t idx) {
  if (idx >= PRESET_COUNT) return;
  curPreset = (int8_t)idx;
  stripOn   = true;
  applyStrip();
  reportLight(&zbMain, true);
  btnResetAt[idx] = millis() + BUTTON_HOLD_MS;
  Serial.printf("[PRESET] %-14s  RGB(%3u,%3u,%3u)  yarkost %u\n",
                PRESETS[idx].name, PRESETS[idx].r, PRESETS[idx].g,
                PRESETS[idx].b, curBrightness);
}

static void applyBrightStep(int16_t delta) {
  int16_t v = (int16_t)curBrightness + delta;
  if (v < BRIGHT_MIN) v = BRIGHT_MIN;
  if (v > BRIGHT_MAX) v = BRIGHT_MAX;
  curBrightness = (uint8_t)v;

  stripOn = true;                 // регулировка яркости сама включает ленту
  applyStrip();
  reportLight(&zbMain, true);
  Serial.printf("[BRIGHT] %u\n", curBrightness);
}

// ============================================================
//   КОЛБЭКИ ZIGBEE
// ============================================================

static void onMainChange(bool value) {
  if (selfReport) return;                  // это эхо нашего же доклада
  queuePush(REQ_MAIN, 0, value);
}

static void onBrightUp(bool v) {
  if (selfReport) return;
  if (!v) return;                          // гашение — не команда
  queuePush(REQ_BRIGHT_UP, 0, true);
}

static void onBrightDown(bool v) {
  if (selfReport) return;
  if (!v) return;
  queuePush(REQ_BRIGHT_DOWN, 0, true);
}

/*
 * Библиотека принимает только указатель на функцию — передать в неё
 * номер пресета нельзя. Поэтому макросом штампуем N одинаковых
 * функций, каждая знает свой номер (как в прошивке магнитолы).
 */
#define MAKE_PRESET_CB(N)                       \
  static void onPreset##N(bool v) {             \
    if (selfReport) return;                     \
    if (!v) return;                             \
    queuePush(REQ_PRESET, N, true);             \
  }

MAKE_PRESET_CB(0) MAKE_PRESET_CB(1) MAKE_PRESET_CB(2) MAKE_PRESET_CB(3)
MAKE_PRESET_CB(4) MAKE_PRESET_CB(5) MAKE_PRESET_CB(6) MAKE_PRESET_CB(7)
MAKE_PRESET_CB(8) MAKE_PRESET_CB(9)

typedef void (*BtnCb)(bool);

static const BtnCb PRESET_CB[PRESET_COUNT] = {
  onPreset0, onPreset1, onPreset2, onPreset3, onPreset4,
  onPreset5, onPreset6, onPreset7, onPreset8, onPreset9,
};
// Если добавишь пресетов больше 10 — допиши MAKE_PRESET_CB(10) и т.д.
// и добавь их в этот массив.

// ============================================================
//   ОТЛАДОЧНОЕ МЕНЮ
// ============================================================

static char keyFor(uint8_t idx) {
  return (idx < 9) ? (char)('1' + idx) : (char)('a' + (idx - 9));
}

static void printHelp() {
  Serial.println();
  Serial.println("=================== МОНИТОР ПОРТА ===================");
  Serial.println("   0    главный выключатель (переключить)");
  for (uint8_t i = 0; i < PRESET_COUNT; i++) {
    Serial.printf("   %c    %-14s  RGB(%3u,%3u,%3u)\n",
                  keyFor(i), PRESETS[i].name,
                  PRESETS[i].r, PRESETS[i].g, PRESETS[i].b);
  }
  Serial.println("-----------------------------------------------------");
  Serial.println("   +    ярче        -    тише");
  Serial.println("   T    прогнать все пресеты подряд (проверка ленты)");
  Serial.println("   S    статус");
  Serial.println("   X    СБРОС сети Zigbee (заново добавлять в HA!)");
  Serial.println("   ?    справка");
  Serial.println("=====================================================");
  Serial.println();
}

static void printStatus() {
  Serial.println();
  Serial.printf("  Лента          : %s\n", stripOn ? "ВКЛЮЧЕНА" : "выключена");
  Serial.printf("  Текущий пресет : %s\n",
                curPreset >= 0 ? PRESETS[curPreset].name : "(ещё не выбран)");
  Serial.printf("  Яркость        : %u / 255\n", curBrightness);
  Serial.printf("  NUM_LEDS       : %u\n", NUM_LEDS);
  Serial.printf("  Zigbee         : %s\n",
                Zigbee.connected() ? "в сети" : "НЕ ПОДКЛЮЧЕН");
  Serial.println();
}

static void testAllPresets() {
  Serial.println("\n>>> Прогон всех пресетов, пауза 1.5 сек. Смотри на ленту.\n");
  bool wasOn   = stripOn;
  int8_t wasP  = curPreset;
  for (uint8_t i = 0; i < PRESET_COUNT; i++) {
    Serial.printf("    [%2u/%2u] %s\n", i + 1, PRESET_COUNT, PRESETS[i].name);
    stripOn = true;
    stripShowPreset(i);
    delay(1500);
  }
  stripOn   = wasOn;
  curPreset = wasP;
  applyStrip();
  Serial.println(">>> Прогон закончен.\n");
}

static void handleKey(char c) {
  if (c == '\n' || c == '\r' || c == ' ') return;

  if (c == '0') { applyMain(!stripOn); return; }

  for (uint8_t i = 0; i < PRESET_COUNT; i++) {
    if (c == keyFor(i)) { applyPreset(i); return; }
  }

  switch (c) {
    case '+':
      applyBrightStep(+BRIGHT_STEP);
      brightUpResetAt = millis() + BUTTON_HOLD_MS;
      break;
    case '-':
      applyBrightStep(-BRIGHT_STEP);
      brightDownResetAt = millis() + BUTTON_HOLD_MS;
      break;
    case 'T': testAllPresets(); break;
    case 'S': printStatus();    break;

    case 'X':
      Serial.println("\n!!! Сброс сети Zigbee. Устройство перезагрузится.");
      Serial.println("!!! Заново добавь его в Home Assistant.\n");
      delay(500);
      Zigbee.factoryReset();
      break;

    case '?':
    case 'h':
    case 'H': printHelp(); break;

    default:
      Serial.printf("Неизвестная клавиша '%c'. Нажми ? для справки.\n", c);
  }
}

// ============================================================
//   SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("#####################################################");
  Serial.println("#   KORONA   —   ARGB-lenta, Zigbee Router          #");
  Serial.println("#####################################################");

  strip.begin();
  stripShowOff();
  Serial.printf("[LED] GPIO%d, %d адресов (NeoPixel/RMT)\n", PIN_DATA, NUM_LEDS);

  // --- эндпоинты ---
  zbMain.setManufacturerAndModel("DIY", "Korona ARGB");
  zbMain.setPowerSource(ZB_POWER_SOURCE_MAINS);
  zbMain.onLightChange(onMainChange);
  Zigbee.addEndpoint(&zbMain);

  for (uint8_t i = 0; i < PRESET_COUNT; i++) {
    zbPreset[i] = new ZigbeeLight((uint8_t)(EP_PRESET_FIRST + i));
    zbPreset[i]->setManufacturerAndModel("DIY", PRESETS[i].name);
    zbPreset[i]->setPowerSource(ZB_POWER_SOURCE_MAINS);
    zbPreset[i]->onLightChange(PRESET_CB[i]);
    Zigbee.addEndpoint(zbPreset[i]);
  }

  zbBrightUp.setManufacturerAndModel("DIY", "Korona yarche");
  zbBrightUp.setPowerSource(ZB_POWER_SOURCE_MAINS);
  zbBrightUp.onLightChange(onBrightUp);
  Zigbee.addEndpoint(&zbBrightUp);

  zbBrightDown.setManufacturerAndModel("DIY", "Korona tishe");
  zbBrightDown.setPowerSource(ZB_POWER_SOURCE_MAINS);
  zbBrightDown.onLightChange(onBrightDown);
  Zigbee.addEndpoint(&zbBrightDown);

  Serial.printf("[ZB]  endpoint %d — главный;  %d..%d — %u пресетов;  %d/%d — ярче/тише\n",
                EP_MAIN, EP_PRESET_FIRST, EP_PRESET_FIRST + PRESET_COUNT - 1,
                PRESET_COUNT, EP_BRIGHT_UP, EP_BRIGHT_DOWN);

  // --- старт стека ---
  Serial.println("[ZB]  запуск в роли Router...");
  if (!Zigbee.begin(ZIGBEE_ROUTER)) {
    Serial.println("[ZB]  стек не стартовал.");
    Serial.println("[ZB]  Проверь zigbee.csv и флаг -DZIGBEE_MODE_ZCZR.");
  } else {
    Serial.println("[ZB]  стек запущен, ищем сеть...");
  }

  printHelp();
  printStatus();
}

// ============================================================
//   LOOP
// ============================================================

void loop() {
  unsigned long now = millis();

  // ---- 1. следим за подключением к сети ----
  {
    bool online = Zigbee.connected();
    if (online != zbOnline) {
      zbOnline = online;
      if (online) {
        Serial.println("[ZB]  подключились к сети");
        reportLight(&zbMain, stripOn);
        reportLight(&zbBrightUp, false);
        reportLight(&zbBrightDown, false);
        for (uint8_t i = 0; i < PRESET_COUNT; i++) reportLight(zbPreset[i], false);
      } else {
        Serial.println("[ZB]  связь с сетью потеряна");
      }
    }
  }

  static unsigned long lastHint = 0;
  if (!zbOnline && now - lastHint > ZB_JOIN_HINT_MS) {
    lastHint = now;
    Serial.println("[ZB]  нет сети. Открой в Zigbee2MQTT режим сопряжения");
    Serial.println("[ZB]  (permit join). Если не помогает — клавиша X.");
  }

  // ---- 2. разбираем очередь запросов из Home Assistant ----
  Req r;
  while (queuePop(r)) {
    switch (r.type) {
      case REQ_MAIN:
        applyMain(r.value);
        break;
      case REQ_PRESET:
        applyPreset(r.idx);
        break;
      case REQ_BRIGHT_UP:
        applyBrightStep(+BRIGHT_STEP);
        brightUpResetAt = now + BUTTON_HOLD_MS;
        break;
      case REQ_BRIGHT_DOWN:
        applyBrightStep(-BRIGHT_STEP);
        brightDownResetAt = now + BUTTON_HOLD_MS;
        break;
    }
  }

  // ---- 3. гасим кнопки, которые отработали (пресеты + ярче/тише) ----
  for (uint8_t i = 0; i < PRESET_COUNT; i++) {
    if (btnResetAt[i] && (long)(now - btnResetAt[i]) >= 0) {
      btnResetAt[i] = 0;
      reportLight(zbPreset[i], false);
    }
  }
  if (brightUpResetAt && (long)(now - brightUpResetAt) >= 0) {
    brightUpResetAt = 0;
    reportLight(&zbBrightUp, false);
  }
  if (brightDownResetAt && (long)(now - brightDownResetAt) >= 0) {
    brightDownResetAt = 0;
    reportLight(&zbBrightDown, false);
  }

  // ---- 4. монитор порта ----
  while (Serial.available()) handleKey((char)Serial.read());

  delay(5);
}
