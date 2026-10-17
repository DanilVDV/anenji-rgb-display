#include <RGBmatrixPanel.h>

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

const uint32_t ENERGY_SCREEN_MS = 10000UL;
const uint32_t INFO_SCREEN_MS = 5000UL;

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

#define ENERGY_ANIM_FPS 8

uint32_t prevEnergyAnimAt = 0;
uint16_t energyAnimPhase = 0;



// ============================================================
// ESP8266 AT
//
// ESP после reset стартует на 115200.
// Mega переводит его на 19200 через AT+UART_CUR ДО matrix.begin().
// Постоянная настройка ESP не меняется.
// ============================================================

HardwareSerial &ESP = Serial3;

const unsigned long ESP_BOOT_BAUD = 115200;
const unsigned long ESP_BAUD = 19200;

const char RMS_WIFI_SSID[] = "ap";
const char RMS_WIFI_PASS[] = "<WIFI_PASSWORD>";

const char RMS_HOST[] = "192.168.54.30";
const uint16_t RMS_HTTP_PORT = 8088;
const char RMS_RGB_PATH[] = "/api/rgb";

const uint32_t RMS_POLL_MS = 30000;
const uint32_t WIFI_RETRY_MS = 15000;

const uint16_t ESP_PASSIVE_CHUNK = 64;

uint32_t prevRmsPoll = 0;
uint32_t prevWifiRetry = 0;

bool wifiReady = false;
uint8_t httpFailCount = 0;


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
    matrix.drawLine(31, 21, 31, 24, RED);
  } else if (batteryState < 0) {
    matrix.drawLine(31, 21, 31, 24, GREEN);
  } else {
    matrix.drawLine(31, 21, 31, 24, GREY);
  }
}


void drawEnergyFlowAnimation() {

  // PV -> inverter
  if (pvPower > 10) {
    flowDotVertical(31, 7, 11, false, energyAnimPhase, YELLOW);
  }

  // GRID <-> inverter
  if (abs(gridPower) > 10) {
    bool reverseGrid = (gridPower < 0);
    flowDotHorizontal(9, 26, 16, reverseGrid, energyAnimPhase, WHITE);
  }

  // inverter -> load
  if (loadPower > 10) {
    flowDotHorizontal(36, 53, 16, false, energyAnimPhase, WHITE);
  }

  // battery <-> inverter
  if (batteryState > 0) {
    // BAT -> INV: вверх, красная точка.
    flowDotVertical(31, 21, 24, true, energyAnimPhase, WHITE);
  } else if (batteryState < 0) {
    // INV -> BAT: вниз, зеленая линия с белой точкой.
    flowDotVertical(31, 21, 24, false, energyAnimPhase, WHITE);
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

    if (gridPower >= 0) arrowRight(9, 16, 26, gridColor);
    else arrowLeft(9, 16, 26, gridColor);

    shownGrid = gridPower;
  }

  if (!shownTelemetryValid || loadPower != shownLoad) {

    matrix.fillRect(36, 15, 18, 3, BLACK);

    uint16_t loadColor = (loadPower > 10) ? GREEN : GREY;
    arrowRight(36, 16, 53, loadColor);

    // LOAD text zone is limited to x=36..53.
    // House begins at x=55, so its body/roof is never erased.
    matrix.fillRect(36, 18, 18, 7, BLACK);
    printPowerKW(36, 18, loadPower, WHITE);

    shownLoad = loadPower;
  }

  if (!shownTelemetryValid ||
      batteryState != shownBatteryState ||
      batteryDischarging != shownDischarging) {

    matrix.fillRect(29, 21, 5, 4, BLACK);

    if (batteryState > 0) arrowUp(31, 21, 24, RED);
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

  // ETA зависит не только от числа ETA, но и от состояния разряда.
  long effectiveEta = batteryDischarging ? etaToReserveSec : -1;

  if (!shownTelemetryValid || effectiveEta != shownEta || batteryDischarging != shownDischarging) {

    matrix.fillRect(39, 25, 25, 7, BLACK);

    if (batteryDischarging && etaToReserveSec >= 0) {
      printETA(39, 25, etaToReserveSec, batteryEtaColor());
    } else if (batteryDischarging) {
      printEtaUnknown();
    }

    shownEta = effectiveEta;
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

  if (gridPower >= 0) {
    arrowRight(9, 16, 26, gridColor);
  } else {
    arrowLeft(9, 16, 26, gridColor);
  }

  // INVERTER -> HOUSE
  uint16_t loadColor = (loadPower > 10) ? GREEN : GREY;
  arrowRight(36, 16, 53, loadColor);
  printPowerKW(36, 18, loadPower, WHITE);

  // BATTERY
  drawBattery(25, 26);

  if (batteryState > 0) {
    arrowUp(31, 21, 24, RED);
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

  // ETA только при подтвержденном разряде
  if (batteryDischarging && etaToReserveSec >= 0) {
    printETA(39, 25, etaToReserveSec, batteryEtaColor());
  }
}


// ============================================================
// ЭКРАНЫ
// ============================================================



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
    matrix.drawLine(36, 16, 53, 16, GREY);
    printDashes(36, 18, GREY);

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

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(RED);

  matrix.setCursor(11, 6);
  matrix.print("BATTERY");

  matrix.setCursor(2, 18);

  if (etaToReserveSec >= 0) {

    unsigned long totalMinutes = (unsigned long)etaToReserveSec / 60UL;
    unsigned int hours = totalMinutes / 60UL;
    uint8_t minutes = totalMinutes % 60UL;

    matrix.print("LEFT ");

    if (hours > 99) {
      matrix.print(">99h");
    } else {
      matrix.print(hours);
      matrix.print(":");
      if (minutes < 10) matrix.print("0");
      matrix.print(minutes);
    }

  } else {

    matrix.print("LEFT --:--");
  }
}


void drawInfoScreen() {

  if (batteryDischarging) drawDischargeInfoScreen();
  else drawClockScreen();
}


// Перед сменой экрана пробуем получить свежий компактный пакет.
// Если запрос не удался, предыдущие валидные данные остаются на экране.
void refreshBeforeScreenSwitch() {

  if (!wifiReady) return;

  if (!fetchRmsData()) {
    httpFailCount++;

    LOG(F("[HTTP] switch refresh fail "));
    LOGLN(httpFailCount);

    if (httpFailCount >= 5) {
      wifiReady = false;
      prevWifiRetry = millis();
    }

  } else {

    httpFailCount = 0;
  }
}


void switchToScreen(ScreenMode nextScreen) {

  refreshBeforeScreenSwitch();

  currentScreen = nextScreen;
  screenStartedAt = millis();

  if (currentScreen == SCREEN_ENERGY) {
    energyAnimPhase = 0;
    prevEnergyAnimAt = millis();
    drawEnergyScreen();
    if (telemetryValid) drawEnergyFlowAnimation();
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
// ESP HELPERS
// ============================================================

void espFlushInput() {

  while (ESP.available()) ESP.read();
}

String espCollect(uint32_t timeoutMs, uint16_t quietMs = 100) {

  String r;
  r.reserve(220);

  uint32_t started = millis();
  uint32_t lastByte = started;
  bool gotAny = false;

  while (millis() - started < timeoutMs) {

    while (ESP.available()) {

      char c = (char)ESP.read();
      gotAny = true;
      lastByte = millis();

      if (r.length() < 219) {
        r += c;
      } else {
        r.remove(0, 70);
        r += c;
      }
    }

    if (gotAny && millis() - lastByte >= quietMs) break;
  }

  return r;
}

String espSendCommand(const String &cmd, uint32_t timeoutMs) {

  espFlushInput();

#if DEBUG_ESP
  if (cmd.startsWith("AT+CWJAP=")) {
    LOGLN(F("[ESP TX] AT+CWJAP=<hidden>"));
  } else {
    LOG(F("[ESP TX] "));
    LOGLN(cmd);
  }
#endif

  ESP.print(cmd);
  ESP.print("\r\n");

  return espCollect(timeoutMs);
}

bool responseHas(const String &r, const char *token) {

  return r.indexOf(token) >= 0;
}

bool espProbeAtCurrentBaud() {

  for (uint8_t i = 0; i < 3; i++) {

    String r = espSendCommand("AT", 1000);

    if (responseHas(r, "OK")) return true;

    delay(100);
  }

  return false;
}


// ============================================================
// 115200 -> 19200 ДО matrix.begin()
// ============================================================

bool espSwitchToWorkBaud() {

  ESP.begin(ESP_BOOT_BAUD);
  delay(350);

  if (espProbeAtCurrentBaud()) {

    LOGLN(F("[ESP] Found at 115200, switching to 19200"));

    espSendCommand("AT+UART_CUR=19200,8,1,0,0", 1200);

    delay(100);

    ESP.end();
    delay(80);

    ESP.begin(ESP_BAUD);
    delay(300);

    if (espProbeAtCurrentBaud()) {
      LOGLN(F("[ESP] 19200 confirmed"));
      return true;
    }
  }

  ESP.end();
  delay(80);

  ESP.begin(ESP_BAUD);
  delay(300);

  if (espProbeAtCurrentBaud()) {
    LOGLN(F("[ESP] Already at 19200"));
    return true;
  }

  LOGLN(F("[ESP] UART setup failed"));
  return false;
}


// ============================================================
// Wi-Fi JOIN
// ============================================================

bool wifiConnect() {

  wifiReady = false;

  LOGLN(F("[WIFI] Connecting..."));

  if (!espProbeAtCurrentBaud()) return false;

  espSendCommand("ATE0", 1000);

  String r = espSendCommand("AT+CWMODE=1", 1200);
  if (!responseHas(r, "OK")) {
    if (!espProbeAtCurrentBaud()) return false;
  }

  r = espSendCommand("AT+CIPMUX=0", 1200);
  if (!responseHas(r, "OK")) {
    if (!espProbeAtCurrentBaud()) return false;
  }

  r = espSendCommand("AT+CIPRECVMODE=1", 1200);
  if (!responseHas(r, "OK")) {
    if (!espProbeAtCurrentBaud()) return false;
  }

  String join = "AT+CWJAP=\"";
  join += RMS_WIFI_SSID;
  join += "\",\"";
  join += RMS_WIFI_PASS;
  join += "\"";

  espFlushInput();
  LOGLN(F("[ESP TX] AT+CWJAP=<hidden>"));
  ESP.print(join);
  ESP.print("\r\n");

  String jr;
  jr.reserve(260);

  uint32_t started = millis();

  while (millis() - started < 20000UL) {

    while (ESP.available()) {

      char c = (char)ESP.read();

      if (jr.length() < 259) {
        jr += c;
      } else {
        jr.remove(0, 90);
        jr += c;
      }

      if (jr.indexOf("WIFI GOT IP") >= 0 || jr.indexOf("GOT IP") >= 0) {
        wifiReady = true;
        httpFailCount = 0;
        LOGLN(F("[WIFI] Connected"));
        delay(500);
        espFlushInput();
        return true;
      }

      if (jr.indexOf("+CWJAP:") >= 0 || jr.indexOf("FAIL") >= 0 || jr.indexOf("ERROR") >= 0) {
        LOGLN(F("[WIFI] Join failed"));
        return false;
      }
    }
  }

  LOGLN(F("[WIFI] Join timeout"));
  return false;
}


// ============================================================
// PASSIVE RECEIVE
// ============================================================

long espPassiveAvailable() {

  String r = espSendCommand("AT+CIPRECVLEN?", 900);

  int p = r.indexOf("+CIPRECVLEN:");
  if (p < 0) return -1;

  p += 12;

  String n;

  while (p < (int)r.length() && r[p] >= '0' && r[p] <= '9') {
    n += r[p++];
  }

  if (!n.length()) return -1;

  return n.toInt();
}

bool espPassiveRead(uint16_t requested, String &out) {

  out = "";

  String cmd = "AT+CIPRECVDATA=";
  cmd += String(requested);

  espFlushInput();
  ESP.print(cmd);
  ESP.print("\r\n");

  String r;
  r.reserve(requested + 90);

  uint32_t started = millis();
  uint32_t lastByte = started;
  bool gotAny = false;

  while (millis() - started < 1600) {

    while (ESP.available()) {

      char c = (char)ESP.read();
      gotAny = true;
      lastByte = millis();

      if (r.length() < requested + 80) r += c;
    }

    if (gotAny && millis() - lastByte > 80) break;
  }

  int p = r.indexOf("+CIPRECVDATA,");
  if (p < 0) return false;

  int comma = r.indexOf(',', p);
  int colon = r.indexOf(':', p);

  if (comma < 0 || colon < 0 || comma > colon) return false;

  int actual = r.substring(comma + 1, colon).toInt();
  int dataStart = colon + 1;

  if (actual < 0 || dataStart + actual > (int)r.length()) return false;

  out.reserve(actual);

  for (int i = 0; i < actual; i++) out += r[dataStart + i];

  return true;
}


// ============================================================
// КОРОТКИЙ JSON PARSER
// ============================================================

bool jsonNumber(const String &j, const char *key, double &value) {

  String token = "\"";
  token += key;
  token += "\":";

  int p = j.indexOf(token);
  if (p < 0) return false;

  p += token.length();

  String n;

  while (p < (int)j.length()) {

    char c = j[p++];

    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
      n += c;
    } else {
      break;
    }
  }

  if (!n.length()) return false;

  value = n.toFloat();
  return true;
}


bool jsonString(const String &j, const char *key, String &value) {

  String token = "\"";
  token += key;
  token += "\":\"";

  int p = j.indexOf(token);
  if (p < 0) return false;

  p += token.length();

  int e = j.indexOf('"', p);
  if (e < 0) return false;

  value = j.substring(p, e);
  return true;
}

bool parseRgbJson(const String &j) {

  double p, l, g, b, v, c, d, a, e, s;
  String t;

  if (!jsonNumber(j, "p", p)) return false;
  if (!jsonNumber(j, "l", l)) return false;
  if (!jsonNumber(j, "g", g)) return false;
  if (!jsonNumber(j, "b", b)) return false;
  if (!jsonNumber(j, "v", v)) return false;
  if (!jsonNumber(j, "c", c)) return false;
  if (!jsonNumber(j, "d", d)) return false;
  if (!jsonNumber(j, "a", a)) return false;
  if (!jsonNumber(j, "e", e)) return false;
  if (!jsonString(j, "t", t)) return false;
  if (!jsonNumber(j, "s", s)) return false;

  pvPower = (long)p;
  loadPower = (long)l;
  gridPower = (long)g;
  batteryPower = (long)b;
  batteryVoltage = (float)v;
  batterySoc = (c >= 0.0 && c <= 100.0) ? constrain((int)(c + 0.5), 0, 100) : -1;
  batteryDischarging = (d != 0);
  attentionRequired = (a != 0);
  etaToReserveSec = (long)e;
  currentTimeText = t;
  batteryState = constrain((int)s, -1, 1);

  // Безопасность от противоречия:
  // подтвержденный discharge всегда имеет приоритет.
  if (batteryDischarging) batteryState = 1;

  telemetryValid = true;

  LOG(F("[RMS] PV=")); LOG(pvPower);
  LOG(F(" GRID=")); LOG(gridPower);
  LOG(F(" LOAD=")); LOG(loadPower);
  LOG(F(" BAT=")); LOG(batteryPower);
  LOG(F(" V=")); Serial.print(batteryVoltage, 1);
  LOG(F(" SOC=")); LOG(batterySoc);
  LOG(F(" state=")); LOG(batteryState);
  LOG(F(" DIS=")); LOG(batteryDischarging ? 1 : 0);
  LOG(F(" ATT=")); LOG(attentionRequired ? 1 : 0);
  LOG(F(" ETA=")); LOG(etaToReserveSec);
  LOG(F(" TIME=")); LOGLN(currentTimeText);

  return true;
}


// ============================================================
// HTTP /api/rgb
// ============================================================

bool fetchRmsData() {

  if (!wifiReady) return false;

  // Закрыть старое соединение без ожидания.
  ESP.print("AT+CIPCLOSE\r\n");
  delay(20);
  espFlushInput();

  String cip = "AT+CIPSTART=\"TCP\",\"";
  cip += RMS_HOST;
  cip += "\",";
  cip += String(RMS_HTTP_PORT);

  String cr = espSendCommand(cip, 2200);

  if ((!responseHas(cr, "CONNECT") && !responseHas(cr, "OK")) || responseHas(cr, "ERROR") || responseHas(cr, "FAIL")) {
    return false;
  }

  String req = "GET ";
  req += RMS_RGB_PATH;
  req += " HTTP/1.0\r\nHost: ";
  req += RMS_HOST;
  req += "\r\nConnection: close\r\n\r\n";

  espFlushInput();

  ESP.print("AT+CIPSEND=");
  ESP.print(req.length());
  ESP.print("\r\n");

  String sr = espCollect(1200, 70);

  if (sr.indexOf('>') < 0) return false;

  ESP.print(req);

  // SEND OK может прийти вместе с +IPD. В passive mode payload остается у ESP.
  espCollect(1200, 100);

  // Короткий /api/rgb обычно полностью буферизован за это время.
  delay(80);

  String tcp;
  tcp.reserve(420);

  uint32_t started = millis();
  uint8_t emptyPolls = 0;

  while (millis() - started < 3500) {

    long available = espPassiveAvailable();

    if (available < 0) {
      delay(20);
      continue;
    }

    if (available == 0) {
      emptyPolls++;

      if (tcp.length() && emptyPolls >= 3) break;

      delay(20);
      continue;
    }

    emptyPolls = 0;

    uint16_t want = (available > ESP_PASSIVE_CHUNK) ? ESP_PASSIVE_CHUNK : (uint16_t)available;

    String chunk;

    if (!espPassiveRead(want, chunk)) return false;

    if (tcp.length() + chunk.length() < 420) tcp += chunk;

    // JSON уже целиком получен — дальше читать HTTP не нужно.
    int js = tcp.indexOf('{');
    int je = tcp.indexOf('}', js);

    if (js >= 0 && je > js) {
      String body = tcp.substring(js, je + 1);
      ESP.print("AT+CIPCLOSE\r\n");
      return parseRgbJson(body);
    }
  }

  ESP.print("AT+CIPCLOSE\r\n");
  return false;
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(300);

  pinMode(BUZZER_PIN, OUTPUT);
  buzzerWrite(false);

  LOGLN(F(""));
  LOGLN(F("========================================"));
  LOGLN(F(" ANENJI RGB /api/rgb TEST"));
  LOGLN(F("========================================"));
  LOGLN(F("[RGB] ENERGY 10s / INFO 5s, continuous refresh"));

  bool espReady = espSwitchToWorkBaud();

  // Матрица запускается только после переключения ESP на 19200.
  matrix.begin();
  updateColors();
  drawEnergyScreen();

  if (espReady) {
    if (wifiConnect()) {
      if (fetchRmsData()) {
        httpFailCount = 0;
        drawEnergyScreen();
      }
    }
  }

  currentScreen = SCREEN_ENERGY;
  screenStartedAt = millis();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  uint32_t now = millis();

  updateBuzzer();
  updateClockColon();
  updateEnergyAnimation();

  // ----------------------------------------------------------
  // Смена экранов:
  // ENERGY 10 секунд -> INFO 5 секунд -> ENERGY ...
  //
  // Перед КАЖДОЙ сменой запрашиваем свежий /api/rgb.
  // Во время запроса текущий экран продолжает светиться.
  // ----------------------------------------------------------

  if (currentScreen == SCREEN_ENERGY) {

    if (now - screenStartedAt >= ENERGY_SCREEN_MS) {
      switchToScreen(SCREEN_INFO);
    }

  } else {

    if (now - screenStartedAt >= INFO_SCREEN_MS) {
      switchToScreen(SCREEN_ENERGY);
    }
  }

  // ----------------------------------------------------------
  // Восстановление Wi-Fi
  // ----------------------------------------------------------

  if (!wifiReady && now - prevWifiRetry >= WIFI_RETRY_MS) {

    prevWifiRetry = now;

    if (wifiConnect()) {

      if (fetchRmsData()) {
        httpFailCount = 0;

        // Не меняем активный экран, только перерисовываем его свежими данными.
        if (currentScreen == SCREEN_ENERGY) {
          drawEnergyScreen();
          energyAnimPhase = 0;
          drawEnergyFlowAnimation();
        } else {
          drawInfoScreen();
        }
      }
    }
  }
}

