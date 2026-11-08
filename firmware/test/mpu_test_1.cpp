#include <Arduino.h>
#include <Wire.h>

// Test MPU6050 su ESP32-S3: SDA = GPIO8, SCL = GPIO9 (AD0 a GND -> indirizzo 0x68)

constexpr int SDA_PIN = 8;
constexpr int SCL_PIN = 9;
constexpr uint8_t MPU_ADDR = 0x68;

// Registri del sensore (dal datasheet)
constexpr uint8_t REG_WHO_AM_I = 0x75;
constexpr uint8_t REG_PWR_MGMT_1 = 0x6B;
constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;  // da qui: 14 byte di dati

static void scanBus() {
  Serial.println("[I2C] scansione...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {  // 0 = qualcuno ha risposto (ACK)
      Serial.printf("[I2C] trovato dispositivo a 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) Serial.println("[I2C] nessun dispositivo: controlla fili, SDA/SCL, alimentazione");
}

static void writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);  // false = non rilasciare il bus (restart)
  Wire.requestFrom(MPU_ADDR, (uint8_t)1);
  return Wire.read();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== Test MPU6050 ===");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);  // 400 kHz (fast mode)

  scanBus();

  uint8_t who = readReg(REG_WHO_AM_I);
  Serial.printf("[MPU] WHO_AM_I = 0x%02X (atteso 0x68; alcuni cloni danno 0x70/0x72)\n", who);

  // All'accensione il sensore e' in sleep: scrivere 0 in PWR_MGMT_1 lo sveglia
  writeReg(REG_PWR_MGMT_1, 0x00);
  delay(100);
}

void loop() {
  // Leggo 14 byte in un colpo solo: accel (6), temperatura (2), giroscopio (6)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_ACCEL_XOUT_H);
  Wire.endTransmission(false);
  size_t n = Wire.requestFrom(MPU_ADDR, (uint8_t)14);
  if (n != 14) {
    Serial.println("[MPU] lettura fallita (contatto?)");
    delay(500);
    return;
  }

  // Ogni valore e' un int16 diviso in due byte, prima quello alto (big-endian)
  auto read16 = []() -> int16_t {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    return (int16_t)((hi << 8) | lo);
  };

  int16_t ax = read16(), ay = read16(), az = read16();
  int16_t tempRaw = read16();
  int16_t gx = read16(), gy = read16(), gz = read16();

  // Scale predefinite: accel +-2 g -> 16384 LSB/g; giro +-250 gradi/s -> 131 LSB/(gradi/s)
  Serial.printf("acc[g] %6.2f %6.2f %6.2f | giro[gradi/s] %7.1f %7.1f %7.1f | T %.1f C\n",
                ax / 16384.0f, ay / 16384.0f, az / 16384.0f,
                gx / 131.0f, gy / 131.0f, gz / 131.0f,
                tempRaw / 340.0f + 36.53f);

  delay(200);
}