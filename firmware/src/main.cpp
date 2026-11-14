#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <atomic>
#include <math.h>

// Eous: mouse BLE HID. Il computer usa il proprio supporto mouse Bluetooth.
// MPU6050: SDA -> GPIO8, SCL -> GPIO9, AD0 -> GND, massa comune.
// Pulsanti: chiudere il contatto tra il relativo GPIO e GND.
constexpr uint8_t LEFT_PIN = 15;
constexpr uint8_t RIGHT_PIN = 16;
constexpr uint8_t UNUSED_PINS[] = {17, 18};
constexpr int SDA_PIN = 8;
constexpr int SCL_PIN = 9;
constexpr uint8_t MPU_ADDR = 0x68;
constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint32_t SEND_MS = 20;
constexpr float SENSITIVITY = 0.2f; // Aumentare per un cursore piu' veloce.
constexpr float DEADZONE = 1.0f;    // gradi/s ignorati a riposo.

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
static float biasX = 0, biasZ = 0;

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

static bool readGyro(float& gx, float& gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x43); // GYRO_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, static_cast<uint8_t>(6)) != 6) return false;
  gx = read16() / 131.0f;
  read16(); // L'asse Y non serve al movimento del cursore.
  gz = read16() / 131.0f;
  return true;
}

static void setupMpu() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  Wire.setTimeOut(10);
  // Sveglia il sensore e imposta esplicitamente scala e filtro del giroscopio.
  if (!writeReg(0x6B, 0x01) || !writeReg(0x1B, 0x00) ||
      !writeReg(0x1A, 0x03) || !writeReg(0x19, 0x04)) {
    Serial.println("[MPU] non disponibile: funzionano solo i pulsanti. Controlla i collegamenti e riavvia.");
    return;
  }
  delay(100);
  Serial.println("[MPU] calibrazione: tieni FERMO il sensore...");
  float sumX = 0, sumZ = 0;
  unsigned samples = 0;
  for (unsigned i = 0; i < 200; ++i) {
    float x, z;
    if (readGyro(x, z)) {
      sumX += x;
      sumZ += z;
      ++samples;
    }
    delay(5);
  }
  if (samples < 180) {
    Serial.println("[MPU] calibrazione fallita: funzionano solo i pulsanti. Controlla il sensore e riavvia.");
    return;
  }
  biasX = sumX / samples;
  biasZ = sumZ / samples;
  mpuReady = true;
  Serial.printf("[MPU] pronto, offset X=%.2f Z=%.2f gradi/s\n", biasX, biasZ);
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
  setupMpu();
  setupBluetooth();
}

void loop() {
  const uint32_t now = millis();
  leftButton.update(now);
  rightButton.update(now);

  static uint32_t lastSend = 0;
  if (now - lastSend < SEND_MS) {
    delay(1);
    return;
  }
  lastSend = now;

  static float remainderX = 0, remainderY = 0;
  if (!reportSubscribed.load()) {
    remainderX = remainderY = 0;
    return;
  }

  int8_t dx = 0, dy = 0;
  if (mpuReady) {
    float gx, gz;
    if (readGyro(gx, gz)) {
      float vx = -(gz - biasZ);
      float vy = -(gx - biasX);
      if (fabsf(vx) < DEADZONE) vx = 0;
      if (fabsf(vy) < DEADZONE) vy = 0;
      remainderX += vx * SENSITIVITY;
      remainderY += vy * SENSITIVITY;
      dx = static_cast<int8_t>(constrain(static_cast<int>(remainderX), -127, 127));
      dy = static_cast<int8_t>(constrain(static_cast<int>(remainderY), -127, 127));
      remainderX -= dx;
      remainderY -= dy;
    } else {
      remainderX = remainderY = 0;
    }
  }

  const uint8_t buttons = (leftButton.pressed ? 0x01 : 0) |
                          (rightButton.pressed ? 0x02 : 0);
  const uint8_t report[] = {buttons, static_cast<uint8_t>(dx), static_cast<uint8_t>(dy)};
  // Invia anche da fermo: pressione, trascinamento e rilascio non dipendono dal sensore.
  // Ripetere lo stato permette inoltre di sincronizzarsi dopo una riconnessione.
  mouseReport->setValue(report, sizeof(report));
  mouseReport->notify();
}
