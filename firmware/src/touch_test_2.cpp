#include <Arduino.h>

// MODE 1: ogni pin e' un ingresso INPUT_PULLUP indipendente (contatto verso GND)
// MODE 2: scansione a coppie: un pin alla volta diventa uscita LOW e gli altri
//         lo leggono -> rileva quali pin sono collegati TRA LORO (senza GND)
#define MODE 2

const uint8_t PINS[] = {15, 16, 17, 18};
const uint8_t N = sizeof(PINS);
const uint32_t DEBOUNCE_MS = 20;

// ---------- MODE 1 ----------
bool stable[8];            // stato stabile (true = premuto/contatto con GND)
bool lastRaw[8];
uint32_t lastChange[8];

void setupMode1() {
  for (uint8_t i = 0; i < N; i++) {
    pinMode(PINS[i], INPUT_PULLUP);
    stable[i] = lastRaw[i] = false;
    lastChange[i] = 0;
  }
}

void loopMode1() {
  bool changed = false;
  for (uint8_t i = 0; i < N; i++) {
    bool raw = (digitalRead(PINS[i]) == LOW);        // LOW = contatto con GND
    if (raw != lastRaw[i]) { lastRaw[i] = raw; lastChange[i] = millis(); }
    // accetta il nuovo stato solo se stabile da DEBOUNCE_MS
    if (raw != stable[i] && millis() - lastChange[i] >= DEBOUNCE_MS) {
      stable[i] = raw;
      changed = true;
    }
  }
  if (changed) {
    uint8_t mask = 0, count = 0;
    Serial.print("Pin attivi: ");
    for (uint8_t i = 0; i < N; i++) {
      if (stable[i]) { mask |= (1 << i); count++; Serial.printf("%u ", PINS[i]); }
    }
    if (!count) Serial.print("nessuno");
    Serial.printf("| maschera=0b%04b | totale=%u\n", mask, count);
  }
}

// ---------- MODE 2 ----------
bool prevLink[8][8];

void setupMode2() {
  for (uint8_t i = 0; i < N; i++) pinMode(PINS[i], INPUT_PULLUP);
}

void loopMode2() {
  bool link[8][8] = {};
  for (uint8_t i = 0; i < N; i++) {
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], LOW);
    delayMicroseconds(50);
    for (uint8_t j = 0; j < N; j++) {
      if (j != i && digitalRead(PINS[j]) == LOW) link[i][j] = true;
    }
    pinMode(PINS[i], INPUT_PULLUP);   // torna ingresso prima del pin successivo
  }
  if (memcmp(link, prevLink, sizeof(link)) != 0) {
    memcpy(prevLink, link, sizeof(link));
    Serial.print("Collegamenti: ");
    bool any = false;
    for (uint8_t i = 0; i < N; i++)
      for (uint8_t j = i + 1; j < N; j++)
        if (link[i][j]) { Serial.printf("%u<->%u  ", PINS[i], PINS[j]); any = true; }
    if (!any) Serial.print("nessuno");
    Serial.println();
  }
  delay(10);
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
#if MODE == 1
  Serial.println("MODE 1: contatti verso GND");
  setupMode1();
#else
  Serial.println("MODE 2: scansione collegamenti tra pin");
  setupMode2();
#endif
}

void loop() {
#if MODE == 1
  loopMode1();
#else
  loopMode2();
#endif
}