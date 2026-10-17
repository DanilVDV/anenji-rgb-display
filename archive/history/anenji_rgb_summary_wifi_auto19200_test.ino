#include <RGBmatrixPanel.h>
#include <avr/io.h>

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
// HUB75 refresh работает непрерывно; во время Wi-Fi/HTTP не отключается.
// ============================================================

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
int batterySoc = 78;   // ТЕСТОВЫЙ SOC, пока не берем из /api/summary

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

// ESP8266 AT UART. HUB75 refresh при обмене НЕ останавливается.
const unsigned long ESP_BOOT_BAUD = 115200;
const unsigned long ESP_BAUD = 19200;

// !!! ЗАМЕНИТЬ НА ПАРАМЕТРЫ ТОЧКИ ДОСТУПА RMS7688A !!!
const char RMS_WIFI_SSID[] = "ap";
const char RMS_WIFI_PASS[] = "<WIFI_PASSWORD>";

// !!! ЗАМЕНИТЬ, ЕСЛИ RMS ИМЕЕТ ДРУГОЙ IP !!!
const char RMS_HOST[] = "192.168.54.30";
const uint16_t RMS_HTTP_PORT = 8088;
const char RMS_RGB_PATH[] = "/api/summary";

const uint32_t RMS_POLL_MS = 2000;
const uint32_t WIFI_RETRY_MS = 15000;
const uint16_t ESP_PASSIVE_CHUNK = 64;
const uint16_t ESP_PASSIVE_POLL_MS = 25;
const uint32_t HTTP_TOTAL_TIMEOUT_MS = 8000;
const uint16_t WIFI_POST_CONNECT_SETTLE_MS = 700;

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
// ТИПЫ ДАННЫХ
//
// ВАЖНО: структура объявлена ДО всех функций.
// Arduino IDE автоматически генерирует прототипы функций,
// поэтому пользовательские типы должны быть известны заранее.
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
// ESP8266 AT: НАДЕЖНЫЙ ПРИЕМ
//
// RGBmatrixPanel может иногда "съедать" отдельные символы UART
// на 115200. Поэтому:
// - AT-проверка остается строгой;
// - конфигурационные команды допускают поврежденный финальный OK;
// - CWJAP считается успешным при WIFI GOT IP;
// - ожидание заканчивается по тихому интервалу, а не висит секунды.
// ============================================================

const uint16_t ESP_QUIET_MS = 120;
const uint16_t ESP_SHORT_TIMEOUT_MS = 1200;
const uint16_t ESP_JOIN_TIMEOUT_MS = 18000;
const uint16_t HTTP_CONNECT_TIMEOUT_MS = 2500;
const uint16_t HTTP_SEND_TIMEOUT_MS = 1500;
const uint16_t HTTP_BODY_TIMEOUT_MS = 3000;
const uint16_t HTTP_IDLE_DONE_MS = 220;

void espFlushInput() {
  while (ESP.available()) ESP.read();
}

void logEspResponse(const String &r) {
#if DEBUG_ESP_RAW
  if (!r.length()) {
    LOGLN(F("[ESP RX] <empty>"));
    return;
  }

  String line;
  line.reserve(120);

  for (unsigned int i = 0; i < r.length(); i++) {
    char c = r[i];

    if (c == '\r') continue;

    if (c == '\n') {
      if (line.length()) {
        LOG(F("[ESP RX] "));
        LOGLN(line);
        line = "";
      }
    } else {
      if (line.length() < 119) line += c;
    }
  }

  if (line.length()) {
    LOG(F("[ESP RX] "));
    LOGLN(line);
  }
#endif
}

String espCollect(uint32_t timeoutMs, uint16_t quietMs = ESP_QUIET_MS) {
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

  logEspResponse(r);
  return r;
}

String espSendCommand(const String &cmd, uint32_t timeoutMs) {
  espFlushInput();

  LOG(F("[ESP TX] "));

  if (cmd.startsWith("AT+CWJAP=")) {
    LOGLN(F("AT+CWJAP=<SSID/PASSWORD hidden>"));
  } else {
    LOGLN(cmd);
  }

  ESP.print(cmd);
  ESP.print("\r\n");

  return espCollect(timeoutMs);
}

bool responseHas(const String &r, const char *token) {
  return r.indexOf(token) >= 0;
}

bool espProbe() {
  String r = espSendCommand("AT", ESP_SHORT_TIMEOUT_MS);
  return responseHas(r, "OK");
}

bool espConfigCommand(const String &cmd) {
  String r = espSendCommand(cmd, ESP_SHORT_TIMEOUT_MS);

  if (responseHas(r, "OK") || responseHas(r, "no change")) return true;
  if (responseHas(r, "ERROR") || responseHas(r, "FAIL")) return false;

  // Финальный O/K мог потеряться. Проверяем, что модем жив.
  LOGLN(F("[ESP] Damaged config reply; probing modem with AT"));

  delay(40);

  if (espProbe()) {
    LOGLN(F("[ESP] Modem responds; continuing"));
    return true;
  }

  return false;
}


// ============================================================
// ESP8266: НАДЕЖНЫЙ PROBE
// ============================================================

bool espProbeReliable() {

  for (uint8_t attempt = 1; attempt <= 3; attempt++) {

    LOG(F("[ESP] Probe attempt "));
    LOGLN(attempt);

    if (espProbe()) return true;

    delay(100);
    espFlushInput();
  }

  return false;
}


// ============================================================
// ESP8266: ОТДЕЛЬНОЕ ОЖИДАНИЕ CWJAP
//
// ВАЖНО:
// AT+CWJAP может делать паузы между:
//   WIFI DISCONNECT
//   WIFI CONNECTED
//   WIFI GOT IP
//   OK
//
// Поэтому общий quiet-time здесь НЕ используется.
// Ждем до 20 секунд именно конечного результата.
// ============================================================

bool espJoinWifi(const String &cmd) {

  espFlushInput();

  LOG(F("[ESP TX] "));
  LOGLN(F("AT+CWJAP=<SSID/PASSWORD hidden>"));

  ESP.print(cmd);
  ESP.print("\r\n");

  String response;
  String line;

  response.reserve(260);
  line.reserve(120);

  uint32_t started = millis();

  bool sawConnected = false;
  bool sawGotIp = false;

  while (millis() - started < ESP_JOIN_TIMEOUT_MS) {

    while (ESP.available()) {

      char c = (char)ESP.read();

      if (response.length() < 259) {
        response += c;
      } else {
        response.remove(0, 90);
        response += c;
      }

#if DEBUG_ESP_RAW
      if (c == '\r') {
        // ignore
      } else if (c == '\n') {
        if (line.length()) {
          LOG(F("[ESP RX] "));
          LOGLN(line);
          line = "";
        }
      } else {
        if (line.length() < 119) line += c;
      }
#endif

      if (response.indexOf("WIFI CONNECTED") >= 0 ||
          response.indexOf("WIF CONNECTED") >= 0) {
        sawConnected = true;
      }

      if (response.indexOf("WIFI GOT IP") >= 0 ||
          response.indexOf("WIF GOT IP") >= 0 ||
          response.indexOf("WIFI GOT") >= 0 ||
          response.indexOf("GOT IP") >= 0) {
        sawGotIp = true;
      }

      if (response.indexOf("+CWJAP:") >= 0 ||
          response.indexOf("FAIL") >= 0 ||
          response.indexOf("ERROR") >= 0) {

#if DEBUG_ESP_RAW
        if (line.length()) {
          LOG(F("[ESP RX] "));
          LOGLN(line);
        }
#endif

        LOGLN(F("[WIFI] JOIN FAILED by ESP response"));
        return false;
      }

      // GOT IP уже является достаточным доказательством подключения.
      if (sawGotIp) {

        // Небольшая пауза, чтобы дочитать финальный OK,
        // но не зависеть от него.
        uint32_t settle = millis();

        while (millis() - settle < 250) {
          while (ESP.available()) {
            char c2 = (char)ESP.read();

#if DEBUG_ESP_RAW
            if (c2 == '\r') {
              // ignore
            } else if (c2 == '\n') {
              if (line.length()) {
                LOG(F("[ESP RX] "));
                LOGLN(line);
                line = "";
              }
            } else {
              if (line.length() < 119) line += c2;
            }
#endif
          }
        }

#if DEBUG_ESP_RAW
        if (line.length()) {
          LOG(F("[ESP RX] "));
          LOGLN(line);
        }
#endif

        LOGLN(F("[WIFI] WIFI GOT IP detected"));
        return true;
      }
    }
  }

#if DEBUG_ESP_RAW
  if (line.length()) {
    LOG(F("[ESP RX] "));
    LOGLN(line);
  }
#endif

  // Если CONNECTED был, но GOT IP потерялся из-за UART,
  // только ТЕПЕРЬ, после завершения ожидания CWJAP,
  // проверяем фактический IP.
  if (sawConnected) {
    LOGLN(F("[WIFI] CONNECTED seen but GOT IP missing; checking CIFSR"));

    delay(150);

    String ipr = espSendCommand("AT+CIFSR", 1800);

    if (responseHas(ipr, "STAIP") && !responseHas(ipr, "0.0.0.0")) {
      LOGLN(F("[WIFI] Station IP confirmed by CIFSR"));
      return true;
    }
  }

  LOGLN(F("[WIFI] CWJAP timed out"));
  return false;
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

  if (!espProbeReliable()) {
    LOGLN(F("[WIFI] ESP8266 did not answer AT"));
    LOGLN(F("[WIFI] Check DIP switches, Serial3 and ESP baud rate"));
    return false;
  }

  LOGLN(F("[WIFI] ESP8266 responds"));

  // Echo OFF. Поврежденный ответ не критичен.
  espConfigCommand("ATE0");

  LOGLN(F("[WIFI] Setting station mode..."));

  if (!espConfigCommand("AT+CWMODE=1")) {
    LOGLN(F("[WIFI] Failed to set station mode"));
    return false;
  }

  // Один TCP-канал и passive receive mode.
  // В passive mode ESP8266 буферизует TCP-данные у себя,
  // а Mega забирает их небольшими блоками через CIPRECVDATA.
  // Матрица при этом продолжает обновляться постоянно.
  if (!espConfigCommand("AT+CIPMUX=0")) {
    LOGLN(F("[WIFI] Failed to set CIPMUX=0"));
    return false;
  }

  if (!espConfigCommand("AT+CIPRECVMODE=1")) {
    LOGLN(F("[WIFI] Passive receive mode is not available"));
    return false;
  }

  String join = "AT+CWJAP=\"";
  join += RMS_WIFI_SSID;
  join += "\",\"";
  join += RMS_WIFI_PASS;
  join += "\"";

  LOGLN(F("[WIFI] Joining RMS7688A access point..."));

  if (!espJoinWifi(join)) {
    LOGLN(F("[WIFI] Wi-Fi join FAILED"));
    return false;
  }

  wifiReady = true;
  httpFailCount = 0;

  LOGLN(F("[WIFI] Wi-Fi connected successfully"));
  LOG(F("[WIFI] Waiting "));
  LOG(WIFI_POST_CONNECT_SETTLE_MS);
  LOGLN(F(" ms before first TCP request"));

  delay(WIFI_POST_CONNECT_SETTLE_MS);
  espFlushInput();

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

  // SOC пока оставляем тестовым и НЕ обновляем из API.
  // Когда решим использовать реальный SOC, достаточно вернуть:
  // if (s.gotSoc) batterySoc = s.soc;

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
  LOG(F(" V  SOC(test)=")); LOG(batterySoc); LOG(F("%"));
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

// ============================================================
// ESP8266 PASSIVE RECEIVE
// ============================================================

// Ответ AT+CIPRECVLEN? ожидается примерно:
// +CIPRECVLEN:1234
// OK
//
// Возвращает:
// >=0  количество доступных TCP байт
// -1   не удалось разобрать ответ
long espPassiveAvailable() {

  String r = espSendCommand("AT+CIPRECVLEN?", 900);

  int p = r.indexOf("+CIPRECVLEN:");
  if (p < 0) return -1;

  p += 12;

  while (p < (int)r.length() && (r[p] == ' ' || r[p] == '\t')) p++;

  String n;
  n.reserve(12);

  while (p < (int)r.length() && r[p] >= '0' && r[p] <= '9') {
    n += r[p++];
  }

  if (!n.length()) return -1;

  return n.toInt();
}


// Забирает до requested байт через:
// AT+CIPRECVDATA=<n>
//
// Ответ обычно:
// +CIPRECVDATA,<actual>:<raw data>
// OK
//
// В out возвращаются только сырые TCP-данные после ':'.
bool espPassiveRead(uint16_t requested, String &out) {

  out = "";

  String cmd = "AT+CIPRECVDATA=";
  cmd += String(requested);

  espFlushInput();

  LOG(F("[ESP TX] "));
  LOGLN(cmd);

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

      if (r.length() < requested + 80) {
        r += c;
      }
    }

    if (gotAny && millis() - lastByte > 80) break;
  }

  int p = r.indexOf("+CIPRECVDATA,");
  if (p < 0) {
    LOG(F("[ESP] CIPRECVDATA parse failed, bytes="));
    LOGLN(r.length());
    return false;
  }

  int colon = r.indexOf(':', p);
  if (colon < 0) {
    LOGLN(F("[ESP] CIPRECVDATA missing ':'"));
    return false;
  }

  // Число реально возвращенных байт находится между ',' и ':'.
  int comma = r.indexOf(',', p);
  if (comma < 0 || comma > colon) return false;

  String ns = r.substring(comma + 1, colon);
  int actual = ns.toInt();

  if (actual < 0) return false;

  int dataStart = colon + 1;

  // r может содержать завершающие \r\nOK\r\n после payload.
  // Берем строго actual байт.
  if (dataStart + actual > (int)r.length()) {
    LOG(F("[ESP] CIPRECVDATA short payload: expected "));
    LOG(actual);
    LOG(F(", got "));
    LOGLN((int)r.length() - dataStart);
    return false;
  }

  out.reserve(actual);

  for (int i = 0; i < actual; i++) {
    out += r[dataStart + i];
  }

  return true;
}


// ============================================================
// HTTP /api/summary через PASSIVE RECEIVE
// ============================================================

bool fetchRmsData() {

#if !WIFI_ENABLE
  return false;
#else

  if (!wifiReady) return false;

  LOGLN(F("[HTTP] --------------------------------"));
  LOG(F("[HTTP] GET http://"));
  LOG(RMS_HOST);
  LOG(F(":"));
  LOG(RMS_HTTP_PORT);
  LOGLN(RMS_RGB_PATH);

  // На случай оставшегося TCP-сеанса.
  ESP.print("AT+CIPCLOSE\r\n");
  delay(40);
  espFlushInput();

  String cip = "AT+CIPSTART=\"TCP\",\"";
  cip += RMS_HOST;
  cip += "\",";
  cip += String(RMS_HTTP_PORT);

  String cr = espSendCommand(cip, 3000);

  bool connected = responseHas(cr, "CONNECT") ||
                   responseHas(cr, "ALREADY CONNECTED") ||
                   responseHas(cr, "OK");

  if (!connected || responseHas(cr, "ERROR") || responseHas(cr, "FAIL") || responseHas(cr, "busy")) {
    LOGLN(F("[HTTP] TCP connection FAILED"));
    return false;
  }

  // HTTP/1.0 + Connection: close упрощает завершение ответа.
  String req = "GET ";
  req += RMS_RGB_PATH;
  req += " HTTP/1.0\r\nHost: ";
  req += RMS_HOST;
  req += "\r\nConnection: close\r\n\r\n";

  espFlushInput();

  LOG(F("[ESP TX] AT+CIPSEND="));
  LOGLN(req.length());

  ESP.print("AT+CIPSEND=");
  ESP.print(req.length());
  ESP.print("\r\n");

  String sr = espCollect(1500, 80);

  if (sr.indexOf('>') < 0) {
    LOGLN(F("[HTTP] No CIPSEND prompt"));
    return false;
  }

  ESP.print(req);

  // Ждем SEND OK. При этом сам HTTP payload остается в буфере ESP,
  // потому что включен CIPRECVMODE=1.
  String sendReply = espCollect(1800, 120);

  if (responseHas(sendReply, "SEND OK")) {
    LOGLN(F("[HTTP] SEND OK"));
  } else {
    LOGLN(F("[HTTP] SEND OK not seen; continuing in passive mode"));
  }

  SummaryFields summary;
  resetSummaryFields(summary);

  String parseWindow;
  parseWindow.reserve(260);

  bool sawHttp200 = false;
  bool gotTcpData = false;
  uint32_t started = millis();
  uint32_t lastDataAt = started;
  uint16_t emptyPolls = 0;

  while (millis() - started < HTTP_TOTAL_TIMEOUT_MS) {

    long available = espPassiveAvailable();

    if (available < 0) {
      delay(ESP_PASSIVE_POLL_MS);
      continue;
    }

    if (available == 0) {
      emptyPolls++;

      // После того как уже получили данные, несколько пустых опросов
      // означают окончание HTTP-ответа.
      if (gotTcpData && emptyPolls >= 4) break;

      delay(ESP_PASSIVE_POLL_MS);
      continue;
    }

    emptyPolls = 0;

    uint16_t want = (available > ESP_PASSIVE_CHUNK) ? ESP_PASSIVE_CHUNK : (uint16_t)available;

    String chunk;

    if (!espPassiveRead(want, chunk)) {
      LOGLN(F("[HTTP] Passive read failed"));
      return false;
    }

    gotTcpData = true;
    lastDataAt = millis();

    // НИКАКОГО RAW-лога chunk во время приема:
    // это специально, чтобы USB Serial не тормозил чтение Serial3.

    if (parseWindow.length() + chunk.length() > 250) {
      int removeCount = (parseWindow.length() + chunk.length()) - 220;
      if (removeCount > 0 && removeCount < (int)parseWindow.length()) {
        parseWindow.remove(0, removeCount);
      }
    }

    parseWindow += chunk;

    if (!sawHttp200 &&
        (parseWindow.indexOf("HTTP/1.0 200") >= 0 ||
         parseWindow.indexOf("HTTP/1.1 200") >= 0)) {
      sawHttp200 = true;
    }

    scanSummaryWindow(parseWindow, summary);

    // Обязательные поля найдены. SOC может отсутствовать в summary,
    // поэтому ждать его специально не будем.
    if (summaryHasRequiredFields(summary)) {
      // Дочитывание всего остатка не требуется для панели.
      break;
    }

    // Защита от редкой ситуации, когда данные перестали приходить.
    if (millis() - lastDataAt > 1500) break;
  }

  // Закрываем TCP без блокирующего ожидания.
  ESP.print("AT+CIPCLOSE\r\n");

  LOG(F("[HTTP] HTTP200="));
  LOG(sawHttp200);
  LOG(F(" DATA="));
  LOGLN(gotTcpData);

  if (!gotTcpData) {
    LOGLN(F("[HTTP] No TCP payload"));
    return false;
  }

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

    return false;
  }

  bool ok = applySummaryFields(summary);

  if (ok) {
    LOGLN(F("[HTTP] Poll OK"));
  } else {
    LOGLN(F("[HTTP] Apply FAILED"));
  }

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
// ESP8266: ПЕРЕХОД 115200 -> 19200 ДО ЗАПУСКА МАТРИЦЫ
//
// Используем AT+UART_CUR, чтобы не менять постоянную настройку ESP.
// После перезагрузки ESP снова стартует на 115200, а Mega каждый раз
// сама переводит его на рабочие 19200 до matrix.begin().
// ============================================================

bool espSwitchToWorkBaud() {

#if !WIFI_ENABLE
  return false;
#else

  LOGLN(F("[ESP] Boot UART setup"));
  LOG(F("[ESP] Opening Serial3 at boot baud "));
  LOGLN(ESP_BOOT_BAUD);

  ESP.begin(ESP_BOOT_BAUD);
  delay(350);

  espFlushInput();

  bool foundBootBaud = false;

  for (uint8_t i = 0; i < 3; i++) {
    String r = espSendCommand("AT", 1000);

    if (responseHas(r, "OK")) {
      foundBootBaud = true;
      break;
    }

    delay(100);
  }

  if (!foundBootBaud) {
    LOGLN(F("[ESP] No AT response at 115200; trying current work baud"));

    ESP.end();
    delay(80);
    ESP.begin(ESP_BAUD);
    delay(250);

    for (uint8_t i = 0; i < 3; i++) {
      String r = espSendCommand("AT", 1000);

      if (responseHas(r, "OK")) {
        LOGLN(F("[ESP] ESP already works at 19200"));
        return true;
      }

      delay(100);
    }

    LOGLN(F("[ESP] ESP not responding at 115200 or 19200"));
    return false;
  }

  LOGLN(F("[ESP] ESP detected at 115200"));
  LOGLN(F("[ESP] Switching current UART to 19200"));

  String ur = espSendCommand("AT+UART_CUR=19200,8,1,0,0", 1500);

  // После UART_CUR финальный OK может уже уйти на новой скорости,
  // поэтому не требуем идеального ответа.
  delay(100);

  ESP.end();
  delay(80);
  ESP.begin(ESP_BAUD);
  delay(300);

  espFlushInput();

  for (uint8_t i = 0; i < 4; i++) {
    String r = espSendCommand("AT", 1200);

    if (responseHas(r, "OK")) {
      LOGLN(F("[ESP] Work UART 19200 confirmed"));
      return true;
    }

    delay(120);
  }

  LOGLN(F("[ESP] Failed to confirm 19200"));
  return false;

#endif
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(300);

  LOGLN(F(""));
  LOGLN(F("========================================"));
  LOGLN(F(" ANENJI RGB 64x32 + ESP8266 TEST"));
  LOGLN(F("========================================"));
  LOGLN(F("[RGB] Continuous refresh: matrix is NEVER paused"));
  LOG(F("[RMS] Test SOC = ")); LOG(batterySoc); LOGLN(F("%"));

#if WIFI_ENABLE

  bool espReady = espSwitchToWorkBaud();

#else

  bool espReady = false;

#endif

  // Матрица запускается ПОСЛЕ настройки скорости ESP.
  matrix.begin();
  updateColors();

  drawDashboard();
  drawEnergyAnimation();

  LOGLN(F("[RGB] Matrix initialized"));

#if WIFI_ENABLE

  if (!espReady) {
    LOGLN(F("[ESP] UART setup failed; reconnect attempts will continue"));
    wifiReady = false;
  } else {
    LOG(F("[ESP] Serial3 work baud = "));
    LOGLN(ESP_BAUD);

    if (!wifiConnect()) {
      LOGLN(F("[ESP] Initial connection failed; automatic retry enabled"));
    }
  }

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
