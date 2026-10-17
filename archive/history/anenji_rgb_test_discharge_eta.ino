#include <RGBmatrixPanel.h>

#define CLK 11
#define OE   9
#define LAT  10
#define A    A0
#define B    A1
#define C    A2
#define D    A3

RGBmatrixPanel matrix(A, B, C, D, CLK, LAT, OE, false, 64);


// ============================================================
// Яркость 0...7
// ============================================================

uint8_t PANEL_BRIGHTNESS = 4;


// ============================================================
// ТЕСТОВЫЕ ДАННЫЕ
//
// batteryPower:
//   > 0 = BAT -> INV  (разряд)
//   < 0 = INV -> BAT  (заряд)
//
// etaToReserveSec:
//   оставшееся время до резервного SOC, секунд
// ============================================================

int pvPower      = 500;
int gridPower    = -940;    // тест отдачи в сеть
int loadPower    = 913;
int batteryPower = 450;     // +450 W = РАЗРЯД

float batteryVoltage = 54.5;
int batterySoc = 78;

bool batteryDischarging = true;
long etaToReserveSec = 3L * 3600L + 30L * 60L;   // 3:30


// ============================================================
// АНИМАЦИЯ
// ============================================================

#define ANIM_FPS 15

uint32_t prevAnimTime = 0;
uint16_t animPhase = 0;
uint16_t slowAnimPhase = 0;
uint8_t slowDivider = 0;


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

uint16_t FLOW_HEAD;
uint16_t FLOW_MID;
uint16_t FLOW_TAIL;

uint16_t BAT_FLOW_HEAD;
uint16_t BAT_FLOW_TAIL;


// ============================================================
// Установка цветов с учетом общей яркости
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

  uint8_t headG = max((int)b - 1, 1);
  uint8_t midG  = max((int)b - 2, 1);
  uint8_t tailG = 1;

  FLOW_HEAD = matrix.Color333(0, headG, 0);
  FLOW_MID  = matrix.Color333(0, midG, 0);
  FLOW_TAIL = matrix.Color333(0, tailG, 0);

  uint8_t batHeadR = max((int)b - 1, 1);
  uint8_t batTailR = 1;

  BAT_FLOW_HEAD = matrix.Color333(batHeadR, 0, 0);
  BAT_FLOW_TAIL = matrix.Color333(batTailR, 0, 0);
}


// ============================================================
// Вывод мощности в кВт
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

  if (seconds < 0) {
    matrix.print("--:--");
    return;
  }

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
// Солнце
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


// ============================================================
// Молния / сеть
// ============================================================

void drawGridIcon(int x, int y) {

  matrix.drawLine(x + 3, y, x, y + 4, YELLOW);
  matrix.drawLine(x, y + 4, x + 3, y + 4, YELLOW);
  matrix.drawLine(x + 3, y + 4, x + 1, y + 8, YELLOW);
}


// ============================================================
// Инвертор
// ============================================================

void drawInverter(int x, int y) {

  matrix.drawRect(x, y, 7, 7, BLUE);
}


// ============================================================
// Дом
// ============================================================

void drawHouse(int x, int y) {

  matrix.drawRect(x + 1, y + 4, 7, 4, WHITE);
  matrix.drawLine(x, y + 4, x + 4, y, RED);
  matrix.drawLine(x + 4, y, x + 8, y + 4, RED);
}


// ============================================================
// Батарея
// ============================================================

void drawBattery(int x, int y, int soc, uint16_t color) {

  (void)soc;

  matrix.drawRect(x, y, 12, 6, color);

  matrix.drawPixel(x + 2, y + 2, RED);
  matrix.drawPixel(x + 3, y + 2, RED);
  matrix.drawPixel(x + 4, y + 2, RED);
  matrix.drawPixel(x + 3, y + 1, RED);
  matrix.drawPixel(x + 3, y + 3, RED);

  matrix.drawPixel(x + 7, y + 2, BLUE);
  matrix.drawPixel(x + 8, y + 2, BLUE);
  matrix.drawPixel(x + 9, y + 2, BLUE);
}


// ============================================================
// СТАТИЧЕСКИЕ СТРЕЛКИ
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
// АНИМИРОВАННЫЙ ПОТОК
// ============================================================

void flowHorizontal(int x1, int x2, int y, bool reverse, uint16_t phase) {

  int len = x2 - x1 + 1;
  if (len <= 0) return;

  int p0 = phase % len;
  int p1 = (p0 + len - 2) % len;

  if (reverse) {
    p0 = len - 1 - p0;
    p1 = len - 1 - p1;
  }

  matrix.drawPixel(x1 + p1, y, FLOW_TAIL);
  matrix.drawPixel(x1 + p0, y, FLOW_HEAD);
}


void flowVertical(int x, int y1, int y2, bool reverse, uint16_t phase) {

  int len = y2 - y1 + 1;
  if (len <= 0) return;

  int p0 = phase % len;
  int p1 = (p0 + len - 1) % len;

  if (reverse) {
    p0 = len - 1 - p0;
    p1 = len - 1 - p1;
  }

  matrix.drawPixel(x, y1 + p1, FLOW_TAIL);
  matrix.drawPixel(x, y1 + p0, FLOW_HEAD);
}


void flowVerticalBattery(int x, int y1, int y2, bool reverse, uint16_t phase) {

  int len = y2 - y1 + 1;
  if (len <= 0) return;

  int p0 = phase % len;
  int p1 = (p0 + len - 1) % len;

  if (reverse) {
    p0 = len - 1 - p0;
    p1 = len - 1 - p1;
  }

  matrix.drawPixel(x, y1 + p1, BAT_FLOW_TAIL);
  matrix.drawPixel(x, y1 + p0, BAT_FLOW_HEAD);
}


// ============================================================
// ОСНОВНОЙ ЭКРАН
// ============================================================

void drawDashboard() {

  matrix.fillScreen(BLACK);

  // ----------------------------------------------------------
  // PV
  // ----------------------------------------------------------

  drawSun(31, 3);
  printPowerKW(39, 0, pvPower, YELLOW);

  uint16_t pvColor = (abs(pvPower) > 10) ? GREEN : GREY;
  arrowDown(31, 7, 11, pvColor);

  // ----------------------------------------------------------
  // GRID
  // ----------------------------------------------------------

  drawGridIcon(1, 12);

  // При отдаче в сеть есть знак "-", поэтому сдвигаем надпись вправо
  if (gridPower < 0) printPowerKW(11, 8, gridPower, WHITE);
  else printPowerKW(9, 8, gridPower, WHITE);

  // ----------------------------------------------------------
  // INVERTER
  // ----------------------------------------------------------

  drawInverter(28, 13);

  // ----------------------------------------------------------
  // HOUSE
  // ----------------------------------------------------------

  drawHouse(55, 12);

  // ----------------------------------------------------------
  // GRID <-> INVERTER
  // ----------------------------------------------------------

  uint16_t gridColor = (abs(gridPower) > 10) ? GREEN : GREY;

  if (gridPower >= 0) {
    arrowRight(9, 16, 26, gridColor);
  } else {
    arrowLeft(9, 16, 26, gridColor);
  }

  // ----------------------------------------------------------
  // INVERTER -> HOUSE
  // ----------------------------------------------------------

  uint16_t loadColor = (loadPower > 10) ? GREEN : GREY;
  arrowRight(36, 16, 53, loadColor);

  printPowerKW(36, 18, loadPower, WHITE);

  // ----------------------------------------------------------
  // BATTERY
  // ----------------------------------------------------------

  bool discharging = (batteryPower > 10);

  uint16_t batteryMainColor = discharging ? RED : GREEN;
  uint16_t batColor = (abs(batteryPower) > 10) ? batteryMainColor : GREY;

  drawBattery(25, 26, batterySoc, discharging ? RED : GREEN);

  // batteryPower > 0 = BAT -> INV = разряд
  // batteryPower < 0 = INV -> BAT = заряд

  if (batteryPower > 10) {
    arrowUp(31, 21, 24, RED);
  } else if (batteryPower < -10) {
    arrowDown(31, 21, 24, GREEN);
  } else {
    matrix.drawLine(31, 21, 31, 24, GREY);
  }

  // ----------------------------------------------------------
  // SOC
  // ----------------------------------------------------------

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(discharging ? RED : GREEN);
  matrix.setCursor(1, 25);

  matrix.print(batterySoc);
  matrix.print("%");

  // ----------------------------------------------------------
  // ETA
  // ----------------------------------------------------------

  if (batteryDischarging && etaToReserveSec >= 0) {
    printETA(39, 25, etaToReserveSec, RED);
  }
}


// ============================================================
// СЛОЙ АНИМАЦИИ
// ============================================================

void drawEnergyAnimation() {

  // PV -> inverter
  if (pvPower > 10) {
    flowVertical(31, 7, 11, false, slowAnimPhase);
  }

  // GRID <-> inverter
  if (abs(gridPower) > 10) {
    bool reverseGrid = (gridPower < 0);
    flowHorizontal(9, 26, 16, reverseGrid, animPhase);
  }

  // inverter -> load
  if (loadPower > 10) {
    flowHorizontal(36, 53, 16, false, animPhase);
  }

  // inverter <-> battery
  if (abs(batteryPower) > 10) {
    if (batteryPower > 0) {
      flowVerticalBattery(31, 21, 24, true, slowAnimPhase);
    } else {
      flowVertical(31, 21, 24, false, slowAnimPhase);
    }
  }
}


// ============================================================
// ВОССТАНОВЛЕНИЕ ТОЛЬКО ЛИНИЙ АНИМАЦИИ
// ============================================================

void restoreFlowLines() {

  uint16_t pvColor   = (abs(pvPower) > 10)   ? GREEN : GREY;
  uint16_t gridColor = (abs(gridPower) > 10) ? GREEN : GREY;
  uint16_t loadColor = (loadPower > 10)      ? GREEN : GREY;

  uint16_t batColor = GREY;
  if (batteryPower > 10) batColor = RED;
  else if (batteryPower < -10) batColor = GREEN;

  matrix.drawLine(31, 7, 31, 11, pvColor);
  matrix.drawLine(9, 16, 26, 16, gridColor);
  matrix.drawLine(36, 16, 53, 16, loadColor);
  matrix.drawLine(31, 21, 31, 24, batColor);
}


// ============================================================
// ЯРКОСТЬ
// ============================================================

void setPanelBrightness(uint8_t value) {

  PANEL_BRIGHTNESS = constrain(value, 0, 7);
  updateColors();
  drawDashboard();
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  matrix.begin();
  updateColors();

  drawDashboard();
  drawEnergyAnimation();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  uint32_t now = millis();

  if (now - prevAnimTime >= (1000UL / ANIM_FPS)) {

    prevAnimTime = now;

    restoreFlowLines();

    animPhase++;

    slowDivider++;
    if (slowDivider >= 3) {
      slowDivider = 0;
      slowAnimPhase++;
    }

    drawEnergyAnimation();
  }
}
