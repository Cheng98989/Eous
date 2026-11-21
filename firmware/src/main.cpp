#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <atomic>
#include <math.h>

// Eous: mouse BLE HID. Il computer usa il proprio supporto mouse Bluetooth.
// MPU6050: SDA -> GPIO8, SCL -> GPIO9, AD0 -> GND, massa comune.
// Pulsanti: chiudere il contatto tra il relativo GPIO e GND.
// Calibrazione e regolazioni dal PC, via seriale: tools/eous_tune.py.
constexpr uint8_t LEFT_PIN = 15;
constexpr uint8_t RIGHT_PIN = 16;
constexpr uint8_t UNUSED_PINS[] = {17, 18};
constexpr int SDA_PIN = 8;
constexpr int SCL_PIN = 9;
constexpr uint8_t MPU_ADDR = 0x68;
constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint32_t SEND_MS = 20;
constexpr float LSB_PER_DPS = 32.8f;   // Scala +-1000 gradi/s: a +-250 un movimento rapido del polso satura.
constexpr float LSB_PER_G = 16384.0f;  // Scala +-2 g dell'accelerometro.

// Parametri regolabili via seriale senza ricompilare. Il comando "save" scrive
// nella memoria flash quelli fino a SAVED_PARAMS; l'offset del giroscopio cambia
// con la temperatura, quindi viene rimisurato a ogni avvio.
enum ParamId {
  SENS, DEADZONE, ACC_LO, ACC_HI, ACC_MAX,
  X_GX, X_GY, X_GZ, Y_GX, Y_GY, Y_GZ,
  SAVED_PARAMS,
  BIAS_X = SAVED_PARAMS, BIAS_Y, BIAS_Z,
  PARAM_COUNT
};

struct Param {
  const char* name;
  float initial;
  float value = 0;
};

static Param params[PARAM_COUNT] = {
    {"sens", 10.0f},     // Pixel per grado di rotazione.
    {"deadzone", 1.0f},  // Gradi/s ignorati a riposo.
    {"acc_lo", 50.0f},   // Sotto questa velocita' (gradi/s) nessuna accelerazione.
    {"acc_hi", 400.0f},  // Da questa velocita' in su l'accelerazione e' massima.
    {"acc_max", 1.0f},   // Moltiplicatore massimo; 1 = accelerazione spenta.
    // Cursore X = x_gx*gx + x_gy*gy + x_gz*gz (positivo = destra);
    // cursore Y = y_gx*gx + y_gy*gy + y_gz*gz (positivo = giu').
    {"x_gx", 0.0f}, {"x_gy", 0.0f}, {"x_gz", -1.0f},
    {"y_gx", -1.0f}, {"y_gy", 0.0f}, {"y_gz", 0.0f},
    {"bias_x", 0.0f}, {"bias_y", 0.0f}, {"bias_z", 0.0f},  // Offset a riposo, gradi/s.
};

struct Button {
  uint8_t pin;
  bool raw = false;
  bool pressed = false;
  uint32_t changedAt = 0;

  void begin() { pinMode(pin, INPUT_PULLUP); }

  void update(uint32_t now) {
    const bool reading = digitalRead(pin) == LOW;
    if (reading != raw) {
      raw = reading;
      changedAt = now;
    }
    if (now - changedAt >= DEBOUNCE_MS) pressed = raw;
  }
};

static Button leftButton{LEFT_PIN};
static Button rightButton{RIGHT_PIN};
static NimBLECharacteristic* mouseReport = nullptr;
static std::atomic<bool> reportSubscribed{false};
static bool mpuReady = false;
static bool streaming = false;
static Preferences prefs;

// Report ID 1: [pulsanti] [spostamento X] [spostamento Y].
// I primi due bit rappresentano sinistro e destro; il terzo resta sempre zero.
static uint8_t REPORT_MAP[] = {
    0x05, 0x01,       // Usage Page: Generic Desktop
    0x09, 0x02,       // Usage: Mouse
    0xA1, 0x01,       // Collection: Application
    0x85, 0x01,       // Report ID: 1
    0x09, 0x01,       // Usage: Pointer
    0xA1, 0x00,       // Collection: Physical
    0x05, 0x09,       // Usage Page: Button
    0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03, // Padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, // X, Y
    0x15, 0x81, 0x25, 0x7F,             // -127 ... +127
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, // Movimento relativo
    0xC0, 0xC0
};

class ReportCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t value) override {
    reportSubscribed.store((value & 1) != 0);
  }
};
static ReportCallbacks reportCallbacks;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    Serial.println("[BLE] computer connesso");
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    reportSubscribed.store(false);
    Serial.println("[BLE] disconnesso, riprendo l'annuncio");
    NimBLEDevice::startAdvertising();
  }
};
static ServerCallbacks serverCallbacks;

static bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static int16_t read16() {
  const uint8_t hi = Wire.read();
  const uint8_t lo = Wire.read();
  return static_cast<int16_t>((hi << 8) | lo);
}

// Legge i valori grezzi: raw[0..2] = accelerometro X Y Z, raw[3..5] = giroscopio X Y Z.
static bool readMpu(int16_t raw[6]) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); // ACCEL_XOUT_H: accelerometro (6 byte), temperatura (2), giroscopio (6)
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, static_cast<uint8_t>(14)) != 14) return false;
  for (int i = 0; i < 3; ++i) raw[i] = read16();
  read16(); // Temperatura, non usata.
  for (int i = 3; i < 6; ++i) raw[i] = read16();
  return true;
}

static void setupMpu() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  Wire.setTimeOut(10);
  // Sveglia il sensore e imposta esplicitamente scala e filtro del giroscopio.
  if (!writeReg(0x6B, 0x01) || !writeReg(0x1B, 0x10) ||
      !writeReg(0x1A, 0x03) || !writeReg(0x19, 0x04)) {
    Serial.println("[MPU] non disponibile: funzionano solo i pulsanti. Controlla i collegamenti e riavvia.");
    return;
  }
  delay(100);
  Serial.println("[MPU] calibrazione: tieni FERMO il sensore...");
  float sum[3] = {0, 0, 0};
  unsigned samples = 0;
  for (unsigned i = 0; i < 200; ++i) {
    int16_t raw[6];
    if (readMpu(raw)) {
      for (int a = 0; a < 3; ++a) sum[a] += raw[3 + a];
      ++samples;
    }
    delay(5);
  }
  if (samples < 180) {
    Serial.println("[MPU] calibrazione fallita: funzionano solo i pulsanti. Controlla il sensore e riavvia.");
    return;
  }
  for (int a = 0; a < 3; ++a) params[BIAS_X + a].value = sum[a] / samples / LSB_PER_DPS;
  mpuReady = true;
  Serial.printf("[MPU] pronto, offset X=%.2f Y=%.2f Z=%.2f gradi/s\n",
                params[BIAS_X].value, params[BIAS_Y].value, params[BIAS_Z].value);
}

static void loadParams() {
  prefs.begin("eous");
  for (int i = 0; i < PARAM_COUNT; ++i) {
    Param& p = params[i];
    p.value = i < SAVED_PARAMS ? prefs.getFloat(p.name, p.initial) : p.initial;
  }
}

// Comandi seriali, uno per riga: get | set <nome> <valore> | stream <0|1> | save | reset.
// La risposta termina sempre con una riga "OK" oppure "ERR ...".
static void handleCommand(const char* line) {
  char name[16];
  float value = 0;
  int on;
  if (strcmp(line, "get") == 0) {
    Serial.printf("I,lsb_per_dps,%.1f\nI,lsb_per_g,%.0f\nI,send_ms,%u\n",
                  LSB_PER_DPS, LSB_PER_G, static_cast<unsigned>(SEND_MS));
    for (const Param& p : params) Serial.printf("P,%s,%.5f\n", p.name, p.value);
  } else if (sscanf(line, "set %15s %f", name, &value) == 2) {
    Param* found = nullptr;
    for (Param& p : params) {
      if (strcmp(p.name, name) == 0) found = &p;
    }
    if (!found || !isfinite(value)) {
      Serial.println("ERR parametro o valore non valido");
      return;
    }
    found->value = value;
  } else if (sscanf(line, "stream %d", &on) == 1) {
    streaming = on != 0;
  } else if (strcmp(line, "save") == 0) {
    for (int i = 0; i < SAVED_PARAMS; ++i) prefs.putFloat(params[i].name, params[i].value);
  } else if (strcmp(line, "reset") == 0) {
    prefs.clear();
    for (int i = 0; i < SAVED_PARAMS; ++i) params[i].value = params[i].initial;
  } else {
    Serial.println("ERR comando sconosciuto");
    return;
  }
  Serial.println("OK");
}

static void readCommands() {
  static char line[48];
  static size_t length = 0;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c != '\n') {
      if (length < sizeof(line) - 1) line[length++] = c;
      continue;
    }
    line[length] = '\0';
    length = 0;
    if (line[0] != '\0') handleCommand(line);
  }
}

// Accelerazione del cursore: il guadagno resta 1 fino ad acc_lo, sale in linea
// retta fino ad acc_max tra acc_lo e acc_hi, poi resta costante.
static float accelGain(float speed) {
  const float lo = params[ACC_LO].value;
  const float hi = params[ACC_HI].value;
  if (hi <= lo) return speed < lo ? 1.0f : params[ACC_MAX].value;
  const float t = constrain((speed - lo) / (hi - lo), 0.0f, 1.0f);
  return 1.0f + (params[ACC_MAX].value - 1.0f) * t;
}

// Restituisce la parte intera da inviare e lascia in acc la frazione, cosi' un
// movimento lento non si perde per l'arrotondamento. Oltre il limite del report
// (+-127) l'eccesso viene scartato: altrimenti il cursore continuerebbe a
// muoversi dopo che la mano si e' fermata.
static int8_t takeWhole(float& acc) {
  const float capped = constrain(acc, -127.0f, 127.0f);
  const int8_t whole = static_cast<int8_t>(capped);
  acc = capped == acc ? acc - whole : 0.0f;
  return whole;
}

static void setupBluetooth() {
  NimBLEDevice::init("Eous Mouse");
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks);

  // La libreria crea i servizi HID, informazioni dispositivo e batteria.
  static NimBLEHIDDevice hid(server);
  hid.setManufacturer("Eous");
  // Identificativi non assegnati per questo prototipo; versione firmware 1.0.
  hid.setPnp(0x02, 0x0000, 0x0000, 0x0100);
  hid.setHidInfo(0x00, 0x02);
  hid.setReportMap(REPORT_MAP, sizeof(REPORT_MAP));
  // Segnaposto: questo progetto non misura ancora una batteria reale.
  hid.setBatteryLevel(100);
  mouseReport = hid.getInputReport(1);
  mouseReport->setCallbacks(&reportCallbacks);
  const uint8_t released[] = {0, 0, 0};
  mouseReport->setValue(released, sizeof(released));
  server->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName("Eous Mouse");
  advertising->setAppearance(HID_MOUSE);
  advertising->addServiceUUID(hid.getHidService()->getUUID());
  advertising->enableScanResponse(true);
  advertising->start();
  Serial.println("[BLE] associa Eous Mouse dalle impostazioni Bluetooth del computer");
}

void setup() {
  Serial.begin(115200);
  leftButton.begin();
  rightButton.begin();
  // Gli altri contatti restano ingressi e non generano eventi mouse.
  for (uint8_t pin : UNUSED_PINS) pinMode(pin, INPUT_PULLUP);
  delay(1000);
  Serial.println("\n=== Eous Mouse: GPIO15 sinistro, GPIO16 destro ===");
  loadParams();
  setupMpu();
  setupBluetooth();
}

void loop() {
  const uint32_t now = millis();
  leftButton.update(now);
  rightButton.update(now);
  readCommands();

  static uint32_t lastUs = micros();
  const uint32_t nowUs = micros();
  if (nowUs - lastUs < SEND_MS * 1000) {
    delay(1);
    return;
  }
  const float dt = (nowUs - lastUs) * 1e-6f;  // Tempo reale trascorso, in secondi.
  lastUs = nowUs;

  static float remainderX = 0, remainderY = 0;
  int16_t raw[6] = {};
  int8_t dx = 0, dy = 0;
  const bool sampled = mpuReady && readMpu(raw);
  if (sampled) {
    float g[3];  // Velocita' angolare in gradi/s, senza offset.
    for (int a = 0; a < 3; ++a) g[a] = raw[3 + a] / LSB_PER_DPS - params[BIAS_X + a].value;
    const float vx = params[X_GX].value * g[0] + params[X_GY].value * g[1] + params[X_GZ].value * g[2];
    const float vy = params[Y_GX].value * g[0] + params[Y_GY].value * g[1] + params[Y_GZ].value * g[2];
    // La zona morta guarda la velocita' complessiva e non i singoli assi: cosi'
    // un movimento obliquo lento non perde una delle due componenti.
    const float speed = sqrtf(vx * vx + vy * vy);
    if (speed >= params[DEADZONE].value) {
      // gradi/s * secondi = gradi; gradi * (pixel per grado) = pixel.
      const float pixelsPerDps = dt * params[SENS].value * accelGain(speed);
      remainderX += vx * pixelsPerDps;
      remainderY += vy * pixelsPerDps;
    }
    dx = takeWhole(remainderX);
    dy = takeWhole(remainderY);
  } else {
    remainderX = remainderY = 0;
  }

  // Dati grezzi per il PC: tempo (microsecondi), accelerometro, giroscopio, movimento inviato.
  if (streaming && sampled) {
    Serial.printf("D,%lu,%d,%d,%d,%d,%d,%d,%d,%d\n", static_cast<unsigned long>(nowUs),
                  raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], dx, dy);
  }

  if (!reportSubscribed.load()) return;

  const uint8_t buttons = (leftButton.pressed ? 0x01 : 0) |
                          (rightButton.pressed ? 0x02 : 0);
  const uint8_t report[] = {buttons, static_cast<uint8_t>(dx), static_cast<uint8_t>(dy)};
  // Invia anche da fermo: pressione, trascinamento e rilascio non dipendono dal sensore.
  // Ripetere lo stato permette inoltre di sincronizzarsi dopo una riconnessione.
  mouseReport->setValue(report, sizeof(report));
  mouseReport->notify();
}
