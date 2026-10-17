#include <RGBmatrixPanel.h>

// ============================================================
// ОТЛАДКА ЧЕРЕЗ USB SERIAL
// ============================================================

#define DEBUG_ESP 1
#define DEBUG_ESP_RAW 1

#if DEBUG_ESP
#define LOG(x) Serial.print(x)
#define LOGLN(x) Serial.println(x)
#else
#define LOG(x)
#define LOGLN(x)
#endif

// ============================================================
// RGB MATRIX 64x32
// Геометрия верхней части сохранена из проверенного шаблона.
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
// ЯРКОСТЬ 0...7
// ============================================================

uint8_t PANEL_BRIGHTNESS = 4;


// ============================================================
// ПОРОГИ ИНДИКАЦИИ АКБ
// ============================================================

// SOC зеленый выше этого значения, красный при <=
const uint8_t BAT_SOC_RED_PCT = 20;

// ETA зеленое выше этого времени, красное при <=
const uint32_t BAT_ETA_RED_SEC = 30UL * 60UL;   // 30 минут


// ============================================================
// ДАННЫЕ
//
// Общая семантика с RMS v2.11.26:
// batteryPower > 0 = BAT -> INV = разряд
// batteryPower < 0 = INV -> BAT = заряд
// ============================================================

long pvPower      = 500;
long gridPower    = -940;
long loadPower    = 913;
long batteryPower = 450;

float batteryVoltage = 54.5;
int batterySoc = 78;

bool batteryDischarging = true;
long etaToReserveSec = 3L * 3600L + 30L * 60L;


// ============================================================
// ESP8266 / RMS7688A
//
// Вариант для ESP8266 с AT firmware.
// ESP8266 подключен к аппаратному UART Mega.
//
// Для типовой Mega+WiFi R3 используется Serial3,
// если DIP-переключатели платы направляют ESP8266 на RX3/TX3.
//
// RMS7688A поднимает Wi-Fi AP.
// ESP8266 подключается к этой точке доступа.
// Затем получает компактную строку:
//
// PV=500;GRID=-940;LOAD=913;BAT=450;BV=54.5;SOC=78;DIS=1;ETA=12600
//
// по HTTP GET /api/rgb
// ============================================================

#define WIFI_ENABLE 1

HardwareSerial &ESP = Serial3;

const unsigned long ESP_BAUD = 19200;

// !!! ЗАМЕНИТЬ НА ПАРАМЕТРЫ ТОЧКИ ДОСТУПА RMS7688A !!!
const char RMS_WIFI_SSID[] = "RMS7688A";
const char RMS_WIFI_PASS[] = "<WIFI_PASSWORD>";

// !!! ЗАМЕНИТЬ, ЕСЛИ RMS ИМЕЕТ ДРУГОЙ IP !!!
const char RMS_HOST[] = "192.168.54.30";
const uint16_t RMS_HTTP_PORT = 8088;
const char RMS_RGB_PATH[] = "/api/summary";

const uint32_t RMS_POLL_MS = 2000;
const uint32_t WIFI_RETRY_MS = 15000;

uint32_t prevRmsPoll = 0;
uint32_t prevWifiRetry = 0;

bool wifiReady = false;
uint8_t httpFailCount = 0;


// ============================================================
// АНИМАЦИЯ
// ============================================================

#define ANIM_FPS 15
#define BATTERY_ANIM_MS 180

uint32_t prevAnimTime = 0;
uint32_t prevBatteryAnimTime = 0;

uint16_t animPhase = 0;
uint16_t slowAnimPhase = 0;
uint8_t slowDivider = 0;

// 0 = пусто, 1..4 = заполнение снизу вверх.
// После 4 следующий кадр снова 0.
uint8_t batteryFillPhase = 0;


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

  uint8_t headG = max((int)b - 1, 1);
  uint8_t midG  = max((int)b - 2, 1);
  uint8_t tailG = 1;

  FLOW_HEAD = matrix.Color333(0, headG, 0);
  FLOW_MID  = matrix.Color333(0, midG, 0);
  FLOW_TAIL = matrix.Color333(0, tailG, 0);
}


// ============================================================
// ВЫВОД МОЩНОСТИ В кВт
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
// СОЛНЦЕ
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
// МОЛНИЯ / СЕТЬ
// ============================================================

void drawGridIcon(int x, int y) {

  matrix.drawLine(x + 3, y, x, y + 4, YELLOW);
  matrix.drawLine(x, y + 4, x + 3, y + 4, YELLOW);
  matrix.drawLine(x + 3, y + 4, x + 1, y + 8, YELLOW);
}


// ============================================================
// ИНВЕРТОР
// ============================================================

void drawInverter(int x, int y) {

  matrix.drawRect(x, y, 7, 7, BLUE);
}


// ============================================================
// ДОМ
// ============================================================

void drawHouse(int x, int y) {

  matrix.drawRect(x + 1, y + 4, 7, 4, WHITE);
  matrix.drawLine(x, y + 4, x + 4, y, RED);
  matrix.drawLine(x + 4, y, x + 8, y + 4, RED);
}


// ============================================================
// ЗНАЧКИ + / - ВНУТРИ АКБ
//
// Рисуются ПОСЛЕ заливки, чтобы всегда оставаться видимыми.
// ============================================================

void drawBatterySigns(int x, int y) {

  // "+" белый — хорошо виден и на черном, и на красном фоне.
  matrix.drawPixel(x + 2, y + 2, WHITE);
  matrix.drawPixel(x + 3, y + 2, WHITE);
  matrix.drawPixel(x + 4, y + 2, WHITE);
  matrix.drawPixel(x + 3, y + 1, WHITE);
  matrix.drawPixel(x + 3, y + 3, WHITE);

  // "-" синий.
  matrix.drawPixel(x + 7, y + 2, BLUE);
  matrix.drawPixel(x + 8, y + 2, BLUE);
  matrix.drawPixel(x + 9, y + 2, BLUE);
}


// ============================================================
// БАТАРЕЯ
//
// Контур всегда БЕЛЫЙ.
//
// Разряд:
// красная заливка идет снизу вверх:
// 0 -> 1 -> 2 -> 3 -> 4 строки,
// затем очищается и цикл повторяется.
//
// Заряд / покой:
// внутренняя область пустая.
// ============================================================

void drawBattery(int x, int y) {

  // Очистить внутренность, не трогая внешний контур.
  matrix.fillRect(x + 1, y + 1, 10, 4, BLACK);

  // Белый контур.
  matrix.drawRect(x, y, 12, 6, WHITE);

  if (batteryDischarging && batteryPower > 10 && batteryFillPhase > 0) {
    uint8_t rows = constrain(batteryFillPhase, 1, 4);
    int fillY = y + 5 - rows;
    matrix.fillRect(x + 1, fillY, 10, rows, RED);
  }

  drawBatterySigns(x, y);
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
// АНИМИРОВАННЫЕ ПОТОКИ
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


// ============================================================
// ЦВЕТ SOC / ETA
// ============================================================

uint16_t batterySocColor() {

  if (batterySoc <= BAT_SOC_RED_PCT) return RED;
  return GREEN;
}

uint16_t batteryEtaColor() {

  if (etaToReserveSec >= 0 && (uint32_t)etaToReserveSec <= BAT_ETA_RED_SEC) return RED;
  return GREEN;
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

  // Отрицательное значение шире на один символ.
  // Сдвигаем ВЛЕВО, чтобы оно не залезало на инвертор.
  if (gridPower < 0) printPowerKW(4, 8, gridPower, WHITE);
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

  drawBattery(25, 26);

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
  matrix.setTextColor(batterySocColor());
  matrix.setCursor(1, 25);

  matrix.print(batterySoc);
  matrix.print("%");

  // ----------------------------------------------------------
  // ETA
  // ----------------------------------------------------------

  if (batteryDischarging && etaToReserveSec >= 0) {
    printETA(39, 25, etaToReserveSec, batteryEtaColor());
  }
}


// ============================================================
// ОБНОВЛЕНИЕ ТОЛЬКО НИЖНЕЙ ОБЛАСТИ АКБ
//
// Вызывается после прихода новых данных.
// ============================================================

void redrawBatteryArea() {

  // SOC
  matrix.fillRect(0, 25, 24, 7, BLACK);

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setTextColor(batterySocColor());
  matrix.setCursor(1, 25);

  matrix.print(batterySoc);
  matrix.print("%");

  // АКБ
  drawBattery(25, 26);

  // ETA
  matrix.fillRect(38, 25, 26, 7, BLACK);

  if (batteryDischarging && etaToReserveSec >= 0) {
    printETA(39, 25, etaToReserveSec, batteryEtaColor());
  }

  // Стрелка батареи
  matrix.fillRect(29, 20, 5, 5, BLACK);

  if (batteryPower > 10) {
    arrowUp(31, 21, 24, RED);
  } else if (batteryPower < -10) {
    arrowDown(31, 21, 24, GREEN);
  } else {
    matrix.drawLine(31, 21, 31, 24, GREY);
  }
}


// ============================================================
// СЛОЙ АНИМАЦИИ ЭНЕРГОПОТОКОВ
//
// Стрелка разряда АКБ красная.
// Внутренняя анимация АКБ выполняется отдельно.
// ============================================================

void drawEnergyAnimation() {

  if (pvPower > 10) {
    flowVertical(31, 7, 11, false, slowAnimPhase);
  }

  if (abs(gridPower) > 10) {
    bool reverseGrid = (gridPower < 0);
    flowHorizontal(9, 26, 16, reverseGrid, animPhase);
  }

  if (loadPower > 10) {
    flowHorizontal(36, 53, 16, false, animPhase);
  }
}


// ============================================================
// ВОССТАНОВЛЕНИЕ ЛИНИЙ
// ============================================================

void restoreFlowLines() {

  uint16_t pvColor   = (abs(pvPower) > 10)   ? GREEN : GREY;
  uint16_t gridColor = (abs(gridPower) > 10) ? GREEN : GREY;
  uint16_t loadColor = (loadPower > 10)      ? GREEN : GREY;

  matrix.drawLine(31, 7, 31, 11, pvColor);
  matrix.drawLine(9, 16, 26, 16, gridColor);
  matrix.drawLine(36, 16, 53, 16, loadColor);

  // Батарейная стрелка — статичная по цвету/направлению.
  if (batteryPower > 10) {
    arrowUp(31, 21, 24, RED);
  } else if (batteryPower < -10) {
    arrowDown(31, 21, 24, GREEN);
  } else {
    matrix.drawLine(31, 21, 31, 24, GREY);
  }
}


// ============================================================
// ESP8266 AT: ОЧИСТКА RX
// ============================================================

void espFlushInput() {

  while (ESP.available()) ESP.read();
}


// ============================================================
// ESP8266 AT: ОЖИДАНИЕ СТРОКИ
// ============================================================

bool espWaitFor(const char *needle, uint32_t timeoutMs) {

  String response;
  String rxLine;

  response.reserve(180);
  rxLine.reserve(120);

  uint32_t started = millis();

  while (millis() - started < timeoutMs) {

    while (ESP.available()) {

      char c = (char)ESP.read();

#if DEBUG_ESP_RAW
      if (c == '\r') {
        // ignore
      } else if (c == '\n') {
        if (rxLine.length()) {
          LOG(F("[ESP RX] "));
          LOGLN(rxLine);
          rxLine = "";
        }
      } else {
        if (rxLine.length() < 119) rxLine += c;
      }
#endif

      if (response.length() < 179) {
        response += c;
      } else {
        response.remove(0, 60);
        response += c;
      }

      if (response.indexOf(needle) >= 0) {
#if DEBUG_ESP_RAW
        if (rxLine.length()) {
          LOG(F("[ESP RX] "));
          LOGLN(rxLine);
        }
#endif
        return true;
      }

      if (response.indexOf("ERROR") >= 0) {
        LOGLN(F("[ESP] ERROR received"));
        return false;
      }

      if (response.indexOf("FAIL") >= 0) {
        LOGLN(F("[ESP] FAIL received"));
        return false;
      }
    }
  }

#if DEBUG_ESP_RAW
  if (rxLine.length()) {
    LOG(F("[ESP RX] "));
    LOGLN(rxLine);
  }
#endif

  LOG(F("[ESP] Timeout waiting for: "));
  LOGLN(needle);

  return false;
}


// ============================================================
// ESP8266 AT: КОМАНДА
// ============================================================

bool espCommand(const String &cmd, const char *okText, uint32_t timeoutMs) {

  espFlushInput();

  LOG(F("[ESP TX] "));

  if (cmd.startsWith("AT+CWJAP=")) {
    LOGLN(F("AT+CWJAP=<SSID/PASSWORD hidden>"));
  } else {
    LOGLN(cmd);
  }

  ESP.print(cmd);
  ESP.print("\r\n");

  bool ok = espWaitFor(okText, timeoutMs);

  if (!ok) {
    LOG(F("[ESP] Command failed, expected: "));
    LOGLN(okText);
  }

  return ok;
}


// ============================================================
// ESP8266: ПОДКЛЮЧЕНИЕ К AP RMS7688A
// ============================================================

bool wifiConnect() {

#if !WIFI_ENABLE
  return false;
#else

  wifiReady = false;

  LOGLN(F("[WIFI] --------------------------------"));
  LOGLN(F("[WIFI] Starting ESP8266 connection"));
  LOG(F("[WIFI] ESP UART baud: "));
  LOGLN(ESP_BAUD);

  LOG(F("[WIFI] Target SSID: "));
  LOGLN(RMS_WIFI_SSID);

  LOG(F("[WIFI] RMS host: "));
  LOG(RMS_HOST);
  LOG(F(":"));
  LOGLN(RMS_HTTP_PORT);

  LOGLN(F("[WIFI] Checking AT firmware..."));

  if (!espCommand("AT", "OK", 1500)) {
    LOGLN(F("[WIFI] ESP8266 did not answer AT"));
    LOGLN(F("[WIFI] Check DIP switches, Serial3 and ESP baud rate"));
    return false;
  }

  LOGLN(F("[WIFI] ESP8266 responds"));

  if (!espCommand("ATE0", "OK", 1500)) {
    LOGLN(F("[WIFI] ATE0 failed, continuing anyway"));
  }

  LOGLN(F("[WIFI] Setting station mode..."));

  if (!espCommand("AT+CWMODE=1", "OK", 2000)) {
    LOGLN(F("[WIFI] Failed to set station mode"));
    return false;
  }

  String join = "AT+CWJAP=\"";
  join += RMS_WIFI_SSID;
  join += "\",\"";
  join += RMS_WIFI_PASS;
  join += "\"";

  LOGLN(F("[WIFI] Joining RMS7688A access point..."));

  if (!espCommand(join, "OK", 20000)) {
    LOGLN(F("[WIFI] Wi-Fi join FAILED"));
    return false;
  }

  wifiReady = true;
  httpFailCount = 0;

  LOGLN(F("[WIFI] Wi-Fi connected successfully"));
  LOGLN(F("[WIFI] --------------------------------"));

  return true;

#endif
}


// ============================================================
// JSON /api/summary — извлечение только нужных полей
//
// Полный JSON большой, поэтому целиком в RAM его не сохраняем.
// Из входного потока ESP8266 вырезаем только:
//   pv_w
//   grid_w
//   load_w
//   battery_power_w
//   battery_voltage_v
//   battery_soc_pct        (если поле добавлено на RMS)
//   battery_discharging
//   eta_to_reserve_sec
//
// Если battery_soc_pct пока отсутствует в /api/summary,
// старое значение SOC на панели сохраняется.
// ============================================================

struct SummaryFields {
  bool gotPv;
  bool gotGrid;
  bool gotLoad;
  bool gotBatPower;
  bool gotBatVoltage;
  bool gotSoc;
  bool gotDischarging;
  bool gotEta;

  long pv;
  long grid;
  long load;
  long batPower;
  float batVoltage;
  int soc;
  bool discharging;
  long etaSec;
};

void resetSummaryFields(SummaryFields &s) {
  s.gotPv = false;
  s.gotGrid = false;
  s.gotLoad = false;
  s.gotBatPower = false;
  s.gotBatVoltage = false;
  s.gotSoc = false;
  s.gotDischarging = false;
  s.gotEta = false;

  s.pv = pvPower;
  s.grid = gridPower;
  s.load = loadPower;
  s.batPower = batteryPower;
  s.batVoltage = batteryVoltage;
  s.soc = batterySoc;
  s.discharging = batteryDischarging;
  s.etaSec = etaToReserveSec;
}

bool isJsonNumberChar(char c) {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

bool extractJsonNumber(const String &window, const char *key, double &value) {
  String token = "\"";
  token += key;
  token += "\":";

  int p = window.indexOf(token);
  if (p < 0) return false;

  p += token.length();

  while (p < (int)window.length() && (window[p] == ' ' || window[p] == '\t')) p++;

  if (window.startsWith("null", p)) return false;

  String num;
  num.reserve(24);

  while (p < (int)window.length() && isJsonNumberChar(window[p])) {
    num += window[p++];
    if (num.length() >= 23) break;
  }

  if (!num.length()) return false;

  value = num.toFloat();
  return true;
}

bool extractJsonBool(const String &window, const char *key, bool &value) {
  String token = "\"";
  token += key;
  token += "\":";

  int p = window.indexOf(token);
  if (p < 0) return false;

  p += token.length();

  while (p < (int)window.length() && (window[p] == ' ' || window[p] == '\t')) p++;

  if (window.startsWith("true", p)) {
    value = true;
    return true;
  }

  if (window.startsWith("false", p)) {
    value = false;
    return true;
  }

  return false;
}

bool extractJsonEta(const String &window, long &eta) {
  String token = "\"eta_to_reserve_sec\":";

  int p = window.indexOf(token);
  if (p < 0) return false;

  p += token.length();

  while (p < (int)window.length() && (window[p] == ' ' || window[p] == '\t')) p++;

  if (window.startsWith("null", p)) {
    eta = -1;
    return true;
  }

  String num;
  num.reserve(24);

  while (p < (int)window.length() && isJsonNumberChar(window[p])) {
    num += window[p++];
    if (num.length() >= 23) break;
  }

  if (!num.length()) return false;

  eta = (long)num.toFloat();
  return true;
}

void scanSummaryWindow(const String &window, SummaryFields &s) {
  double n;
  bool b;

  if (!s.gotPv && extractJsonNumber(window, "pv_w", n)) {
    s.pv = (long)n;
    s.gotPv = true;
  }

  if (!s.gotGrid && extractJsonNumber(window, "grid_w", n)) {
    s.grid = (long)n;
    s.gotGrid = true;
  }

  if (!s.gotLoad && extractJsonNumber(window, "load_w", n)) {
    s.load = (long)n;
    s.gotLoad = true;
  }

  if (!s.gotBatPower && extractJsonNumber(window, "battery_power_w", n)) {
    s.batPower = (long)n;
    s.gotBatPower = true;
  }

  if (!s.gotBatVoltage && extractJsonNumber(window, "battery_voltage_v", n)) {
    s.batVoltage = (float)n;
    s.gotBatVoltage = true;
  }

  if (!s.gotSoc && extractJsonNumber(window, "battery_soc_pct", n)) {
    s.soc = constrain((int)n, 0, 100);
    s.gotSoc = true;
  }

  if (!s.gotDischarging && extractJsonBool(window, "battery_discharging", b)) {
    s.discharging = b;
    s.gotDischarging = true;
  }

  if (!s.gotEta) {
    long eta;
    if (extractJsonEta(window, eta)) {
      s.etaSec = eta;
      s.gotEta = true;
    }
  }
}

bool summaryHasRequiredFields(const SummaryFields &s) {
  return s.gotPv && s.gotGrid && s.gotLoad && s.gotBatPower && s.gotBatVoltage && s.gotDischarging && s.gotEta;
}

bool applySummaryFields(const SummaryFields &s) {
  if (!summaryHasRequiredFields(s)) return false;

  bool fullRedraw = (s.pv != pvPower || s.grid != gridPower || s.load != loadPower);

  pvPower = s.pv;
  gridPower = s.grid;
  loadPower = s.load;
  batteryPower = s.batPower;
  batteryVoltage = s.batVoltage;

  if (s.gotSoc) batterySoc = s.soc;

  batteryDischarging = s.discharging;
  etaToReserveSec = s.etaSec;

  if (!batteryDischarging) batteryFillPhase = 0;

  if (fullRedraw) {
    drawDashboard();
    drawEnergyAnimation();
  } else {
    redrawBatteryArea();
  }

  LOGLN(F("[RMS] /api/summary accepted"));

  LOG(F("[RMS] PV=")); LOG(pvPower);
  LOG(F(" W  GRID=")); LOG(gridPower);
  LOG(F(" W  LOAD=")); LOG(loadPower);
  LOG(F(" W  BAT=")); LOG(batteryPower);
  LOGLN(F(" W"));

  LOG(F("[RMS] BV=")); Serial.print(batteryVoltage, 1);
  LOG(F(" V  SOC="));

  if (s.gotSoc) {
    LOG(batterySoc);
    LOG(F("%"));
  } else {
    LOG(F("<field absent>"));
  }

  LOG(F("  DIS=")); LOG(batteryDischarging ? 1 : 0);
  LOG(F("  ETA=")); LOGLN(etaToReserveSec);

  if (batteryDischarging && etaToReserveSec >= 0) {
    unsigned long totalMinutes = (unsigned long)etaToReserveSec / 60UL;

    LOG(F("[RMS] ETA human: "));
    LOG(totalMinutes / 60UL);
    LOG(F(":"));

    if ((totalMinutes % 60UL) < 10) LOG(F("0"));

    LOGLN(totalMinutes % 60UL);
  }

  return true;
}


// ============================================================
// HTTP GET /api/rgb ЧЕРЕЗ ESP8266 AT
// ============================================================

bool fetchRmsData() {

#if !WIFI_ENABLE
  return false;
#else

  if (!wifiReady) {
    LOGLN(F("[HTTP] Request skipped: Wi-Fi is not ready"));
    return false;
  }

  LOGLN(F("[HTTP] --------------------------------"));
  LOG(F("[HTTP] GET http://"));
  LOG(RMS_HOST);
  LOG(F(":"));
  LOG(RMS_HTTP_PORT);
  LOGLN(RMS_RGB_PATH);

  String cip = "AT+CIPSTART=\"TCP\",\"";
  cip += RMS_HOST;
  cip += "\",";
  cip += String(RMS_HTTP_PORT);

  // В AT 1.7.x CIPSTART может вернуть OK, CONNECT или ALREADY CONNECTED.
  espFlushInput();

  LOG(F("[ESP TX] "));
  LOGLN(cip);

  ESP.print(cip);
  ESP.print("\r\n");

  String tcpReply;
  tcpReply.reserve(160);

  uint32_t tcpStart = millis();
  bool tcpConnected = false;

  while (millis() - tcpStart < 6000) {
    while (ESP.available()) {
      char c = (char)ESP.read();

      if (tcpReply.length() < 159) tcpReply += c;

      if (tcpReply.indexOf("CONNECT") >= 0 || tcpReply.indexOf("ALREADY CONNECTED") >= 0) {
        tcpConnected = true;
      }

      if (tcpReply.indexOf("ERROR") >= 0 || tcpReply.indexOf("FAIL") >= 0) {
        LOG(F("[ESP RX] "));
        LOGLN(tcpReply);
        LOGLN(F("[HTTP] TCP connection FAILED"));
        return false;
      }
    }

    if (tcpConnected && tcpReply.indexOf("OK") >= 0) break;
  }

  LOG(F("[ESP RX] "));
  LOGLN(tcpReply);

  if (!tcpConnected && tcpReply.indexOf("OK") < 0) {
    LOGLN(F("[HTTP] TCP connection FAILED"));
    return false;
  }

  LOGLN(F("[HTTP] TCP connected"));

  String req = "GET ";
  req += RMS_RGB_PATH;
  req += " HTTP/1.1\r\nHost: ";
  req += RMS_HOST;
  req += "\r\nConnection: close\r\n\r\n";

  espFlushInput();

  LOG(F("[ESP TX] AT+CIPSEND="));
  LOGLN(req.length());

  ESP.print("AT+CIPSEND=");
  ESP.print(req.length());
  ESP.print("\r\n");

  if (!espWaitFor(">", 3000)) {
    LOGLN(F("[HTTP] ESP did not return CIPSEND prompt"));
    espCommand("AT+CIPCLOSE", "OK", 1000);
    return false;
  }

  LOGLN(F("[HTTP] Sending request"));
  ESP.print(req);

  // /api/summary большой. Держим только скользящее окно,
  // достаточное для обнаружения имени поля и его значения.
  String window;
  window.reserve(220);

  SummaryFields summary;
  resetSummaryFields(summary);

  uint32_t started = millis();
  uint32_t lastByteAt = started;
  bool sawAnyData = false;

  while (millis() - started < 8000) {

    while (ESP.available()) {
      char c = (char)ESP.read();

      sawAnyData = true;
      lastByteAt = millis();

      if (window.length() >= 210) {
        window.remove(0, 70);
      }

      window += c;

      // Не сканируем каждый символ с самого начала.
      // После 40+ символов окно уже содержит достаточно контекста.
      if (window.length() >= 40) {
        scanSummaryWindow(window, summary);
      }

      if (summaryHasRequiredFields(summary)) {
        // SOC может находиться позже в JSON, поэтому если его еще нет,
        // даем потоку еще немного времени.
        if (summary.gotSoc) {
          espCommand("AT+CIPCLOSE", "OK", 1000);
          LOGLN(F("[HTTP] Required fields + SOC received"));
          LOGLN(F("[HTTP] --------------------------------"));
          return applySummaryFields(summary);
        }
      }
    }

    // После прекращения потока применяем найденные обязательные поля.
    if (sawAnyData && millis() - lastByteAt > 350) {
      break;
    }
  }

  espCommand("AT+CIPCLOSE", "OK", 1000);

  if (!summaryHasRequiredFields(summary)) {
    LOGLN(F("[HTTP] /api/summary incomplete"));

    LOG(F("[HTTP] fields: PV=")); LOG(summary.gotPv);
    LOG(F(" GRID=")); LOG(summary.gotGrid);
    LOG(F(" LOAD=")); LOG(summary.gotLoad);
    LOG(F(" BAT=")); LOG(summary.gotBatPower);
    LOG(F(" BV=")); LOG(summary.gotBatVoltage);
    LOG(F(" SOC=")); LOG(summary.gotSoc);
    LOG(F(" DIS=")); LOG(summary.gotDischarging);
    LOG(F(" ETA=")); LOGLN(summary.gotEta);

    LOGLN(F("[HTTP] --------------------------------"));
    return false;
  }

  if (!summary.gotSoc) {
    LOGLN(F("[HTTP] battery_soc_pct not present; keeping previous SOC"));
  }

  bool ok = applySummaryFields(summary);

  LOGLN(F("[HTTP] --------------------------------"));

  return ok;

#endif
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

  Serial.begin(115200);
  delay(300);

  LOGLN(F(""));
  LOGLN(F("========================================"));
  LOGLN(F(" ANENJI RGB 64x32 + ESP8266 DIAGNOSTICS"));
  LOGLN(F("========================================"));

  matrix.begin();
  updateColors();

  drawDashboard();
  drawEnergyAnimation();

  LOGLN(F("[RGB] Matrix initialized"));

#if WIFI_ENABLE

  LOG(F("[ESP] Opening Serial3 at "));
  LOG(ESP_BAUD);
  LOGLN(F(" baud"));

  ESP.begin(ESP_BAUD);

  delay(500);

  if (!wifiConnect()) {
    LOGLN(F("[ESP] Initial connection failed; automatic retry enabled"));
  }

#else

  LOGLN(F("[ESP] WIFI_ENABLE=0"));

#endif
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  uint32_t now = millis();

  // ----------------------------------------------------------
  // Обычная анимация энергопотоков
  // ----------------------------------------------------------

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

  // ----------------------------------------------------------
  // Анимация заполнения батареи при разряде
  // ----------------------------------------------------------

  if (now - prevBatteryAnimTime >= BATTERY_ANIM_MS) {

    prevBatteryAnimTime = now;

    if (batteryDischarging && batteryPower > 10) {

      batteryFillPhase++;

      if (batteryFillPhase > 4) batteryFillPhase = 0;

      drawBattery(25, 26);

    } else {

      if (batteryFillPhase != 0) {
        batteryFillPhase = 0;
        drawBattery(25, 26);
      }
    }
  }

#if WIFI_ENABLE

  // ----------------------------------------------------------
  // Получение телеметрии с RMS7688A
  // ----------------------------------------------------------

  if (wifiReady && now - prevRmsPoll >= RMS_POLL_MS) {

    prevRmsPoll = now;

    if (fetchRmsData()) {

      httpFailCount = 0;
      LOGLN(F("[HTTP] Poll OK"));

    } else {

      httpFailCount++;

      LOG(F("[HTTP] Poll FAILED, consecutive failures: "));
      LOGLN(httpFailCount);

      if (httpFailCount >= 3) {
        LOGLN(F("[WIFI] Too many HTTP failures; forcing Wi-Fi reconnect"));
        wifiReady = false;
        prevWifiRetry = now;
      }
    }
  }

  // ----------------------------------------------------------
  // Повторное подключение Wi-Fi
  // ----------------------------------------------------------

  if (!wifiReady && now - prevWifiRetry >= WIFI_RETRY_MS) {

    prevWifiRetry = now;

    LOGLN(F("[WIFI] Automatic reconnect attempt"));

    if (!wifiConnect()) {
      LOGLN(F("[WIFI] Reconnect failed"));
    }
  }

#endif
}
