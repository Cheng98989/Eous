#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>

// Mouse Bluetooth (BLE HID) su ESP32-S3.
//  USE_MPU = false -> il cursore disegna un quadrato (test, senza sensore)
//  USE_MPU = true  -> il cursore si muove con il giroscopio MPU6050
constexpr bool USE_MPU = true;

// --- MPU6050 (I2C: SDA = GPIO8, SCL = GPIO9, AD0 a GND) ---
constexpr int SDA_PIN = 8;
constexpr int SCL_PIN = 9;
constexpr uint8_t MPU_ADDR = 0x68;
constexpr float SENS = 0.2f;      // pixel per (grado/s) a ogni invio: alza per un cursore piu' veloce
constexpr float DEADZONE = 1.0f;  // gradi/s ignorati (rumore a riposo)
constexpr uint32_t SEND_MS = 20;  // 50 invii al secondo

// --- Descrittore HID: dichiara al sistema operativo che tipo di dispositivo siamo ---
// Ogni report e' di 3 byte: [bottoni (3 bit + 5 di riempimento)] [dx] [dy]
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01,        // Usage Page: Generic Desktop
    0x09, 0x02,        // Usage: Mouse
    0xA1, 0x01,        // Collection: Application
    0x09, 0x01,        //   Usage: Pointer
    0xA1, 0x00,        //   Collection: Physical
    0x05, 0x09,        //     Usage Page: Button
    0x19, 0x01,        //     Usage Minimum: bottone 1
    0x29, 0x03,        //     Usage Maximum: bottone 3
    0x15, 0x00,        //     Logical Minimum: 0
    0x25, 0x01,        //     Logical Maximum: 1
    0x95, 0x03,        //     Report Count: 3
    0x75, 0x01,        //     Report Size: 1 bit
    0x81, 0x02,        //     Input (dato, variabile, assoluto)
    0x95, 0x01,        //     Report Count: 1
    0x75, 0x05,        //     Report Size: 5 bit
    0x81, 0x03,        //     Input (costante: riempimento)
    0x05, 0x01,        //     Usage Page: Generic Desktop
    0x09, 0x30,        //     Usage: X
    0x09, 0x31,        //     Usage: Y
    0x15, 0x81,        //     Logical Minimum: -127
    0x25, 0x7F,        //     Logical Maximum: 127
    0x75, 0x08,        //     Report Size: 8 bit
    0x95, 0x02,        //     Report Count: 2
    0x81, 0x06,        //     Input (dato, variabile, RELATIVO)
    0xC0,              //   End Collection
    0xC0               // End Collection
};

static NimBLEServer *server = nullptr;
static NimBLECharacteristic *reportChar = nullptr;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    Serial.println("[BLE] connesso");
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
    Serial.println("[BLE] disconnesso, riprendo l'annuncio");
    NimBLEDevice::startAdvertising();
  }
};
static ServerCallbacks serverCallbacks;

static void sendMouse(int8_t dx, int8_t dy, uint8_t buttons = 0) {
  if (server->getConnectedCount() == 0) return;
  uint8_t report[3] = {buttons, (uint8_t)dx, (uint8_t)dy};
  reportChar->setValue(report, sizeof(report));
  reportChar->notify();
}

// --- MPU6050 ---
static int16_t read16() {
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  return (int16_t)((hi << 8) | lo);
}

static bool readGyro(float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x43);  // GYRO_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)6) != 6) return false;
  gx = read16() / 131.0f;  // +-250 gradi/s -> 131 LSB per grado/s
  gy = read16() / 131.0f;
  gz = read16() / 131.0f;
  return true;
}

static float biasX = 0, biasY = 0, biasZ = 0;

static void setupMpu() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);  // PWR_MGMT_1: scrivere 0 sveglia il sensore
  Wire.write(0x00);
  Wire.endTransmission();
  delay(100);

  Serial.println("[MPU] calibrazione: tieni il modulo FERMO...");
  float sx = 0, sy = 0, sz = 0;
  int n = 0;
  for (int i = 0; i < 200; i++) {
    float x, y, z;
    if (readGyro(x, y, z)) { sx += x; sy += y; sz += z; n++; }
    delay(5);
  }
  if (n > 0) { biasX = sx / n; biasY = sy / n; biasZ = sz / n; }
  Serial.printf("[MPU] offset giroscopio: %.2f %.2f %.2f gradi/s\n", biasX, biasY, biasZ);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Mouse BLE ===");

  if (USE_MPU) setupMpu();

  NimBLEDevice::init("Eous Mouse");
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, senza PIN, connessione sicura

  server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks);

  // Servizio HID (0x1812)
  NimBLEService *hid = server->createService((uint16_t)0x1812);

  NimBLECharacteristic *info = hid->createCharacteristic((uint16_t)0x2A4A, NIMBLE_PROPERTY::READ);
  const uint8_t infoVal[] = {0x11, 0x01, 0x00, 0x02};  // HID 1.11, nessun paese, connettibile
  info->setValue(infoVal, sizeof(infoVal));

  NimBLECharacteristic *map = hid->createCharacteristic(
      (uint16_t)0x2A4B, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  map->setValue(REPORT_MAP, sizeof(REPORT_MAP));

  hid->createCharacteristic((uint16_t)0x2A4C, NIMBLE_PROPERTY::WRITE_NR);  // control point

  NimBLECharacteristic *proto = hid->createCharacteristic(
      (uint16_t)0x2A4E, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR);
  const uint8_t protoVal[] = {0x01};  // modalita' "report"
  proto->setValue(protoVal, sizeof(protoVal));

  reportChar = hid->createCharacteristic(
      (uint16_t)0x2A4D,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  NimBLEDescriptor *ref = reportChar->createDescriptor(
      (uint16_t)0x2908, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  const uint8_t refVal[] = {0x00, 0x01};  // report ID 0, tipo "input"
  ref->setValue(refVal, sizeof(refVal));
  const uint8_t zero[3] = {0, 0, 0};
  reportChar->setValue(zero, sizeof(zero));

  hid->start();

  // Servizio batteria (0x180F): alcuni sistemi lo vogliono insieme all'HID
  NimBLEService *bat = server->createService((uint16_t)0x180F);
  NimBLECharacteristic *level = bat->createCharacteristic(
      (uint16_t)0x2A19, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  const uint8_t levelVal[] = {100};
  level->setValue(levelVal, sizeof(levelVal));
  bat->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName("Eous Mouse");
  adv->setAppearance(0x03C2);  // "mouse": il sistema mostra l'icona giusta
  adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  adv->enableScanResponse(true);
  adv->start();

  Serial.println("[BLE] in annuncio come \"Eous Mouse\": accoppialo dalle impostazioni Bluetooth");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < SEND_MS) return;
  last = millis();

  if (!USE_MPU) {
    // Test: quadrato di 50 passi per lato, 5 pixel a passo
    static int step = 0;
    int phase = (step / 50) % 4;
    int8_t dx = 0, dy = 0;
    if (phase == 0) dx = 5;
    else if (phase == 1) dy = 5;
    else if (phase == 2) dx = -5;
    else dy = -5;
    step++;
    sendMouse(dx, dy);
    return;
  }

  // Giroscopio -> movimento: rotazione intorno a Z -> X, rotazione intorno a X -> Y
  float gx, gy, gz;
  if (!readGyro(gx, gy, gz)) return;
  float vx = -(gz - biasZ);  // se il verso e' sbagliato, togli o metti il segno meno
  float vy = -(gx - biasX);
  if (fabsf(vx) < DEADZONE) vx = 0;
  if (fabsf(vy) < DEADZONE) vy = 0;

  // Accumulo la parte frazionaria: un movimento lento non si perde per l'arrotondamento
  static float accX = 0, accY = 0;
  accX += vx * SENS;
  accY += vy * SENS;
  int8_t sx = (int8_t)constrain((int)accX, -127, 127);
  int8_t sy = (int8_t)constrain((int)accY, -127, 127);
  accX -= sx;
  accY -= sy;

  if (sx != 0 || sy != 0) sendMouse(sx, sy);
}