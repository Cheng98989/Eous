#include <Arduino.h>
#include <NimBLEDevice.h>
#include "firefox_launcher.h"

// Test BLE: la scheda si annuncia come "Eous" ed espone una caratteristica
// che invia un contatore ogni secondo (notifica).
// Con un client BLE connesso, premendo BOOT si apre Firefox sul PC (via USB).

#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_UUID    "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define BUTTON_PIN   0          // pulsante BOOT della scheda

static NimBLECharacteristic *counterChar = nullptr;
static uint32_t counter = 0;
static uint32_t lastNotify = 0;
static volatile bool bleConnected = false;   // true solo con un client connesso

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    bleConnected = true;
    Serial.println("[BLE] client connesso");
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    bleConnected = false;
    Serial.println("[BLE] client disconnesso, riprendo l'annuncio");
    NimBLEDevice::startAdvertising();
  }
};

static ServerCallbacks serverCallbacks;

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Test BLE ===");

  launcherInit();
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  NimBLEDevice::init("Eous");

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks);

  NimBLEService *service = server->createService(SERVICE_UUID);
  counterChar = service->createCharacteristic(
      CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  counterChar->setValue("0");
  service->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName("Eous");
  adv->addServiceUUID(service->getUUID());
  adv->enableScanResponse(true);
  adv->start();

  Serial.println("[BLE] in annuncio come \"Eous\"");
}

void loop() {
  // Pulsante BOOT premuto + BLE connesso -> apre Firefox sul PC
  if (bleConnected && digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("[BTN] apro Firefox");
    launchFirefox();
    delay(2000);   // evita ripetizioni se tieni premuto
  }

  // Contatore BLE ogni secondo
  if (millis() - lastNotify >= 1000) {
    lastNotify = millis();
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)counter++);
    counterChar->setValue(buf);
    counterChar->notify();
  }
}