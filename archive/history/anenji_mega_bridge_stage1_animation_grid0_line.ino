#include <RGBmatrixPanel.h>
#include <avr/wdt.h>

// ============================================================
// RGB MATRIX 64x32
// Статический экран. Геометрия исходного шаблона сохранена.
// ============================================================

#define CLK 11
#define OE   9
#define LAT  10
#define A    A0
#define B    A1
#define C    A2
#define D    A3

RGBmatrixPanel matrix(A, B, C, D, CLK, LAT, OE, false, 64);


// ============================================================
// DEBUG
// ============================================================

#define DEBUG_ESP 1

#if DEBUG_ESP
#define LOG(x) Serial.print(x)
#define LOGLN(x) Serial.println(x)
#else
#define LOG(x)
#define LOGLN(x)
#endif


// ============================================================
// ЯРКОСТЬ
// ============================================================

uint8_t PANEL_BRIGHTNESS = 4;


// ============================================================
// SOC / ETA
// ============================================================

// SOC обновляется из компактного поля c. Начальное значение до первого ответа.
int batterySoc = -1;   // неизвестно до первого корректного /api/rgb

// SOC становится красным при <= 20%.
const uint8_t BAT_SOC_RED_PCT = 20;

// ETA становится красным при <= 30 минут.
const uint32_t BAT_ETA_RED_SEC = 30UL * 60UL;


// ============================================================
// BUZZER / attention_required
//
// Предполагается АКТИВНЫЙ буззер.
// Pin 8 свободен в текущей разводке.
// При attention_required=1: короткий писк каждые 5 секунд.
// ============================================================

#define BUZZER_PIN 8
#define BUZZER_ACTIVE_HIGH 1

const uint32_t BUZZER_PERIOD_MS = 5000;
const uint16_t BUZZER_BEEP_MS = 140;

bool attentionRequired = false;
bool buzzerOn = false;
uint32_t buzzerCycleStarted = 0;


// ============================================================
// ДАННЫЕ /api/rgb
//
// Короткие поля:
// p = PV W
// l = LOAD W
// g = GRID W
// b = BAT W (+ discharge, - charge)
// v = battery V
// c = SOC %
// d = battery_discharging 0/1
// a = attention_required 0/1
// e = eta_to_reserve_sec, -1 если нет
// s = battery state: -1 charge, 0 idle, 1 discharge
// ============================================================

long pvPower = 0;
long gridPower = 0;
long loadPower = 0;
long batteryPower = 0;
bool telemetryValid = false;

float batteryVoltage = 0.0;
bool batteryDischarging = false;
long etaToReserveSec = -1;
int8_t batteryState = 0;


String currentTimeText = "--:--";

enum ScreenMode {
  SCREEN_ENERGY = 0,
  SCREEN_INFO = 1
};

ScreenMode currentScreen = SCREEN_ENERGY;

const uint32_t ENERGY_SCREEN_MS = 40000UL;
const uint32_t INFO_SCREEN_MS = 20000UL;

uint32_t screenStartedAt = 0;

// Мигание двоеточия часов.
// Перерисовывается только символ ":" раз в секунду.
bool clockColonVisible = true;
uint32_t clockColonChangedAt = 0;


// ============================================================
// ЛЕГКАЯ АНИМАЦИЯ ЭНЕРГОПОТОКОВ
//
// Работает ТОЛЬКО на SCREEN_ENERGY.
// 8 FPS — заметно легче исходных 15 FPS.
// Во время HTTP вызова loop() кратко блокируется, поэтому точки просто
// замирают на время запроса; refresh HUB75 продолжает работать.
// ============================================================

#define ENERGY_ANIM_FPS 8  // disabled: updateEnergyAnimation() is not called

uint32_t prevEnergyAnimAt = 0;
uint16_t energyAnimPhase = 0;

// Разрядная стрелка АКБ: обычная 1 px, мигает каждые 250 мс.
const uint16_t BAT_DISCHARGE_BLINK_MS = 250;
bool batteryDischargeArrowVisible = true;
uint32_t batteryDischargeBlinkAt = 0;

// Легкая анимация потоков, 8 FPS.
const uint8_t BRIDGE_FLOW_FPS = 8;
uint32_t bridgeFlowAt = 0;
uint16_t bridgeFlowPhase = 0;



// ============================================================
// ESP8266 BRIDGE — STAGE 1
//
// ESP8266 больше не AT-модем.
// Wi-Fi/HTTP работают на ESP, Mega получает только готовые DATA-пакеты.
// Wi-Fi/RMS настройки Mega передает ESP при старте и по REQCFG.
// ============================================================

HardwareSerial &ESP = Serial3;

const unsigned long ESP_BAUD = 19200;

const char RMS_WIFI_SSID[] = "ap";
const char RMS_WIFI_PASS[] = "<WIFI_PASSWORD>";
const char RMS_HOST[] = "192.168.54.30";
const uint16_t RMS_HTTP_PORT = 8088;
const char RMS_RGB_PATH[] = "/api/rgb";

const uint32_t ESP_RGB_POLL_MS = 5000UL;
const uint32_t ESP_CONFIG_REPEAT_MS = 15000UL;

String espRxLine;
uint32_t lastEspConfigAt = 0;
uint32_t lastEspDataAt = 0;
bool espBridgeConfigured = false;


// ============================================================
// ЦВЕТА
// ============================================================

uint16_t BLACK;
uint16_t WHITE;
uint16_t GREEN;
uint16_t GREY;
uint16_t YELLOW;
uint16_t BLUE;
uint16_t RED;
uint16_t FLOW_DARK;


// ============================================================
// ЦВЕТА
// ============================================================

void updateColors() {

  uint8_t b = constrain(PANEL_BRIGHTNESS, 0, 7);

  BLACK = matrix.Color333(0, 0, 0);
  WHITE = matrix.Color333(b, b, b);
  GREEN = matrix.Color333(0, b, 0);
  RED   = matrix.Color333(b, 0, 0);
  BLUE  = matrix.Color333(0, 0, b);

  // Темная точка потока: заметна на зеленой/серой линии,
  // но не превращается в яркий белый "бегунок".
  uint8_t flowLevel = (b >= 3) ? 1 : 0;
  FLOW_DARK = matrix.Color333(0, flowLevel, 0);

  uint8_t yellowGreen = (b * 5) / 7;
  if (b > 0 && yellowGreen == 0) yellowGreen = 1;
  YELLOW = matrix.Color333(b, yellowGreen, 0);

  uint8_t grey = b / 3;
  if (b > 0 && grey == 0) grey = 1;
  GREY = matrix.Color333(grey, grey, grey);
}


// ============================================================
// МОЩНОСТЬ кВт
// ============================================================

void printPowerKW(int x, int y, long watts, uint16_t color) {

  bool negative = (watts < 0);
  if (negative) watts = -watts;

  unsigned long tenths = watts / 100;
  unsigned long whole = tenths / 10;
  unsigned long decimal = tenths % 10;

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(color);
  matrix.setCursor(x, y);

  if (negative) matrix.print("-");

  matrix.print(whole);
  matrix.print(".");
  matrix.print(decimal);
}


// ============================================================
// ETA
// ============================================================

void printETA(int x, int y, long seconds, uint16_t color) {

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(color);
  matrix.setCursor(x, y);

  if (seconds < 0) return;

  unsigned long totalMinutes = (unsigned long)seconds / 60UL;
  unsigned int hours = totalMinutes / 60UL;
  uint8_t minutes = totalMinutes % 60UL;

  if (hours > 99) {
    matrix.print(">99h");
    return;
  }

  matrix.print(hours);
  matrix.print(":");

  if (minutes < 10) matrix.print("0");

  matrix.print(minutes);
}


// ============================================================
// ИКОНКИ
// ============================================================

void drawSun(int x, int y) {

  matrix.fillCircle(x, y, 2, YELLOW);

  matrix.drawPixel(x, y - 4, YELLOW);
  matrix.drawPixel(x, y + 4, YELLOW);
  matrix.drawPixel(x - 4, y, YELLOW);
  matrix.drawPixel(x + 4, y, YELLOW);

  matrix.drawPixel(x - 3, y - 3, YELLOW);
  matrix.drawPixel(x + 3, y - 3, YELLOW);
  matrix.drawPixel(x - 3, y + 3, YELLOW);
  matrix.drawPixel(x + 3, y + 3, YELLOW);
}

void drawGridIcon(int x, int y) {

  // Увеличенная молния, но вся остается в колонках x..x+3.
  // При вызове drawGridIcon(0, 11) занимает x=0..3, поэтому
  // отрицательная мощность GRID может безопасно начинаться с x=4.
  matrix.drawLine(x + 3, y, x, y + 5, YELLOW);
  matrix.drawLine(x, y + 5, x + 3, y + 5, YELLOW);
  matrix.drawLine(x + 3, y + 5, x + 1, y + 10, YELLOW);
  matrix.drawPixel(x + 2, y + 5, YELLOW);
}

void drawInverter(int x, int y) {

  matrix.drawRect(x, y, 7, 7, BLUE);
}

void drawHouse(int x, int y) {

  matrix.drawRect(x + 1, y + 4, 7, 4, WHITE);
  matrix.drawLine(x, y + 4, x + 4, y, RED);
  matrix.drawLine(x + 4, y, x + 8, y + 4, RED);
}


// ============================================================
// БАТАРЕЯ
//
// Контур:
//   discharge = RED
//   idle      = GREY
//   charge    = GREEN
// ============================================================

uint16_t batteryOutlineColor() {

  if (batteryState > 0) return RED;
  if (batteryState < 0) return GREEN;
  return GREY;
}

void drawBatterySigns(int x, int y) {

  matrix.drawPixel(x + 2, y + 2, WHITE);
  matrix.drawPixel(x + 3, y + 2, WHITE);
  matrix.drawPixel(x + 4, y + 2, WHITE);
  matrix.drawPixel(x + 3, y + 1, WHITE);
  matrix.drawPixel(x + 3, y + 3, WHITE);

  matrix.drawPixel(x + 7, y + 2, BLUE);
  matrix.drawPixel(x + 8, y + 2, BLUE);
  matrix.drawPixel(x + 9, y + 2, BLUE);
}

void drawBattery(int x, int y) {

  matrix.fillRect(x, y, 12, 6, BLACK);
  matrix.drawRect(x, y, 12, 6, batteryOutlineColor());
  drawBatterySigns(x, y);
}


// ============================================================
// СТРЕЛКИ
// ============================================================

void arrowRight(int x1, int y, int x2, uint16_t color) {

  matrix.drawLine(x1, y, x2, y, color);
  matrix.drawPixel(x2 - 1, y - 1, color);
  matrix.drawPixel(x2 - 1, y + 1, color);
}

void arrowLeft(int x1, int y, int x2, uint16_t color) {

  matrix.drawLine(x1, y, x2, y, color);
  matrix.drawPixel(x1 + 1, y - 1, color);
  matrix.drawPixel(x1 + 1, y + 1, color);
}

void arrowDown(int x, int y1, int y2, uint16_t color) {

  matrix.drawLine(x, y1, x, y2, color);
  matrix.drawPixel(x - 1, y2 - 1, color);
  matrix.drawPixel(x + 1, y2 - 1, color);
}

void arrowUp(int x, int y1, int y2, uint16_t color) {

  matrix.drawLine(x, y1, x, y2, color);
  matrix.drawPixel(x - 1, y1 + 1, color);
  matrix.drawPixel(x + 1, y1 + 1, color);
}


// Обычная однопиксельная стрелка разряда BAT -> INV.
// Мигание сохраняется, но сама форма такая же, как обычная arrowUp().
void drawBatteryDischargeArrow(bool visible) {

  matrix.fillRect(29, 21, 5, 4, BLACK);

  if (!visible) return;

  arrowUp(31, 21, 24, RED);
}


void updateBatteryDischargeBlink() {

  if (currentScreen != SCREEN_ENERGY) return;
  if (!telemetryValid) return;
  if (batteryState <= 0) return;

  uint32_t now = millis();

  if (now - batteryDischargeBlinkAt < BAT_DISCHARGE_BLINK_MS) return;

  batteryDischargeBlinkAt = now;
  batteryDischargeArrowVisible = !batteryDischargeArrowVisible;
  drawBatteryDischargeArrow(batteryDischargeArrowVisible);
}


// ============================================================
// АНИМАЦИЯ ТОЧЕК НА СТАТИЧЕСКИХ ЛИНИЯХ
// ============================================================

void flowDotHorizontal(int x1, int x2, int y, bool reverse, uint16_t phase, uint16_t color) {

  int len = x2 - x1 + 1;
  if (len <= 0) return;

  int p = phase % len;

  if (reverse) p = len - 1 - p;

  matrix.drawPixel(x1 + p, y, color);
}


void flowDotVertical(int x, int y1, int y2, bool reverse, uint16_t phase, uint16_t color) {

  int len = y2 - y1 + 1;
  if (len <= 0) return;

  int p = phase % len;

  if (reverse) p = len - 1 - p;

  matrix.drawPixel(x, y1 + p, color);
}


// Восстановить центральные линии после предыдущей движущейся точки.
// Стрелочные наконечники не трогаем.
void restoreEnergyFlowLines() {

  uint16_t pvColor = (abs(pvPower) > 10) ? GREEN : GREY;
  uint16_t gridColor = (abs(gridPower) > 10) ? GREEN : GREY;
  uint16_t loadColor = (loadPower > 10) ? GREEN : GREY;

  matrix.drawLine(31, 7, 31, 11, pvColor);
  matrix.drawLine(9, 16, 26, 16, gridColor);
  matrix.drawLine(36, 16, 53, 16, loadColor);

  if (batteryState > 0) {
    // Разрядную стрелку рисует updateBatteryDischargeBlink().
  } else if (batteryState < 0) {
    matrix.drawLine(31, 21, 31, 24, GREEN);
  } else {
    matrix.drawLine(31, 21, 31, 24, GREY);
  }
}


void drawEnergyFlowAnimation() {

  // PV -> inverter
  if (pvPower > 10) {
    flowDotVertical(31, 7, 11, false, energyAnimPhase, FLOW_DARK);
  }

  // GRID <-> inverter
  if (abs(gridPower) > 10) {
    bool reverseGrid = (gridPower < 0);
    flowDotHorizontal(9, 26, 16, reverseGrid, energyAnimPhase, FLOW_DARK);
  }

  // inverter -> load
  if (loadPower > 10) {
    flowDotHorizontal(36, 53, 16, false, energyAnimPhase, FLOW_DARK);
  }

  // battery -> inverter при разряде НЕ анимируем точкой:
  // обычная красная стрелка мигает отдельно.
  if (batteryState < 0) {
    // INV -> BAT при зарядке: темная точка вниз.
    flowDotVertical(31, 21, 24, false, energyAnimPhase, FLOW_DARK);
  }
}


void updateEnergyAnimation() {

  if (currentScreen != SCREEN_ENERGY) return;
  if (!telemetryValid) return;

  uint32_t now = millis();

  if (now - prevEnergyAnimAt < (1000UL / ENERGY_ANIM_FPS)) return;

  prevEnergyAnimAt = now;

  restoreEnergyFlowLines();

  energyAnimPhase++;

  drawEnergyFlowAnimation();
}


// ============================================================
// АНИМАЦИЯ НАПРАВЛЕНИЯ ПОТОКОВ — STAGE 1
// ============================================================

void bridgeFlowH(int x1, int x2, int y, bool reverse, uint16_t phase) {

  int len = x2 - x1 + 1;
  if (len <= 0) return;

  int p = phase % len;
  if (reverse) p = len - 1 - p;

  matrix.drawPixel(x1 + p, y, FLOW_DARK);
}


void bridgeFlowV(int x, int y1, int y2, bool reverse, uint16_t phase) {

  int len = y2 - y1 + 1;
  if (len <= 0) return;

  int p = phase % len;
  if (reverse) p = len - 1 - p;

  matrix.drawPixel(x, y1 + p, FLOW_DARK);
}


void restoreBridgeFlowLines() {

  uint16_t pvColor = (abs(pvPower) > 10) ? GREEN : GREY;
  uint16_t gridColor = (abs(gridPower) > 10) ? GREEN : GREY;
  uint16_t loadColor = (loadPower > 10) ? GREEN : GREY;

  matrix.drawLine(31, 7, 31, 11, pvColor);
  matrix.drawLine(36, 16, 53, 16, loadColor);

  if (gridPower == 0) matrix.drawLine(9, 16, 26, 16, GREY);
  else matrix.drawLine(9, 16, 26, 16, gridColor);

  if (batteryState < 0) matrix.drawLine(31, 21, 31, 24, GREEN);
  else if (batteryState == 0) matrix.drawLine(31, 21, 31, 24, GREY);

  // При разряде BAT стрелка мигает отдельно.
}


void updateBridgeFlowAnimation() {

  if (currentScreen != SCREEN_ENERGY) return;
  if (!telemetryValid) return;

  uint32_t now = millis();

  if (now - bridgeFlowAt < (1000UL / BRIDGE_FLOW_FPS)) return;

  bridgeFlowAt = now;

  restoreBridgeFlowLines();
  bridgeFlowPhase++;

  // PV -> inverter
  if (pvPower > 10) bridgeFlowV(31, 7, 11, false, bridgeFlowPhase);

  // GRID:
  // >0 сеть -> инвертор
  // <0 инвертор -> сеть
  //  0 только серая линия
  if (gridPower > 0) bridgeFlowH(9, 26, 16, false, bridgeFlowPhase);
  else if (gridPower < 0) bridgeFlowH(9, 26, 16, true, bridgeFlowPhase);

  // inverter -> load
  if (loadPower > 10) bridgeFlowH(36, 53, 16, false, bridgeFlowPhase);

  // заряд: inverter -> battery
  if (batteryState < 0) bridgeFlowV(31, 21, 24, false, bridgeFlowPhase);
}


// ============================================================
// SOC / ETA COLORS
// ============================================================

uint16_t batterySocColor() {

  if (batterySoc < 0) return GREY;
  if (batterySoc <= BAT_SOC_RED_PCT) return RED;
  return GREEN;
}

uint16_t batteryEtaColor() {

  if (etaToReserveSec >= 0 && (uint32_t)etaToReserveSec <= BAT_ETA_RED_SEC) return RED;
  return GREEN;
}


// ============================================================
// НЕИЗВЕСТНЫЕ ЗНАЧЕНИЯ / КЭШ ОТОБРАЖЕНИЯ
// ============================================================

void printDashes(int x, int y, uint16_t color) {

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(color);
  matrix.setCursor(x, y);
  matrix.print("--");
}

void printSocUnknown() {

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(GREY);
  matrix.setCursor(1, 25);
  matrix.print("--%");
}

void printEtaUnknown() {

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(GREY);
  matrix.setCursor(39, 25);
  matrix.print("--:--");
}


// Последние реально нарисованные значения.
// Специальные стартовые значения гарантируют первое обновление.
long shownPv = 2147483647L;
long shownGrid = 2147483647L;
long shownLoad = 2147483647L;
int shownSoc = -32768;
long shownEta = -2147483647L;
int8_t shownBatteryState = 99;
bool shownDischarging = false;
bool shownTelemetryValid = false;


// ============================================================
// СТАТИЧЕСКИЙ ЭКРАН ПРИ СТАРТЕ
//
// До первого корректного /api/rgb никаких тестовых чисел:
// отображаются прочерки.
// ============================================================

void drawInitialDashboard() {

  matrix.fillScreen(BLACK);

  drawSun(31, 3);
  printDashes(39, 0, GREY);
  arrowDown(31, 7, 11, GREY);

  drawGridIcon(0, 11);
  printDashes(9, 8, GREY);

  drawInverter(28, 13);
  drawHouse(55, 12);

  matrix.drawLine(9, 16, 26, 16, GREY);
  matrix.drawLine(36, 16, 53, 16, GREY);
  printDashes(36, 18, GREY);

  batteryState = 0;
  drawBattery(25, 26);
  matrix.drawLine(31, 21, 31, 24, GREY);

  printSocUnknown();
  printEtaUnknown();

  shownTelemetryValid = false;
}


// ============================================================
// ЧАСТИЧНОЕ ОБНОВЛЕНИЕ
//
// Каждый участок перерисовывается ТОЛЬКО если его значение/состояние
// действительно поменялось.
// ============================================================

void redrawChangedFields() {

  if (!telemetryValid) return;

  if (!shownTelemetryValid || pvPower != shownPv) {

    matrix.fillRect(38, 0, 26, 7, BLACK);
    printPowerKW(39, 0, pvPower, YELLOW);

    matrix.fillRect(29, 7, 5, 5, BLACK);
    uint16_t pvColor = (abs(pvPower) > 10) ? GREEN : GREY;
    arrowDown(31, 7, 11, pvColor);

    shownPv = pvPower;
  }

  if (!shownTelemetryValid || gridPower != shownGrid) {

    // GRID text zone: x=4..27. Icon is x=0..3, inverter starts at x=28.
    matrix.fillRect(4, 8, 24, 7, BLACK);

    if (gridPower < 0) printPowerKW(4, 8, gridPower, WHITE);
    else printPowerKW(9, 8, gridPower, WHITE);

    matrix.fillRect(9, 15, 18, 3, BLACK);

    uint16_t gridColor = (abs(gridPower) > 10) ? GREEN : GREY;

    if (gridPower == 0) {
      matrix.drawLine(9, 16, 26, 16, GREY);
    } else if (gridPower > 0) {
      arrowRight(9, 16, 26, gridColor);
    } else {
      arrowLeft(9, 16, 26, gridColor);
    }

    shownGrid = gridPower;
  }

  if (!shownTelemetryValid || loadPower != shownLoad) {

    matrix.fillRect(36, 15, 18, 3, BLACK);

    uint16_t loadColor = (loadPower > 10) ? GREEN : GREY;
    arrowRight(36, 16, 53, loadColor);

    // LOAD text zone is limited to x=36..53.
    // House begins at x=55, so its body/roof is never erased.
    matrix.fillRect(36, 8, 18, 7, BLACK);
    printPowerKW(36, 8, loadPower, WHITE);

    shownLoad = loadPower;
  }

  if (!shownTelemetryValid ||
      batteryState != shownBatteryState ||
      batteryDischarging != shownDischarging) {

    matrix.fillRect(29, 21, 5, 4, BLACK);

    if (batteryState > 0) drawBatteryDischargeArrow(batteryDischargeArrowVisible);
    else if (batteryState < 0) arrowDown(31, 21, 24, GREEN);
    else matrix.drawLine(31, 21, 31, 24, GREY);

    drawBattery(25, 26);

    shownBatteryState = batteryState;
    shownDischarging = batteryDischarging;
  }

  if (!shownTelemetryValid || batterySoc != shownSoc) {

    matrix.fillRect(0, 25, 24, 7, BLACK);

    if (batterySoc >= 0) {
      matrix.setTextSize(1);
      matrix.setTextWrap(false);
      matrix.setTextColor(batterySocColor());
      matrix.setCursor(1, 25);
      matrix.print(batterySoc);
      matrix.print("%");
    } else {
      printSocUnknown();
    }

    shownSoc = batterySoc;
  }


  shownTelemetryValid = true;
}


// ============================================================
// DASHBOARD
// ============================================================

void drawDashboard() {

  matrix.fillScreen(BLACK);

  // PV
  drawSun(31, 3);
  printPowerKW(39, 0, pvPower, YELLOW);

  uint16_t pvColor = (abs(pvPower) > 10) ? GREEN : GREY;
  arrowDown(31, 7, 11, pvColor);

  // GRID
  drawGridIcon(1, 12);

  // Отрицательное значение шире. Сдвиг ВЛЕВО.
  if (gridPower < 0) printPowerKW(4, 8, gridPower, WHITE);
  else printPowerKW(9, 8, gridPower, WHITE);

  // INVERTER
  drawInverter(28, 13);

  // HOUSE
  drawHouse(55, 12);

  // GRID <-> INVERTER
  uint16_t gridColor = (abs(gridPower) > 10) ? GREEN : GREY;

  if (gridPower == 0) {
    matrix.drawLine(9, 16, 26, 16, GREY);
  } else if (gridPower > 0) {
    arrowRight(9, 16, 26, gridColor);
  } else {
    arrowLeft(9, 16, 26, gridColor);
  }

  // INVERTER -> HOUSE
  uint16_t loadColor = (loadPower > 10) ? GREEN : GREY;
  printPowerKW(36, 8, loadPower, WHITE);
  arrowRight(36, 16, 53, loadColor);

  // BATTERY
  drawBattery(25, 26);

  if (batteryState > 0) {
    batteryDischargeArrowVisible = true;
    batteryDischargeBlinkAt = millis();
    drawBatteryDischargeArrow(true);
  } else if (batteryState < 0) {
    arrowDown(31, 21, 24, GREEN);
  } else {
    matrix.drawLine(31, 21, 31, 24, GREY);
  }

  // SOC test
  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(batterySocColor());
  matrix.setCursor(1, 25);
  matrix.print(batterySoc);
  matrix.print("%");

}


// ============================================================
// МИНИ-ШРИФТ КИРИЛЛИЦЫ 5x7
// ============================================================

const uint8_t GLYPH_V[7]  = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
const uint8_t GLYPH_R[7]  = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
const uint8_t GLYPH_E[7]  = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
const uint8_t GLYPH_M[7]  = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
const uint8_t GLYPH_YA[7] = {0x0F,0x11,0x11,0x0F,0x05,0x09,0x11};
const uint8_t GLYPH_A[7]  = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
const uint8_t GLYPH_B[7]  = {0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E};
const uint8_t GLYPH_O[7]  = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
const uint8_t GLYPH_T[7]  = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
const uint8_t GLYPH_Y[7]  = {0x11,0x11,0x11,0x0F,0x01,0x01,0x1E};
const uint8_t GLYPH_CH[7] = {0x11,0x11,0x11,0x0F,0x01,0x01,0x01};
const uint8_t GLYPH_I[7]  = {0x11,0x13,0x15,0x19,0x11,0x11,0x11};
const uint8_t GLYPH_N[7]  = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11};

const uint8_t DIGIT_0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
const uint8_t DIGIT_1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
const uint8_t DIGIT_2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
const uint8_t DIGIT_3[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E};
const uint8_t DIGIT_4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
const uint8_t DIGIT_5[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E};
const uint8_t DIGIT_6[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E};
const uint8_t DIGIT_7[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
const uint8_t DIGIT_8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
const uint8_t DIGIT_9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E};

void drawGlyph5x7(int x, int y, const uint8_t *g, uint16_t color) {
  for (uint8_t row = 0; row < 7; row++) {
    for (uint8_t col = 0; col < 5; col++) {
      if (g[row] & (1 << (4 - col))) matrix.drawPixel(x + col, y + row, color);
    }
  }
}

const uint8_t* digitGlyph(char c) {
  switch (c) {
    case '0': return DIGIT_0; case '1': return DIGIT_1; case '2': return DIGIT_2;
    case '3': return DIGIT_3; case '4': return DIGIT_4; case '5': return DIGIT_5;
    case '6': return DIGIT_6; case '7': return DIGIT_7; case '8': return DIGIT_8;
    case '9': return DIGIT_9;
  }
  return NULL;
}

void drawWordVremya(int x, int y, uint16_t color) {
  drawGlyph5x7(x+0,y,GLYPH_V,color); drawGlyph5x7(x+6,y,GLYPH_R,color);
  drawGlyph5x7(x+12,y,GLYPH_E,color); drawGlyph5x7(x+18,y,GLYPH_M,color);
  drawGlyph5x7(x+24,y,GLYPH_YA,color);
}

void drawWordRaboty(int x, int y, uint16_t color) {
  drawGlyph5x7(x+0,y,GLYPH_R,color); drawGlyph5x7(x+6,y,GLYPH_A,color);
  drawGlyph5x7(x+12,y,GLYPH_B,color); drawGlyph5x7(x+18,y,GLYPH_O,color);
  drawGlyph5x7(x+24,y,GLYPH_T,color); drawGlyph5x7(x+30,y,GLYPH_Y,color);
}

void drawEtaRus(int y, uint16_t color) {
  if (etaToReserveSec < 0) {
    matrix.setTextSize(1); matrix.setTextWrap(false); matrix.setTextColor(color);
    matrix.setCursor(4,y); matrix.print("--");
    drawGlyph5x7(18,y,GLYPH_CH,color);
    matrix.setCursor(26,y); matrix.print("--");
    drawGlyph5x7(40,y,GLYPH_M,color); drawGlyph5x7(46,y,GLYPH_I,color); drawGlyph5x7(52,y,GLYPH_N,color);
    return;
  }

  unsigned long totalMinutes = (unsigned long)etaToReserveSec / 60UL;
  unsigned int hours = totalMinutes / 60UL;
  uint8_t minutes = totalMinutes % 60UL;
  if (hours > 99) hours = 99;

  char h1 = (hours >= 10) ? ('0' + hours / 10) : 0;
  char h2 = '0' + hours % 10;
  char m1 = '0' + minutes / 10;
  char m2 = '0' + minutes % 10;

  int x = 2;
  if (h1) { drawGlyph5x7(x,y,digitGlyph(h1),color); x += 6; }
  drawGlyph5x7(x,y,digitGlyph(h2),color); x += 8;
  drawGlyph5x7(x,y,GLYPH_CH,color); x += 8;
  drawGlyph5x7(x,y,digitGlyph(m1),color); x += 6;
  drawGlyph5x7(x,y,digitGlyph(m2),color); x += 8;
  drawGlyph5x7(x,y,GLYPH_M,color); drawGlyph5x7(x+6,y,GLYPH_I,color); drawGlyph5x7(x+12,y,GLYPH_N,color);
}


// ============================================================
// ЭКРАНЫ
// ============================================================



void syncShownEnergyValues() {

  if (!telemetryValid) {
    shownTelemetryValid = false;
    return;
  }

  shownPv = pvPower;
  shownGrid = gridPower;
  shownLoad = loadPower;
  shownSoc = batterySoc;
  shownBatteryState = batteryState;
  shownDischarging = batteryDischarging;
  shownTelemetryValid = true;
}


void drawEnergyScreen() {

  if (!telemetryValid) {

    matrix.fillScreen(BLACK);

    drawSun(31, 3);
    printDashes(39, 0, GREY);
    arrowDown(31, 7, 11, GREY);

    drawGridIcon(0, 11);
    printDashes(9, 8, GREY);

    drawInverter(28, 13);
    drawHouse(55, 12);

    matrix.drawLine(9, 16, 26, 16, GREY);
    printDashes(36, 8, GREY);
    matrix.drawLine(36, 16, 53, 16, GREY);

    batteryState = 0;
    drawBattery(25, 26);
    matrix.drawLine(31, 21, 31, 24, GREY);

    matrix.setTextSize(1);
    matrix.setTextWrap(false);
    matrix.setTextColor(GREY);
    matrix.setCursor(1, 25);
    matrix.print("--%");

    return;
  }

  drawDashboard();
  syncShownEnergyValues();
}


// Нормальный информационный экран: крупные зеленые часы.
//
// TextSize=2:
//   первый символ x=2
//   второй        x=14
//   ":"           x=26
//   минуты        x=38 и x=50
//
// Часы/минуты рисуются один раз.
// В loop() меняется только двоеточие раз в секунду.
//
// GREEN сформирован через updateColors() из PANEL_BRIGHTNESS,
// поэтому яркость часов подчиняется общей яркости панели.
void drawClockColon(bool visible) {

  matrix.setTextWrap(false);
  matrix.setTextSize(2);
  matrix.setCursor(26, 9);
  matrix.setTextColor(visible ? GREEN : BLACK);
  matrix.print(":");
}


void drawClockScreen() {

  matrix.fillScreen(BLACK);

  matrix.setTextWrap(false);
  matrix.setTextSize(2);
  matrix.setTextColor(GREEN);

  if (currentTimeText.length() == 5) {

    matrix.setCursor(2, 9);
    matrix.print(currentTimeText.substring(0, 2));

    matrix.setCursor(38, 9);
    matrix.print(currentTimeText.substring(3, 5));

  } else {

    matrix.setCursor(2, 9);
    matrix.print("--");

    matrix.setCursor(38, 9);
    matrix.print("--");
  }

  clockColonVisible = true;
  clockColonChangedAt = millis();
  drawClockColon(true);
}


void updateClockColon() {

  if (currentScreen != SCREEN_INFO) return;
  if (batteryDischarging) return;

  uint32_t now = millis();

  if (now - clockColonChangedAt >= 1000UL) {

    clockColonChangedAt = now;
    clockColonVisible = !clockColonVisible;

    // Только ":" — никакого fillScreen и перерисовки цифр.
    drawClockColon(clockColonVisible);
  }
}


// Информационный экран при подтвержденном разряде.
// Рамка красная, две короткие ASCII-строки гарантированно помещаются.
//
// BATTERY      = 7 символов ≈ 42 px
// LEFT 12:34   = 10 символов ≈ 60 px
void drawDischargeInfoScreen() {

  matrix.fillScreen(BLACK);
  matrix.drawRect(0, 0, 64, 32, RED);

  uint16_t infoColor = (batterySoc >= 0 && batterySoc <= BAT_SOC_RED_PCT) ? RED : GREEN;

  drawWordVremya(17, 3, infoColor);
  drawWordRaboty(14, 12, infoColor);
  drawEtaRus(22, infoColor);
}


void drawInfoScreen() {

  if (batteryDischarging) drawDischargeInfoScreen();
  else drawClockScreen();
}


void switchToScreen(ScreenMode nextScreen) {
  currentScreen = nextScreen;
  screenStartedAt = millis();

  if (currentScreen == SCREEN_ENERGY) {
    energyAnimPhase = 0;
    prevEnergyAnimAt = millis();
    batteryDischargeArrowVisible = true;
    batteryDischargeBlinkAt = millis();
    drawEnergyScreen();
    syncShownEnergyValues();
  } else {
    drawInfoScreen();
  }
}


// ============================================================
// BUZZER
// ============================================================

void buzzerWrite(bool on) {

#if BUZZER_ACTIVE_HIGH
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
#else
  digitalWrite(BUZZER_PIN, on ? LOW : HIGH);
#endif

  buzzerOn = on;
}

void updateBuzzer() {

  uint32_t now = millis();

  if (!attentionRequired) {
    if (buzzerOn) buzzerWrite(false);
    buzzerCycleStarted = now;
    return;
  }

  uint32_t phase = now - buzzerCycleStarted;

  if (phase >= BUZZER_PERIOD_MS) {
    buzzerCycleStarted = now;
    phase = 0;
  }

  bool shouldBeOn = (phase < BUZZER_BEEP_MS);

  if (shouldBeOn != buzzerOn) buzzerWrite(shouldBeOn);
}


// ============================================================
// ESP8266 BRIDGE UART PROTOCOL
// ============================================================

uint16_t bridgeCrc16(const uint8_t *data, size_t len) {

  uint16_t crc = 0xFFFF;

  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;

    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }

  return crc;
}


void sendEspConfig() {

  String cfg = "CFG|";
  cfg += RMS_WIFI_SSID;
  cfg += "|";
  cfg += RMS_WIFI_PASS;
  cfg += "|";
  cfg += RMS_HOST;
  cfg += "|";
  cfg += String(RMS_HTTP_PORT);
  cfg += "|";
  cfg += RMS_RGB_PATH;
  cfg += "|";
  cfg += String(ESP_RGB_POLL_MS);

  ESP.print(cfg);
  ESP.print('\n');

  lastEspConfigAt = millis();

  LOGLN(F("[ESP] CFG sent"));
}


// DATA|p|l|g|b|v10|soc|d|a|eta|s|HH:MM|crc
bool parseBridgeData(const String &line) {

  if (!line.startsWith("DATA|")) return false;

  int lastSep = line.lastIndexOf('|');
  if (lastSep < 0) return false;

  String payload = line.substring(0, lastSep);
  String crcHex = line.substring(lastSep + 1);

  uint16_t expected = (uint16_t)strtoul(crcHex.c_str(), NULL, 16);
  uint16_t actual = bridgeCrc16((const uint8_t*)payload.c_str(), payload.length());

  if (expected != actual) {
    LOGLN(F("[ESP] DATA CRC error"));
    return false;
  }

  String part[11];
  int start = 5;

  for (uint8_t i = 0; i < 11; i++) {

    int sep = payload.indexOf('|', start);

    if (i == 10) {
      part[i] = payload.substring(start);
      break;
    }

    if (sep < 0) return false;

    part[i] = payload.substring(start, sep);
    start = sep + 1;
  }

  pvPower = part[0].toInt();
  loadPower = part[1].toInt();
  gridPower = part[2].toInt();
  batteryPower = part[3].toInt();
  batteryVoltage = part[4].toInt() / 10.0f;

  int soc = part[5].toInt();
  batterySoc = (soc >= 0 && soc <= 100) ? soc : -1;

  batteryDischarging = (part[6].toInt() != 0);
  attentionRequired = (part[7].toInt() != 0);
  etaToReserveSec = part[8].toInt();
  batteryState = constrain(part[9].toInt(), -1, 1);
  currentTimeText = part[10];

  if (batteryDischarging) batteryState = 1;

  telemetryValid = true;
  lastEspDataAt = millis();

  if (currentScreen == SCREEN_ENERGY) redrawChangedFields();

  LOG(F("[ESP DATA] PV=")); LOG(pvPower);
  LOG(F(" GRID=")); LOG(gridPower);
  LOG(F(" LOAD=")); LOG(loadPower);
  LOG(F(" SOC=")); LOG(batterySoc);
  LOG(F(" S=")); LOG(batteryState);
  LOG(F(" ATT=")); LOGLN(attentionRequired ? 1 : 0);

  return true;
}


void processEspLine(const String &line) {

  if (line == "REQCFG") {
    LOGLN(F("[ESP] REQCFG"));
    sendEspConfig();
    return;
  }

  if (line == "CFGOK") {
    espBridgeConfigured = true;
    LOGLN(F("[ESP] CFG accepted"));
    return;
  }

  if (line.startsWith("DATA|")) {
    parseBridgeData(line);
    return;
  }

  if (line.startsWith("WIFI|") || line.startsWith("ERR|")) {
    LOG(F("[ESP] "));
    LOGLN(line);
  }
}


void serviceEspBridge() {

  while (ESP.available()) {

    char c = (char)ESP.read();

    if (c == '\r') continue;

    if (c == '\n') {

      if (espRxLine.length()) {
        processEspLine(espRxLine);
        espRxLine = "";
      }

    } else {

      if (espRxLine.length() < 220) espRxLine += c;
      else espRxLine = "";
    }
  }

  if (!espBridgeConfigured &&
      millis() - lastEspConfigAt >= ESP_CONFIG_REPEAT_MS) {
    sendEspConfig();
  }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  MCUSR &= ~_BV(WDRF);
  wdt_disable();

  Serial.begin(115200);
  ESP.begin(ESP_BAUD);

  delay(150);

  pinMode(BUZZER_PIN, OUTPUT);
  buzzerWrite(false);

  LOGLN(F(""));
  LOGLN(F("========================================"));
  LOGLN(F(" ANENJI RGB + ESP8266 BRIDGE STAGE 1"));
  LOGLN(F("========================================"));

  matrix.begin();
  updateColors();
  drawEnergyScreen();

  sendEspConfig();

  currentScreen = SCREEN_ENERGY;
  screenStartedAt = millis();

  wdt_enable(WDTO_8S);
  wdt_reset();

  LOGLN(F("[WDT] Enabled: 8 s"));
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  wdt_reset();

  uint32_t now = millis();

  // UART обслуживание быстрое и неблокирующее.
  serviceEspBridge();

  updateBuzzer();
  updateClockColon();
  updateBatteryDischargeBlink();
  updateBridgeFlowAnimation();

  // ENERGY 40 sec -> INFO 20 sec -> ENERGY.
  // Wi-Fi polling выполняется независимо на ESP8266.
  if (currentScreen == SCREEN_ENERGY) {

    if (now - screenStartedAt >= ENERGY_SCREEN_MS) {
      switchToScreen(SCREEN_INFO);
    }

  } else {

    if (now - screenStartedAt >= INFO_SCREEN_MS) {

      // Для часов переключаем экран, когда двоеточие погашено.
      if (batteryDischarging || !clockColonVisible) {
        switchToScreen(SCREEN_ENERGY);
      }
    }
  }

  wdt_reset();
}

