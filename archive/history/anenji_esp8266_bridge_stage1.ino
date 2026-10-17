#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>

// ============================================================
// ANENJI ESP8266 BRIDGE — STAGE 1
//
// ESP8266:
//   - получает Wi-Fi/RMS параметры от ATmega2560 по UART;
//   - сохраняет последний CFG в EEPROM;
//   - сам подключается к Wi-Fi;
//   - сам опрашивает /api/rgb;
//   - отправляет Mega только короткий пакет DATA.
//
// UART: 19200 8N1
// ============================================================

static const uint32_t UART_BAUD = 19200;
static const uint32_t DEFAULT_POLL_MS = 5000;
static const uint32_t WIFI_RETRY_MS = 10000;
static const uint32_t CFG_REQUEST_MS = 3000;

struct Config {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  char host[40];
  uint16_t port;
  char path[48];
  uint32_t pollMs;
};

static const uint32_t CFG_MAGIC = 0x414E4A31UL; // "ANJ1"

Config cfg;
bool cfgValid = false;

uint32_t lastPollAt = 0;
uint32_t lastWifiRetryAt = 0;
uint32_t lastCfgRequestAt = 0;

String rxLine;


// ============================================================
// CRC16
// ============================================================

uint16_t crc16(const uint8_t *data, size_t len) {
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


// ============================================================
// EEPROM
// ============================================================

void loadConfig() {
  EEPROM.begin(sizeof(Config));

  EEPROM.get(0, cfg);

  if (cfg.magic == CFG_MAGIC &&
      strlen(cfg.ssid) > 0 &&
      strlen(cfg.host) > 0 &&
      cfg.port > 0 &&
      strlen(cfg.path) > 0) {

    cfgValid = true;
  }
}

void saveConfig() {
  cfg.magic = CFG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
  cfgValid = true;
}


// ============================================================
// UART helpers
// ============================================================

void sendLine(const String &s) {
  Serial.print(s);
  Serial.print('\n');
}

void requestConfig() {
  sendLine("REQCFG");
}

bool copyField(char *dst, size_t dstSize, const String &src) {
  if (src.length() + 1 > dstSize) return false;
  src.toCharArray(dst, dstSize);
  return true;
}


// CFG|ssid|pass|host|port|path|poll_ms
bool parseConfigLine(const String &line) {

  if (!line.startsWith("CFG|")) return false;

  String parts[6];
  int start = 4;

  for (uint8_t i = 0; i < 6; i++) {
    int sep = line.indexOf('|', start);

    if (i == 5) {
      if (start >= (int)line.length()) return false;
      parts[i] = line.substring(start);
      break;
    }

    if (sep < 0) return false;

    parts[i] = line.substring(start, sep);
    start = sep + 1;
  }

  Config next = {};

  if (!copyField(next.ssid, sizeof(next.ssid), parts[0])) return false;
  if (!copyField(next.pass, sizeof(next.pass), parts[1])) return false;
  if (!copyField(next.host, sizeof(next.host), parts[2])) return false;

  next.port = (uint16_t)parts[3].toInt();

  if (!copyField(next.path, sizeof(next.path), parts[4])) return false;

  next.pollMs = parts[5].toInt();

  if (!next.port) return false;
  if (next.pollMs < 1000) next.pollMs = DEFAULT_POLL_MS;

  cfg = next;
  saveConfig();

  sendLine("CFGOK");

  WiFi.disconnect();
  delay(50);

  return true;
}

void handleUart() {

  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      if (rxLine.length()) {
        parseConfigLine(rxLine);
        rxLine = "";
      }
    } else {
      if (rxLine.length() < 220) rxLine += c;
      else rxLine = "";
    }
  }
}


// ============================================================
// Wi-Fi
// ============================================================

void ensureWifi() {

  if (!cfgValid) return;

  if (WiFi.status() == WL_CONNECTED) return;

  uint32_t now = millis();

  if (now - lastWifiRetryAt < WIFI_RETRY_MS) return;

  lastWifiRetryAt = now;

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);

  sendLine("WIFI|CONNECTING");
}


// ============================================================
// Compact JSON helpers
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

    if ((c >= '0' && c <= '9') ||
        c == '-' || c == '+' ||
        c == '.' || c == 'e' || c == 'E') {
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


// DATA|p|l|g|b|v10|soc|d|a|eta|s|HH:MM|crc
void sendRgbData(const String &body) {

  double p,l,g,b,v,c,d,a,e,s;
  String t;

  if (!jsonNumber(body, "p", p)) return;
  if (!jsonNumber(body, "l", l)) return;
  if (!jsonNumber(body, "g", g)) return;
  if (!jsonNumber(body, "b", b)) return;
  if (!jsonNumber(body, "v", v)) return;
  if (!jsonNumber(body, "c", c)) return;
  if (!jsonNumber(body, "d", d)) return;
  if (!jsonNumber(body, "a", a)) return;
  if (!jsonNumber(body, "e", e)) return;
  if (!jsonString(body, "t", t)) return;
  if (!jsonNumber(body, "s", s)) return;

  String payload = "DATA|";
  payload += String((long)p); payload += "|";
  payload += String((long)l); payload += "|";
  payload += String((long)g); payload += "|";
  payload += String((long)b); payload += "|";
  payload += String((long)(v * 10.0 + (v >= 0 ? 0.5 : -0.5))); payload += "|";
  payload += String((int)c); payload += "|";
  payload += String((int)d); payload += "|";
  payload += String((int)a); payload += "|";
  payload += String((long)e); payload += "|";
  payload += String((int)s); payload += "|";
  payload += t;

  uint16_t crc = crc16((const uint8_t*)payload.c_str(), payload.length());

  payload += "|";
  payload += String(crc, HEX);

  sendLine(payload);
}


// ============================================================
// HTTP
// ============================================================

void pollRms() {

  if (!cfgValid) return;
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClient client;
  HTTPClient http;

  String url = "http://";
  url += cfg.host;
  url += ":";
  url += String(cfg.port);
  url += cfg.path;

  if (!http.begin(client, url)) {
    sendLine("ERR|HTTP_BEGIN");
    return;
  }

  http.setTimeout(2500);

  int code = http.GET();

  if (code == HTTP_CODE_OK) {
    String body = http.getString();
    sendRgbData(body);
  } else {
    String err = "ERR|HTTP|";
    err += String(code);
    sendLine(err);
  }

  http.end();
}


// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {

  Serial.begin(UART_BAUD);
  Serial.setTimeout(20);

  loadConfig();

  delay(100);

  requestConfig();
}

void loop() {

  handleUart();

  if (!cfgValid) {
    uint32_t now = millis();

    if (now - lastCfgRequestAt >= CFG_REQUEST_MS) {
      lastCfgRequestAt = now;
      requestConfig();
    }

    delay(2);
    return;
  }

  ensureWifi();

  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastPollAt >= cfg.pollMs) {
      lastPollAt = now;
      pollRms();
    }
  }

  delay(2);
}
