#include <Arduino.h>
// Test touch ESP32-S3 (Arduino-ESP32 core 3.x)
// Metodo 1: touch capacitivo nativo (touchRead)  -> GPIO 4 (T4)
// Metodo 2: contatto verso GND con pull-up interna -> GPIO 15

const uint8_t TOUCH_PIN = 4;     // pin touch nativo (T4)
const uint8_t GND_PIN   = 15;    // pin digitale normale

const float   TOUCH_RATIO = 1.10;  // soglia: +10% rispetto alla baseline (da tarare)
const uint8_t SAMPLES     = 8;     // media mobile per ridurre il rumore

uint32_t baseline = 0;

uint32_t readTouchAvg() {
  uint64_t sum = 0;
  for (uint8_t i = 0; i < SAMPLES; i++) {
    sum += touchRead(TOUCH_PIN);
    delay(2);
  }
  return sum / SAMPLES;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(GND_PIN, INPUT_PULLUP);   // riposo = HIGH, contatto con GND = LOW

  // Calibrazione: NON toccare il pad in questi 2 secondi
  Serial.println("Calibrazione touch, non toccare...");
  uint64_t sum = 0;
  for (int i = 0; i < 50; i++) {
    sum += readTouchAvg();
    delay(40);
  }
  baseline = sum / 50;
  Serial.printf("Baseline: %lu\n", (unsigned long)baseline);
}

void loop() {
  // --- Metodo 1: touch capacitivo ---
  uint32_t raw = readTouchAvg();
  // Su ESP32-S3 il valore SALE quando tocchi (sull'ESP32 classico scende)
  bool touched = raw > baseline * TOUCH_RATIO;

  // --- Metodo 2: circuito chiuso verso GND ---
  bool contact = (digitalRead(GND_PIN) == LOW);

  Serial.printf("touch raw=%lu  [%s]   |   GND contact: [%s]\n",
                (unsigned long)raw,
                touched ? "TOCCATO" : "  -    ",
                contact ? "CHIUSO" : "aperto");

  delay(100);
}
