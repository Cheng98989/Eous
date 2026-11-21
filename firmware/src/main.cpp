#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#include <atomic>
#include <math.h>

// Eous: mouse BLE HID. Il computer usa il proprio supporto mouse Bluetooth.
// MPU6050: SDA -> GPIO8, SCL -> GPIO9, AD0 -> GND, massa comune.
// Pulsanti e frizione: chiudere il contatto tra il relativo GPIO e GND.
// Calibrazione e regolazioni dal PC, via seriale: tools/eous_tune.py e tools/eous_gui.py.
constexpr uint8_t LEFT_PIN = 15;
constexpr uint8_t RIGHT_PIN = 16;
constexpr uint8_t CLUTCH_PIN = 17;
constexpr uint8_t UNUSED_PINS[] = {18};
constexpr int SDA_PIN = 8;
constexpr int SCL_PIN = 9;
constexpr uint8_t MPU_ADDR = 0x68;
constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint32_t SAMPLE_MS = 5;           // Il sensore viene letto 200 volte al secondo...
constexpr uint32_t SAMPLES_PER_REPORT = 2;  // ...e ogni due letture parte un report mouse.
constexpr uint32_t SEND_MS = SAMPLE_MS * SAMPLES_PER_REPORT;
constexpr float LSB_PER_DPS = 32.8f;   // Scala +-1000 gradi/s: a +-250 un movimento rapido del polso satura.
constexpr float LSB_PER_G = 16384.0f;  // Scala +-2 g dell'accelerometro.

// Riconoscimento del sensore fermo, usato per misurare l'offset del giroscopio.
constexpr uint16_t STILL_SAMPLES = 200;      // Una finestra dura un secondo.
constexpr float STILL_SPREAD_DPS = 2.0f;     // Da fermo massimo e minimo distano meno di cosi' (solo rumore).
constexpr float STILL_AGREE_DPS = 0.3f;      // Due finestre ferme di fila devono dare la stessa media.
constexpr float MAX_BIAS_DPS = 20.0f;        // Offset massimo dichiarato dal costruttore del sensore.
constexpr float BIAS_STORE_DPS = 0.2f;       // Sotto questa variazione non si riscrive la memoria flash.

// Parametri regolabili via seriale senza ricompilare. Il comando "save" scrive
// nella memoria flash quelli fino a SAVED_PARAMS; l'offset del giroscopio cambia
// con la temperatura, quindi il firmware lo rimisura da solo e ne salva una copia
// da usare quando all'avvio il sensore non e' fermo.
enum ParamId {
  SENS, DEADZONE, ACC_LO, ACC_HI, ACC_MAX,
  FILT_HZ, FILT_BETA, CLUTCH, CLICK_MS, AUTOBIAS,
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
    {"filt_hz", 3.0f},   // Filtro: frequenza di taglio a mano ferma, in Hz; 0 = filtro spento.
    {"filt_beta", 0.1f}, // Filtro: di quanti Hz sale il taglio per ogni grado/s di velocita'.
    {"clutch", 2.0f},    // Frizione su CLUTCH_PIN: 0 = non usata, 1 = premuta per muovere, 2 = premuta per bloccare.
    {"click_ms", 120.0f},// Il cursore resta fermo per questo tempo dopo ogni pressione o rilascio di un pulsante.
    {"autobias", 1.0f},  // 1 = l'offset si corregge da solo quando il sensore resta fermo.
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
static Button clutchButton{CLUTCH_PIN};
static NimBLECharacteristic* mouseReport = nullptr;
static std::atomic<bool> reportSubscribed{false};
static bool mpuReady = false;
static bool streaming = false;
static Preferences prefs;
static float storedBias[3] = {NAN, NAN, NAN};  // Copia dell'offset nella memoria flash; NAN = mai salvata.
static float remainderX = 0, remainderY = 0;   // Frazioni di pixel non ancora inviate.

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
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    Serial.printf("[BLE] computer connesso, intervallo %.2f ms\n", info.getConnInterval() * 1.25f);
    // Chiede al computer di scambiare dati ogni 7,5-15 ms (unita' di 1,25 ms), cosi'
    // un report ogni 10 ms non resta in attesa. Il computer puo' rifiutare.
    server->updateConnParams(info.getConnHandle(), 6, 12, 0, 200);
  }

  void onConnParamsUpdate(NimBLEConnInfo& info) override {
    Serial.printf("[BLE] nuovo intervallo di connessione: %.2f ms\n", info.getConnInterval() * 1.25f);
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

// Raccoglie campioni del giroscopio per capire se il sensore e' fermo: da fermo
// la distanza tra il valore piu' alto e il piu' basso di ogni asse e' solo rumore.
struct StillWindow {
  int32_t sum[3];
  int16_t low[3], high[3];
  uint16_t count;

  StillWindow() { clear(); }

  void clear() {
    count = 0;
    for (int a = 0; a < 3; ++a) {
      sum[a] = 0;
      low[a] = INT16_MAX;
      high[a] = INT16_MIN;
    }
  }

  void add(const int16_t gyro[3]) {
    for (int a = 0; a < 3; ++a) {
      sum[a] += gyro[a];
      if (gyro[a] < low[a]) low[a] = gyro[a];
      if (gyro[a] > high[a]) high[a] = gyro[a];
    }
    ++count;
  }

  float mean(int a) const { return sum[a] / (count * LSB_PER_DPS); }

  bool still() const {
    for (int a = 0; a < 3; ++a) {
      if (high[a] - low[a] > STILL_SPREAD_DPS * LSB_PER_DPS) return false;
      if (fabsf(mean(a)) > MAX_BIAS_DPS) return false;  // Rotazione costante, non offset.
    }
    return count > 0;
  }
};

// Adotta un nuovo offset, avvisa il PC e aggiorna la copia in memoria flash.
static void applyBias(const float bias[3]) {
  bool changed = false, store = false;
  for (int a = 0; a < 3; ++a) {
    if (fabsf(bias[a] - params[BIAS_X + a].value) > 0.02f) changed = true;
    // Il confronto con NAN e' sempre falso: la prima volta si salva comunque.
    if (!(fabsf(bias[a] - storedBias[a]) <= BIAS_STORE_DPS)) store = true;
  }
  if (!changed && !store) return;
  for (int a = 0; a < 3; ++a) params[BIAS_X + a].value = bias[a];
  if (streaming) Serial.printf("B,%.3f,%.3f,%.3f\n", bias[0], bias[1], bias[2]);
  if (!store) return;
  for (int a = 0; a < 3; ++a) {
    prefs.putFloat(params[BIAS_X + a].name, bias[a]);
    storedBias[a] = bias[a];
  }
}

// Offset automatico: se per due secondi consecutivi il sensore e' fermo e le due
// medie coincidono, quella media e' il nuovo offset. Con il dispositivo indossato
// succede quando la mano e' appoggiata, non mentre e' sospesa in aria.
static void trackBias(const int16_t gyro[3]) {
  static StillWindow window;
  static float previous[3] = {0, 0, 0};
  static bool previousStill = false;
  window.add(gyro);
  if (window.count < STILL_SAMPLES) return;

  const bool still = window.still();
  bool agree = still && previousStill;
  float bias[3];
  for (int a = 0; a < 3; ++a) {
    const float mean = window.mean(a);
    if (fabsf(mean - previous[a]) > STILL_AGREE_DPS) agree = false;
    bias[a] = (mean + previous[a]) / 2;
    previous[a] = mean;
  }
  previousStill = still;
  window.clear();
  if (agree) applyBias(bias);
}

static void setupMpu() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  Wire.setTimeOut(10);
  // Sveglia il sensore; scala +-1000 gradi/s; filtro interno a 44 Hz; 1000 misure al secondo.
  if (!writeReg(0x6B, 0x01) || !writeReg(0x1B, 0x10) ||
      !writeReg(0x1A, 0x03) || !writeReg(0x19, 0x00)) {
    Serial.println("[MPU] non disponibile: funzionano solo i pulsanti. Controlla i collegamenti e riavvia.");
    return;
  }
  delay(100);

  StillWindow window;
  for (int attempt = 0; attempt < 3 && !window.still(); ++attempt) {
    Serial.println("[MPU] calibrazione: tieni FERMO il sensore...");
    window.clear();
    for (unsigned i = 0; i < STILL_SAMPLES; ++i) {
      int16_t raw[6];
      if (readMpu(raw)) window.add(raw + 3);
      delay(SAMPLE_MS);
    }
    if (window.count < STILL_SAMPLES * 9 / 10) {
      Serial.println("[MPU] calibrazione fallita: funzionano solo i pulsanti. Controlla il sensore e riavvia.");
      return;
    }
  }

  const float measured[3] = {window.mean(0), window.mean(1), window.mean(2)};
  if (window.still()) {
    applyBias(measured);
    Serial.print("[MPU] pronto, offset misurato");
  } else if (!isnan(storedBias[0])) {
    // I parametri contengono gia' la copia letta dalla memoria flash.
    Serial.print("[MPU] il sensore si muove: uso l'ultimo offset salvato");
  } else {
    for (int a = 0; a < 3; ++a) params[BIAS_X + a].value = measured[a];
    Serial.print("[MPU] il sensore si muove: offset provvisorio");
  }
  Serial.printf(" X=%.2f Y=%.2f Z=%.2f gradi/s\n",
                params[BIAS_X].value, params[BIAS_Y].value, params[BIAS_Z].value);
  mpuReady = true;
}

static void loadParams() {
  prefs.begin("eous");
  for (int i = 0; i < PARAM_COUNT; ++i) {
    Param& p = params[i];
    p.value = prefs.isKey(p.name) ? prefs.getFloat(p.name, p.initial) : p.initial;
  }
  if (prefs.isKey(params[BIAS_X].name)) {
    for (int a = 0; a < 3; ++a) storedBias[a] = params[BIAS_X + a].value;
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
    for (float& stored : storedBias) stored = NAN;
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

// Filtro "One Euro": un passa-basso la cui frequenza di taglio sale con la
// velocita' della mano. Mano lenta = molto smorzamento (via il tremolio), mano
// veloce = quasi nessun ritardo. Lavora sulla posizione e non sulla velocita':
// "lag" sono i pixel che il cursore deve ancora recuperare, e a ogni passo ne
// esce una frazione. Cosi' il movimento arriva in ritardo ma arriva tutto.
static float lagX = 0, lagY = 0;

static void smooth(float& stepX, float& stepY, float speed, float dt) {
  lagX += stepX;
  lagY += stepY;
  float alpha = 1.0f;  // Filtro spento: esce tutto subito.
  if (params[FILT_HZ].value > 0) {
    const float cutoff = params[FILT_HZ].value + fabsf(params[FILT_BETA].value) * speed;
    // Passa-basso del primo ordine: alpha = dt / (dt + costante di tempo).
    alpha = dt / (dt + 1.0f / (2.0f * PI * cutoff));
  }
  stepX = alpha * lagX;
  stepY = alpha * lagY;
  lagX -= stepX;
  lagY -= stepY;
}

// Elabora una lettura del sensore e accumula i pixel da inviare.
static void processSample(const int16_t raw[6], float dt, bool frozen) {
  if (params[AUTOBIAS].value != 0) trackBias(raw + 3);

  float g[3];  // Velocita' angolare in gradi/s, senza offset.
  for (int a = 0; a < 3; ++a) g[a] = raw[3 + a] / LSB_PER_DPS - params[BIAS_X + a].value;
  const float vx = params[X_GX].value * g[0] + params[X_GY].value * g[1] + params[X_GZ].value * g[2];
  const float vy = params[Y_GX].value * g[0] + params[Y_GY].value * g[1] + params[Y_GZ].value * g[2];
  const float speed = sqrtf(vx * vx + vy * vy);

  if (frozen) {
    // Cursore bloccato: si scarta anche il movimento che il filtro stava ancora recuperando.
    lagX = lagY = 0;
    return;
  }
  float stepX = 0, stepY = 0;
  // La zona morta guarda la velocita' complessiva e non i singoli assi: cosi'
  // un movimento obliquo lento non perde una delle due componenti.
  if (speed >= params[DEADZONE].value) {
    // gradi/s * secondi = gradi; gradi * (pixel per grado) = pixel.
    const float pixelsPerDps = dt * params[SENS].value * accelGain(speed);
    stepX = vx * pixelsPerDps;
    stepY = vy * pixelsPerDps;
  }
  smooth(stepX, stepY, speed, dt);
  remainderX += stepX;
  remainderY += stepY;
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
  advertising->setPreferredParams(6, 12);  // Intervallo di connessione preferito: 7,5-15 ms.
  advertising->addServiceUUID(hid.getHidService()->getUUID());
  advertising->enableScanResponse(true);
  advertising->start();
  Serial.println("[BLE] associa Eous Mouse dalle impostazioni Bluetooth del computer");
}

void setup() {
  Serial.begin(115200);
  leftButton.begin();
  rightButton.begin();
  clutchButton.begin();
  // Gli altri contatti restano ingressi e non generano eventi mouse.
  for (uint8_t pin : UNUSED_PINS) pinMode(pin, INPUT_PULLUP);
  delay(1000);
  Serial.println("\n=== Eous Mouse: GPIO15 sinistro, GPIO16 destro, GPIO17 frizione ===");
  loadParams();
  setupMpu();
  setupBluetooth();
}

void loop() {
  const uint32_t now = millis();
  leftButton.update(now);
  rightButton.update(now);
  clutchButton.update(now);
  readCommands();

  // Cadenza: un passo ogni SAMPLE_MS. Sommare il periodo invece di ripartire da
  // "adesso" tiene esatta la frequenza media anche se un passo arriva in ritardo.
  static uint32_t tickUs = micros();
  static uint32_t lastSampleUs = tickUs;
  const uint32_t nowUs = micros();
  if (nowUs - tickUs < SAMPLE_MS * 1000) {
    delay(1);
    return;
  }
  tickUs += SAMPLE_MS * 1000;
  if (nowUs - tickUs > 4 * SAMPLE_MS * 1000) tickUs = nowUs;  // Dopo una pausa lunga non rincorre.

  // Frizione e blocco al click: premere un contatto muove la mano, quindi per un
  // attimo dopo ogni pressione o rilascio il cursore non segue il movimento.
  const int clutchMode = static_cast<int>(params[CLUTCH].value);
  const bool clutched = (clutchMode == 1 && !clutchButton.pressed) ||
                        (clutchMode == 2 && clutchButton.pressed);
  const float clickMs = params[CLICK_MS].value;
  const bool clicking = now - leftButton.changedAt < clickMs || now - rightButton.changedAt < clickMs;

  // Dati dell'intervallo tra due report, per il flusso verso il PC.
  static int32_t gyroSum[3] = {0, 0, 0};
  static int16_t accel[3] = {0, 0, 0};
  static uint8_t samples = 0, flags = 0, steps = 0;

  int16_t raw[6];
  if (mpuReady && readMpu(raw)) {
    const float dt = min((nowUs - lastSampleUs) * 1e-6f, 0.05f);  // Tempo reale trascorso, in secondi.
    processSample(raw, dt, clutched || clicking);
    for (int a = 0; a < 3; ++a) {
      accel[a] = raw[a];
      gyroSum[a] += raw[3 + a];
      if (raw[3 + a] == INT16_MAX || raw[3 + a] == INT16_MIN) flags |= 1;  // Giroscopio a fondo scala.
    }
    if (clutched) flags |= 2;
    if (clicking) flags |= 4;
    ++samples;
  } else {
    remainderX = remainderY = lagX = lagY = 0;
  }
  lastSampleUs = nowUs;

  if (++steps < SAMPLES_PER_REPORT) return;
  steps = 0;
  const int8_t dx = takeWhole(remainderX);
  const int8_t dy = takeWhole(remainderY);

  // Dati per il PC: tempo (microsecondi), accelerometro, giroscopio medio
  // dell'intervallo, movimento inviato, stato (1 saturato, 2 frizione, 4 click).
  if (streaming && samples > 0) {
    Serial.printf("D,%lu,%d,%d,%d,%ld,%ld,%ld,%d,%d,%u\n", static_cast<unsigned long>(nowUs),
                  accel[0], accel[1], accel[2],
                  lroundf(gyroSum[0] / static_cast<float>(samples)),
                  lroundf(gyroSum[1] / static_cast<float>(samples)),
                  lroundf(gyroSum[2] / static_cast<float>(samples)), dx, dy, flags);
  }
  gyroSum[0] = gyroSum[1] = gyroSum[2] = 0;
  samples = flags = 0;

  if (!reportSubscribed.load()) return;

  const uint8_t buttons = (leftButton.pressed ? 0x01 : 0) |
                          (rightButton.pressed ? 0x02 : 0);
  const uint8_t report[] = {buttons, static_cast<uint8_t>(dx), static_cast<uint8_t>(dy)};
  // Invia anche da fermo: pressione, trascinamento e rilascio non dipendono dal sensore.
  // Ripetere lo stato permette inoltre di sincronizzarsi dopo una riconnessione.
  mouseReport->setValue(report, sizeof(report));
  mouseReport->notify();
}
