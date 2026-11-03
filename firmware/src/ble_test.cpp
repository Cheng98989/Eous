// importo le librerie necessarie
#include <Arduino.h> // per la gestione della scheda e del serial monitor
#include <NimBLEDevice.h> // per la gestione del BLE (Bluetooth Low Energy)

// ** Test BLE: la scheda si annuncia come "Eous" ed espone una caratteristica
// ** che invia un contatore ogni secondo (notifica).

/*uso #define: molto in pratica definisco una macro, quando la definisco, alla compilazione
viene sostituita con il valore specificato.*/
// UUID del servizio e della caratteristica
#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e" // indentifica servizio BLE
#define CHAR_UUID    "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // indentifica caratteristica BLE

/*In C++ a differenza di C sharp gli la gestione degli indirizzi è più complessa/complessa
l'asterisco '*' serve per creare un puntatore a una variabile. Ovverro una variabile che contiene
un indirizzo di memoria. "nullptr" è un puntatore nullo, ovvero un puntatore che non punta a nessun oggetto,
ovvero "null" in c sharp.*/
static NimBLECharacteristic *counterChar = nullptr;
//Unsigned int equivale a un intero senza segno, ovvero un intero che può contenere solo valori positivi. "Uint" in C Sharp
static uint32_t counter = 0;

/*Dichiato classe ServerCallbacks che eredita da NimBLEServerCallbacks, ovvero una classe che gestisce gli eventi del server BLE.
sovrascrivo i metodi onConnect e onDisconnect per gestire gli eventi di connessione e disconnessione dei client.*/
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override { //NimBLEServer è l'indirizzo del server BLE (ESP32 stesso)
    Serial.println("[BLE] client connesso");
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override { 
    Serial.println("[BLE] client disconnesso, riprendo l'annuncio");
    NimBLEDevice::startAdvertising();// riprendo l'annuncio del servizio BLE quando un client si disconnette
  }
};

/*Dichiaro una variabile statica serverCallbacks di tipo ServerCallbacks, ovvero una variabile che gestisce gli eventi del server BLE.
A differenza di C Sharp, senza nessun corrispondente a "new" la mia classe e' istanziata automaticamente.*/
static ServerCallbacks serverCallbacks;

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Test BLE ===");

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
  char buf[16];
  snprintf(buf, sizeof(buf), "%u", (unsigned)counter++);
  counterChar->setValue(buf);
  counterChar->notify();
  delay(1000);
}