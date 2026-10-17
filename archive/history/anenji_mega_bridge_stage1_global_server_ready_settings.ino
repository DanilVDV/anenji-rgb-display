#include <RGBmatrixPanel.h>
#include <U8g2_for_Adafruit_GFX.h>
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

U8G2_FOR_ADAFRUIT_GFX u8g2;


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
// ГЛОБАЛЬНЫЕ НАСТРОЙКИ ПАНЕЛИ
//
// Эти значения пока являются локальными defaults.
// Mega уже умеет принять будущие серверные поля, если ESP начнет
// добавлять их в конец DATA-пакета:
//
//   dc   = полный обычный цикл ENERGY+CLOCK, секунд
//   ep   = доля ENERGY, процентов
//   ai   = экран "Время работы", секунд
//   br   = яркость панели 0...7
//   bc10 = ток АКБ в десятых ампера: -12 = -1.2 A
//
// Старый DATA-пакет без этих полей полностью совместим.
// ============================================================

uint16_t DISPLAY_CYCLE_SEC = 60;
uint8_t ENERGY_SHARE_PCT = 67;
uint8_t AUTONOMY_INFO_SEC = 5;

uint8_t PANEL_BRIGHTNESS = 4;

// Серверный ток АКБ. Пока поле не пришло, используется старый fallback
// batteryPower / batteryVoltage.
bool batteryCurrentFromServerValid = false;
int16_t batteryCurrentTenthsA = 0;


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
  SCREEN_AUTONOMY = 1,
  SCREEN_CLOCK = 2
};

ScreenMode currentScreen = SCREEN_ENERGY;

uint32_t screenStartedAt = 0;


// ============================================================
// РАСЧЕТ ДЛИТЕЛЬНОСТИ ЭКРАНОВ
//
// AUTONOMY не входит в процентное соотношение ENERGY/CLOCK.
// При разряде он добавляется как отдельный информационный экран.
// ============================================================

uint32_t energyScreenMs() {

  uint16_t cycle = constrain(DISPLAY_CYCLE_SEC, 20, 300);
  uint8_t share = constrain(ENERGY_SHARE_PCT, 10, 95);

  return ((uint32_t)cycle * 1000UL * share) / 100UL;
}


uint32_t clockScreenMs() {

  uint32_t total = (uint32_t)constrain(DISPLAY_CYCLE_SEC, 20, 300) * 1000UL;
  uint32_t energy = energyScreenMs();

  return total - energy;
}


uint32_t autonomyScreenMs() {

  uint8_t seconds = constrain(AUTONOMY_INFO_SEC, 2, 30);
  return (uint32_t)seconds * 1000UL;
}

// Мигание двоеточия больших часов.
bool clockColonVisible = true;
uint32_t clockColonChangedAt = 0;

// Нижняя строка часов при разряде:
// анимированный красный значок АКБ + расчетное время "14ч. 5мин.".
const uint16_t CLOCK_BAT_ANIM_MS = 220;
uint32_t clockBatAnimAt = 0;
uint8_t clockBatAnimPhase = 0;


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

// Анимация зарядки АКБ:
// зеленые вертикальные полосы постепенно заполняют внутреннюю область,
// затем очищаются и цикл повторяется.
const uint16_t BAT_CHARGE_ANIM_MS = 180;
uint32_t batteryChargeAnimAt = 0;
uint8_t batteryChargePhase = 0;

// Анимация разряда АКБ:
// красное заполнение внутренности постепенно УМЕНЬШАЕТСЯ справа налево.
// После опустошения цикл начинается снова с полного прямоугольника.
const uint16_t BAT_DISCHARGE_FILL_MS = 180;
uint32_t batteryDischargeFillAt = 0;
uint8_t batteryDischargeFillPhase = 0;

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
// ОКРУГЛЕНИЕ / СРАВНЕНИЕ МОЩНОСТИ ДЛЯ ЭКРАНА
//
// printPowerKW() реально показывает только десятые доли кВт:
// 244 W  -> 0.2
// 266 W  -> 0.2
// 299 W  -> 0.2
// 300 W  -> 0.3
//
// Поэтому для решения "перерисовывать или нет" сравниваем именно
// отображаемое значение, а не исходные ватты.
// ============================================================

long displayedPowerTenths(long watts) {

  bool negative = (watts < 0);

  if (negative) watts = -watts;

  long tenths = watts / 100L;

  return negative ? -tenths : tenths;
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
// АНИМАЦИЯ ЗАРЯДКИ АКБ
//
// Внутренняя область батареи:
//   x+1 ... x+10
//   y+1 ... y+4
//
// Заполняем ее зелеными вертикальными полосами слева направо.
// После КАЖДОГО кадра заново рисуем + и -, поэтому символы всегда
// остаются поверх зеленой заливки.
// ============================================================

void drawBatteryChargeFrame(int x, int y, uint8_t phase) {

  // Контур должен оставаться зеленым во время зарядки.
  matrix.drawRect(x, y, 12, 6, GREEN);

  // Очистить только внутренность; рамку не трогаем.
  matrix.fillRect(x + 1, y + 1, 10, 4, BLACK);

  // 5 стадий по 2 пикселя + пустой кадр.
  // phase: 0=пусто, 1=2px, ... 5=10px.
  uint8_t columns = constrain((int)phase, 0, 5) * 2;

  if (columns > 0) {
    matrix.fillRect(x + 1, y + 1, columns, 4, GREEN);
  }

  // + и - всегда поверх заливки.
  drawBatterySigns(x, y);
}


void updateBatteryChargeAnimation() {

  if (currentScreen != SCREEN_ENERGY) return;
  if (!telemetryValid) return;

  // s=-1 = зарядка.
  if (batteryState >= 0) {
    batteryChargePhase = 0;
    return;
  }

  uint32_t now = millis();

  if (now - batteryChargeAnimAt < BAT_CHARGE_ANIM_MS) return;

  batteryChargeAnimAt = now;

  batteryChargePhase++;

  if (batteryChargePhase > 5) {
    batteryChargePhase = 0;
  }

  drawBatteryChargeFrame(25, 26, batteryChargePhase);
}


// ============================================================
// АНИМАЦИЯ РАЗРЯДА АКБ
//
// Полный красный внутренний прямоугольник постепенно опустошается
// справа налево. Контур остается красным.
// Знаки + и - каждый кадр рисуются заново поверх заполнения.
// ============================================================

void drawBatteryDischargeFrame(int x, int y, uint8_t phase) {

  matrix.drawRect(x, y, 12, 6, RED);
  matrix.fillRect(x + 1, y + 1, 10, 4, BLACK);

  // phase 0 = полный, phase 5 = пустой.
  uint8_t columns = (5 - constrain((int)phase, 0, 5)) * 2;

  if (columns > 0) {
    matrix.fillRect(x + 1, y + 1, columns, 4, RED);
  }

  // Символы не должны затираться красной заливкой.
  drawBatterySigns(x, y);
}


void updateBatteryDischargeFillAnimation() {

  if (currentScreen != SCREEN_ENERGY) return;
  if (!telemetryValid) return;

  // s=+1 = разряд.
  if (batteryState <= 0) {
    batteryDischargeFillPhase = 0;
    return;
  }

  uint32_t now = millis();

  if (now - batteryDischargeFillAt < BAT_DISCHARGE_FILL_MS) return;

  batteryDischargeFillAt = now;

  batteryDischargeFillPhase++;

  if (batteryDischargeFillPhase > 5) {
    batteryDischargeFillPhase = 0;
  }

  drawBatteryDischargeFrame(25, 26, batteryDischargeFillPhase);
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
// ТОК АКБ ДЛЯ ЭНЕРГОТАБЛО
//
// В компактном протоколе уже есть batteryPower и batteryVoltage,
// поэтому отдельное поле UART не добавляем.
// Знак сохраняется:
//   +A = разряд
//   -A = заряд
// 0 A на табло не рисуем.
// ============================================================

int displayedBatteryCurrentA() {

  if (!telemetryValid) return 0;

  // Будущий серверный bc10 имеет приоритет.
  if (batteryCurrentFromServerValid) {

    int16_t v = batteryCurrentTenthsA;

    // Округление десятых ампера до целого с сохранением знака.
    if (v >= 0) return (v + 5) / 10;
    return (v - 5) / 10;
  }

  // Backward-compatible fallback для текущего ESP-протокола.
  if (batteryVoltage < 1.0f) return 0;

  float amps = ((float)batteryPower) / batteryVoltage;

  if (amps >= 0.0f) return (int)(amps + 0.5f);
  return (int)(amps - 0.5f);
}


void drawBatteryCurrentValue() {

  int amps = displayedBatteryCurrentA();

  // Область справа от АКБ.
  matrix.fillRect(34, 25, 30, 7, BLACK);

  if (amps == 0) return;

  matrix.setTextSize(1);
  matrix.setTextWrap(false);

  if (amps > 0) matrix.setTextColor(RED);
  else matrix.setTextColor(GREEN);

  // Для -1A нужно 18 px, для 1A — 12 px.
  // На 5 пикселей левее предыдущего положения.
  matrix.setCursor(amps < 0 ? 39 : 45, 25);
  matrix.print(amps);
  matrix.print("A");
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
long shownPvTenths = 2147483647L;
long shownGridTenths = 2147483647L;
long shownLoadTenths = 2147483647L;

// Отдельно храним состояния линий, потому что направление/цвет могут
// измениться даже если округленная надпись мощности осталась той же.
bool shownPvActive = false;
int8_t shownGridDirection = 99;   // -1 export, 0 line, +1 import
bool shownGridActive = false;
bool shownLoadActive = false;

// Ток АКБ на экране показываем целыми амперами.
// Специальное стартовое значение заставляет первый вывод обновиться.
int shownBatteryCurrentA = 32767;

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

  long pvTenths = displayedPowerTenths(pvPower);
  bool pvActive = (abs(pvPower) > 10);

  if (!shownTelemetryValid ||
      pvTenths != shownPvTenths ||
      pvActive != shownPvActive) {

    matrix.fillRect(38, 0, 26, 7, BLACK);
    printPowerKW(39, 0, pvPower, YELLOW);

    matrix.fillRect(29, 7, 5, 5, BLACK);
    uint16_t pvColor = (abs(pvPower) > 10) ? GREEN : GREY;
    arrowDown(31, 7, 11, pvColor);

    shownPvTenths = pvTenths;
    shownPvActive = pvActive;
  }

  long gridTenths = displayedPowerTenths(gridPower);
  int8_t gridDirection = (gridPower > 0) ? 1 : ((gridPower < 0) ? -1 : 0);
  bool gridActive = (abs(gridPower) > 10);

  if (!shownTelemetryValid ||
      gridTenths != shownGridTenths ||
      gridDirection != shownGridDirection ||
      gridActive != shownGridActive) {

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

    shownGridTenths = gridTenths;
    shownGridDirection = gridDirection;
    shownGridActive = gridActive;
  }

  long loadTenths = displayedPowerTenths(loadPower);
  bool loadActive = (loadPower > 10);

  if (!shownTelemetryValid ||
      loadTenths != shownLoadTenths ||
      loadActive != shownLoadActive) {

    matrix.fillRect(36, 15, 18, 3, BLACK);

    uint16_t loadColor = (loadPower > 10) ? GREEN : GREY;
    arrowRight(36, 16, 53, loadColor);

    // LOAD text zone is limited to x=36..53.
    // House begins at x=55, so its body/roof is never erased.
    matrix.fillRect(36, 8, 18, 7, BLACK);
    printPowerKW(36, 8, loadPower, WHITE);

    shownLoadTenths = loadTenths;
    shownLoadActive = loadActive;
  }

  if (!shownTelemetryValid ||
      batteryState != shownBatteryState ||
      batteryDischarging != shownDischarging) {

    matrix.fillRect(29, 21, 5, 4, BLACK);

    if (batteryState > 0) drawBatteryDischargeArrow(batteryDischargeArrowVisible);
    else if (batteryState < 0) arrowDown(31, 21, 24, GREEN);
    else matrix.drawLine(31, 21, 31, 24, GREY);

    drawBattery(25, 26);

    if (batteryState < 0) {
      batteryChargePhase = 0;
      batteryChargeAnimAt = millis();
      drawBatteryChargeFrame(25, 26, batteryChargePhase);
    } else if (batteryState > 0) {
      batteryDischargeFillPhase = 0;
      batteryDischargeFillAt = millis();
      drawBatteryDischargeFrame(25, 26, batteryDischargeFillPhase);
    }

    shownBatteryState = batteryState;
    shownDischarging = batteryDischarging;
  }

  int batteryCurrentA = displayedBatteryCurrentA();

  if (!shownTelemetryValid || batteryCurrentA != shownBatteryCurrentA) {
    drawBatteryCurrentValue();
    shownBatteryCurrentA = batteryCurrentA;
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

  if (batteryState < 0) {
    batteryChargePhase = 0;
    batteryChargeAnimAt = millis();
    drawBatteryChargeFrame(25, 26, batteryChargePhase);
  } else if (batteryState > 0) {
    batteryDischargeFillPhase = 0;
    batteryDischargeFillAt = millis();
    drawBatteryDischargeFrame(25, 26, batteryDischargeFillPhase);
  }

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

  // Ток АКБ справа от значка. При 0 A область остается пустой.
  drawBatteryCurrentValue();

}


// ============================================================
// КИРИЛЛИЦА: U8g2_for_Adafruit_GFX
//
// Используется сторонний U8g2 шрифт:
//   u8g2_font_5x7_t_cyrillic
//
// Строки в скетче — обычный UTF-8.
// ============================================================

void setupCyrFont(uint16_t color) {

  u8g2.setFontMode(1);          // transparent background
  u8g2.setFontDirection(0);
  u8g2.setForegroundColor(color);
  u8g2.setFont(u8g2_font_5x7_t_cyrillic);
}


void printCyrCentered(const char *s, int baselineY, uint16_t color) {

  setupCyrFont(color);

  int16_t width = u8g2.getUTF8Width(s);
  int16_t x = (64 - width) / 2;

  if (x < 0) x = 0;

  u8g2.setCursor(x, baselineY);
  u8g2.print(s);
}


String etaRusText() {

  if (etaToReserveSec < 0) return String("--ч. --мин.");

  unsigned long totalMinutes = (unsigned long)etaToReserveSec / 60UL;
  unsigned int hours = totalMinutes / 60UL;
  uint8_t minutes = totalMinutes % 60UL;

  if (hours > 99) hours = 99;

  String s;
  s.reserve(20);

  s += String(hours);
  s += "ч. ";
  s += String(minutes);
  s += "мин.";

  return s;
}


// ============================================================
// ЭКРАНЫ
// ============================================================



void syncShownEnergyValues() {

  if (!telemetryValid) {
    shownTelemetryValid = false;
    return;
  }

  shownPvTenths = displayedPowerTenths(pvPower);
  shownGridTenths = displayedPowerTenths(gridPower);
  shownLoadTenths = displayedPowerTenths(loadPower);

  shownPvActive = (abs(pvPower) > 10);

  if (gridPower > 0) shownGridDirection = 1;
  else if (gridPower < 0) shownGridDirection = -1;
  else shownGridDirection = 0;

  shownGridActive = (abs(gridPower) > 10);
  shownLoadActive = (loadPower > 10);
  shownBatteryCurrentA = displayedBatteryCurrentA();

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
  matrix.setCursor(26, 2);
  matrix.setTextColor(visible ? GREEN : BLACK);
  matrix.print(":");
}


uint16_t autonomyTimeColor() {

  if (batterySoc >= 0 && batterySoc <= BAT_SOC_RED_PCT) return RED;

  if (etaToReserveSec >= 0 &&
      (uint32_t)etaToReserveSec <= BAT_ETA_RED_SEC) return RED;

  return GREEN;
}


void drawClockBatteryIcon(uint8_t phase) {

  const int x = 1;
  const int y = 25;

  matrix.fillRect(x, y, 13, 7, BLACK);

  matrix.drawRect(x, y, 11, 6, RED);
  matrix.fillRect(x + 11, y + 2, 2, 2, RED);

  matrix.fillRect(x + 1, y + 1, 9, 4, BLACK);

  uint8_t p = constrain((int)phase, 0, 5);
  uint8_t columns = (5 - p) * 2;
  if (columns > 9) columns = 9;

  if (columns > 0) {
    matrix.fillRect(x + 1, y + 1, columns, 4, RED);
  }
}


void updateClockBatteryAnimation() {

  if (currentScreen != SCREEN_CLOCK) return;
  if (!batteryDischarging) return;

  uint32_t now = millis();

  if (now - clockBatAnimAt < CLOCK_BAT_ANIM_MS) return;

  clockBatAnimAt = now;

  clockBatAnimPhase++;
  if (clockBatAnimPhase > 5) clockBatAnimPhase = 0;

  drawClockBatteryIcon(clockBatAnimPhase);
}


String etaWorkText() {

  if (etaToReserveSec < 0) return String("--ч. --мин.");

  unsigned long totalMinutes = (unsigned long)etaToReserveSec / 60UL;
  unsigned int hours = totalMinutes / 60UL;
  uint8_t minutes = totalMinutes % 60UL;

  if (hours > 99) hours = 99;

  String s;
  s.reserve(20);

  s += String(hours);
  s += "ч. ";
  s += String(minutes);
  s += "мин.";

  return s;
}


void drawClockAutonomyRemaining() {

  matrix.fillRect(0, 23, 64, 9, BLACK);

  if (!batteryDischarging) return;

  uint16_t color = autonomyTimeColor();

  drawClockBatteryIcon(clockBatAnimPhase);

  setupCyrFont(color);

  String s = etaWorkText();

  u8g2.setCursor(16, 30);
  u8g2.print(s);
}


void drawClockScreen() {

  matrix.fillScreen(BLACK);

  // Большие часы сверху.
  matrix.setTextWrap(false);
  matrix.setTextSize(2);
  matrix.setTextColor(GREEN);

  if (currentTimeText.length() == 5) {

    matrix.setCursor(2, 2);
    matrix.print(currentTimeText.substring(0, 2));

    matrix.setCursor(38, 2);
    matrix.print(currentTimeText.substring(3, 5));

  } else {

    matrix.setCursor(2, 2);
    matrix.print("--");

    matrix.setCursor(38, 2);
    matrix.print("--");
  }

  clockColonVisible = true;
  clockColonChangedAt = millis();

  matrix.setTextSize(2);
  matrix.setCursor(26, 2);
  matrix.setTextColor(GREEN);
  matrix.print(":");

  // Нижняя строка не мигает.
  // При разряде: анимированная АКБ + "14ч. 5мин.".
  clockBatAnimPhase = 0;
  clockBatAnimAt = millis();
  drawClockAutonomyRemaining();
}


void updateClockColon() {

  if (currentScreen != SCREEN_CLOCK) return;

  uint32_t now = millis();

  if (now - clockColonChangedAt >= 1000UL) {

    clockColonChangedAt = now;
    clockColonVisible = !clockColonVisible;
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

  uint16_t infoColor =
      (batterySoc >= 0 && batterySoc <= BAT_SOC_RED_PCT) ? RED : GREEN;

  // Сторонний U8g2 Cyrillic 5x7.
  // baseline: 7 / 16 / 27 — все три строки помещаются в 32 px.
  printCyrCentered("Время", 7, infoColor);
  printCyrCentered("работы:", 16, infoColor);

  String eta = etaRusText();
  printCyrCentered(eta.c_str(), 27, infoColor);
}


void drawCurrentScreen() {

  if (currentScreen == SCREEN_ENERGY) drawEnergyScreen();
  else if (currentScreen == SCREEN_AUTONOMY) drawDischargeInfoScreen();
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

  } else if (currentScreen == SCREEN_AUTONOMY) {

    drawDischargeInfoScreen();

  } else {

    drawClockScreen();
  }
}


// ============================================================
// ПРИМЕНЕНИЕ НАСТРОЕК, ПОЛУЧЕННЫХ С СЕРВЕРА
// ============================================================

void redrawCurrentScreenAfterSettings() {

  if (currentScreen == SCREEN_ENERGY) {

    drawEnergyScreen();
    syncShownEnergyValues();

  } else if (currentScreen == SCREEN_AUTONOMY) {

    drawDischargeInfoScreen();

  } else {

    drawClockScreen();
  }
}


void applyServerDisplaySettings(
    int dc,
    int ep,
    int ai,
    int br,
    int bc10
) {

  bool brightnessChanged = false;

  if (dc >= 20 && dc <= 300) {
    DISPLAY_CYCLE_SEC = (uint16_t)dc;
  }

  if (ep >= 10 && ep <= 95) {
    ENERGY_SHARE_PCT = (uint8_t)ep;
  }

  if (ai >= 2 && ai <= 30) {
    AUTONOMY_INFO_SEC = (uint8_t)ai;
  }

  if (br >= 0 && br <= 7 && br != PANEL_BRIGHTNESS) {
    PANEL_BRIGHTNESS = (uint8_t)br;
    updateColors();
    brightnessChanged = true;
  }

  // bc10: десятые ампера. Разумный защитный диапазон ±500 A.
  if (bc10 >= -5000 && bc10 <= 5000) {
    batteryCurrentTenthsA = (int16_t)bc10;
    batteryCurrentFromServerValid = true;
  }

  if (brightnessChanged) {
    redrawCurrentScreenAfterSettings();
  }

  LOG(F("[SET] cycle=")); LOG(DISPLAY_CYCLE_SEC);
  LOG(F("s energy=")); LOG(ENERGY_SHARE_PCT);
  LOG(F("% autonomy=")); LOG(AUTONOMY_INFO_SEC);
  LOG(F("s br=")); LOG(PANEL_BRIGHTNESS);

  if (batteryCurrentFromServerValid) {
    LOG(F(" bc10=")); LOG(batteryCurrentTenthsA);
  }

  LOGLN(F(""));
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

  // Legacy:
  // DATA|p|l|g|b|v10|soc|d|a|eta|s|HH:MM|crc
  //
  // Future extended:
  // DATA|p|l|g|b|v10|soc|d|a|eta|s|HH:MM|dc|ep|ai|br|bc10|crc
  //
  // Текущая ESP-прошивка может продолжать присылать 11 полей.
  String part[16];
  uint8_t partCount = 0;
  int pos = 5;

  while (pos <= (int)payload.length() && partCount < 16) {

    int sep = payload.indexOf('|', pos);

    if (sep < 0) {
      part[partCount++] = payload.substring(pos);
      break;
    }

    part[partCount++] = payload.substring(pos, sep);
    pos = sep + 1;
  }

  if (partCount < 11) return false;

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

  // Если сервер/ESP позже добавят все 5 полей — Mega уже готова.
  if (partCount >= 16) {

    applyServerDisplaySettings(
        part[11].toInt(),  // dc
        part[12].toInt(),  // ep
        part[13].toInt(),  // ai
        part[14].toInt(),  // br
        part[15].toInt()   // bc10
    );
  }

  telemetryValid = true;
  lastEspDataAt = millis();

  if (currentScreen == SCREEN_ENERGY) {

    redrawChangedFields();

  } else if (currentScreen == SCREEN_CLOCK) {

    drawClockAutonomyRemaining();
  }

  LOG(F("[ESP DATA] PV=")); LOG(pvPower);
  LOG(F(" GRID=")); LOG(gridPower);
  LOG(F(" LOAD=")); LOG(loadPower);
  LOG(F(" SOC=")); LOG(batterySoc);
  LOG(F(" S=")); LOG(batteryState);
  LOG(F(" ATT=")); LOG(attentionRequired ? 1 : 0);

  if (batteryCurrentFromServerValid) {
    LOG(F(" BC10=")); LOG(batteryCurrentTenthsA);
  }

  LOGLN(F(""));

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

  // Подключаем U8g2 renderer к RGBmatrixPanel/Adafruit_GFX.
  u8g2.begin(matrix);

  updateColors();
  drawEnergyScreen();

  sendEspConfig();

  currentScreen = SCREEN_ENERGY;
  screenStartedAt = millis();

  wdt_enable(WDTO_8S);
  wdt_reset();

  LOGLN(F("[WDT] Enabled: 8 s"));

  LOG(F("[SET] display cycle=")); LOG(DISPLAY_CYCLE_SEC);
  LOG(F(" s, energy=")); LOG(ENERGY_SHARE_PCT);
  LOG(F(" %, autonomy=")); LOG(AUTONOMY_INFO_SEC);
  LOG(F(" s, brightness=")); LOGLN(PANEL_BRIGHTNESS);
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  wdt_reset();

  uint32_t now = millis();

  serviceEspBridge();

  updateBuzzer();
  updateClockColon();
  updateClockBatteryAnimation();

  updateBatteryDischargeBlink();
  updateBatteryChargeAnimation();
  updateBatteryDischargeFillAnimation();
  updateBridgeFlowAnimation();

  // ----------------------------------------------------------
  // Экраны:
  //
  // Норма:
  //   ENERGY = DISPLAY_CYCLE_SEC * ENERGY_SHARE_PCT
  //   CLOCK  = остаток DISPLAY_CYCLE_SEC
  //
  // Разряд:
  //   ENERGY -> "Время работы:" AUTONOMY_INFO_SEC
  //          -> CLOCK
  //          -> ENERGY
  // ----------------------------------------------------------

  if (currentScreen == SCREEN_ENERGY) {

    if (now - screenStartedAt >= energyScreenMs()) {

      if (batteryDischarging) switchToScreen(SCREEN_AUTONOMY);
      else switchToScreen(SCREEN_CLOCK);
    }

  } else if (currentScreen == SCREEN_AUTONOMY) {

    if (now - screenStartedAt >= autonomyScreenMs()) {
      switchToScreen(SCREEN_CLOCK);
    }

  } else {

    if (now - screenStartedAt >= clockScreenMs()) {

      // Аккуратный переход, когда большое двоеточие погашено.
      if (!clockColonVisible) {
        switchToScreen(SCREEN_ENERGY);
      }
    }
  }

  wdt_reset();
}

