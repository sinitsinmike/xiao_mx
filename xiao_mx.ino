#include <Arduino.h>
#include <MIDIUSB.h>
#include <FlashStorage.h>

const int LED = 13;

static const uint32_t CFG_MAGIC   = 0x4D584343; // 'MXCC'
static const uint8_t  CFG_VERSION = 1;

struct Config {
  uint32_t magic;
  uint8_t  version;
  uint8_t  midiChannel;  // 1..16
  uint8_t  ccPots[5];
  uint8_t  ccBtns[6];
  uint8_t  reserved[8];
  uint16_t crc;
};

FlashStorage(config_store, Config);
Config cfg;

uint8_t pot_prev[5];
uint8_t btn_prev[6];
String lineBuf;

static uint16_t checksum16(const uint8_t* data, size_t len) {
  uint32_t s = 0;
  for (size_t i = 0; i < len; i++) s += data[i];
  return (uint16_t)(s & 0xFFFF);
}
static uint16_t calcCRC(const Config& c) {
  return checksum16((const uint8_t*)&c, sizeof(Config) - sizeof(c.crc));
}
static void setDefaults(Config& c) {
  c.magic = CFG_MAGIC;
  c.version = CFG_VERSION;
  c.midiChannel = 1;
  for (int i = 0; i < 5; i++) c.ccPots[i] = (uint8_t)(20 + i); // 20..24
  for (int i = 0; i < 6; i++) c.ccBtns[i] = (uint8_t)(40 + i); // 40..45
  memset(c.reserved, 0, sizeof(c.reserved));
  c.crc = calcCRC(c);
}
static bool loadConfig() {
  Config t = config_store.read();
  if (t.magic != CFG_MAGIC) return false;
  if (t.version != CFG_VERSION) return false;
  if (t.crc != calcCRC(t)) return false;
  if (t.midiChannel < 1 || t.midiChannel > 16) return false;
  for (int i = 0; i < 5; i++) if (t.ccPots[i] > 127) return false;
  for (int i = 0; i < 6; i++) if (t.ccBtns[i] > 127) return false;
  cfg = t;
  return true;
}
static void saveConfig() {
  cfg.crc = calcCRC(cfg);
  config_store.write(cfg);
}

static void sendCC(uint8_t cc, uint8_t val, uint8_t ch) {
  midiEventPacket_t ev = {
    0x0B,
    uint8_t(0xB0 | ((ch - 1) & 0x0F)),
    uint8_t(cc & 0x7F),
    uint8_t(val & 0x7F)
  };
  MidiUSB.sendMIDI(ev);
  MidiUSB.flush();
}

static void printConfig(Stream& s) {
  s.println(F("CONFIG"));
  s.print(F("CH=")); s.println(cfg.midiChannel);
  for (int i = 0; i < 5; i++) { s.print(F("P")); s.print(i); s.print(F("=")); s.println(cfg.ccPots[i]); }
  for (int i = 0; i < 6; i++) { s.print(F("B")); s.print(i); s.print(F("=")); s.println(cfg.ccBtns[i]); }
  s.println(F("OK"));
}

static void handleCommand(String cmdLine) {
  cmdLine.trim();
  if (!cmdLine.length()) return;
  String up = cmdLine; up.toUpperCase();

  if (up == "GET") { printConfig(Serial); return; }
  if (up == "SAVE") { saveConfig(); Serial.println(F("OK SAVED")); return; }
  if (up == "DEFAULTS") { setDefaults(cfg); saveConfig(); Serial.println(F("OK DEFAULTS")); printConfig(Serial); return; }

  if (up.startsWith("SET ")) {
    int sp1 = cmdLine.indexOf(' ');
    int sp2 = cmdLine.indexOf(' ', sp1 + 1);
    if (sp2 < 0) { Serial.println(F("ERR usage")); return; }

    String target = cmdLine.substring(sp1 + 1, sp2); target.trim();
    String valueS = cmdLine.substring(sp2 + 1); valueS.trim();
    String t = target; t.toUpperCase();
    int v = valueS.toInt();

    if (t == "CH") { cfg.midiChannel = (uint8_t)constrain(v, 1, 16); Serial.println(F("OK")); return; }

    if (t.length() == 2 && t[0] == 'P') {
      int idx = t[1] - '0';
      if (idx >= 0 && idx <= 4) { cfg.ccPots[idx] = (uint8_t)constrain(v, 0, 127); Serial.println(F("OK")); return; }
    }
    if (t.length() == 2 && t[0] == 'B') {
      int idx = t[1] - '0';
      if (idx >= 0 && idx <= 5) { cfg.ccBtns[idx] = (uint8_t)constrain(v, 0, 127); Serial.println(F("OK")); return; }
    }
    Serial.println(F("ERR bad target"));
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { handleCommand(lineBuf); lineBuf = ""; }
    else if (lineBuf.length() < 200) lineBuf += c;
  }
}

static inline uint8_t readPot7(int pin) {
  uint16_t v = analogRead(pin);   // 0..63 (если res=6)
  uint16_t d = (v << 1);          // 0..126
  if (d > 127) d = 127;
  return (uint8_t)d;
}

void setup() {
  pinMode(LED, OUTPUT);
  analogReadResolution(6);

  for (int i = 0; i < 6; i++) { pinMode(5 + i, INPUT_PULLUP); btn_prev[i] = 1; }
  for (int i = 0; i < 5; i++) pot_prev[i] = 255;

  Serial.begin(115200);
  delay(250);

  if (!loadConfig()) { setDefaults(cfg); saveConfig(); }
  printConfig(Serial);
}

void loop() {
  pollSerial();

  for (int i = 0; i < 5; i++) {
    uint8_t v = readPot7(A0 + i);
    if (v != pot_prev[i]) {
      pot_prev[i] = v;
      sendCC(cfg.ccPots[i], v, cfg.midiChannel);
      delay(3);
    }
  }

  for (int i = 0; i < 6; i++) {
    uint8_t s = (uint8_t)digitalRead(5 + i);
    if (s != btn_prev[i]) {
      btn_prev[i] = s;
      uint8_t v = (uint8_t)((1 - s) * 127);
      sendCC(cfg.ccBtns[i], v, cfg.midiChannel);
      digitalWrite(LED, s);
      delay(3);
    }
  }
}
