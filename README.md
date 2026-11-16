# Eous

**Scegli la lingua / Choose your language:** [Italiano](#italiano) | [English](#english)

<a id="italiano"></a>
## Italiano

Eous è un prototipo di mouse Bluetooth Low Energy (BLE HID) basato su ESP32-S3.
Il giroscopio di un MPU6050 controlla il cursore, mentre due pulsanti o contatti
collegati a massa permettono di eseguire il clic sinistro e destro. Il computer
usa il proprio supporto per mouse Bluetooth, senza un'applicazione dedicata.

### Indice

- [Funzionamento](#it-funzionamento)
- [Componenti e strumenti](#it-componenti)
- [Collegamenti ESP32-S3, scheda MPU6050 e terminali](#it-collegamenti)
- [Compilazione e caricamento](#it-installazione)
- [Avvio e associazione Bluetooth](#it-avvio)
- [Regolazioni](#it-regolazioni)
- [Struttura del progetto e programmi sperimentali](#it-struttura)
- [Risoluzione dei problemi](#it-problemi)
- [Licenza](#it-licenza)

<a id="it-funzionamento"></a>
### Funzionamento

- Il dispositivo si presenta al computer con il nome **Eous Mouse**.
- La rotazione attorno all'asse Z del sensore controlla il movimento orizzontale;
  la rotazione attorno all'asse X controlla quello verticale.
- Il contatto tra GPIO15 e GND attiva il pulsante sinistro; quello tra GPIO16
  e GND attiva il destro. Mantenendo chiuso il contatto si tiene premuto il pulsante,
  anche per trascinare.
- Il firmware applica un antirimbalzo di 20 ms ai contatti e invia lo stato del mouse
  ogni 20 ms quando il computer ha abilitato le notifiche HID.
- All'avvio il giroscopio viene calibrato. Se il sensore non è disponibile o la
  calibrazione fallisce, i pulsanti continuano a funzionare.

La versione attuale non implementa la rotella o il clic centrale. Il livello batteria
Bluetooth è impostato al 100% come segnaposto: non è una misura reale.

<a id="it-componenti"></a>
### Componenti e strumenti

- Scheda ESP32-S3: il progetto PlatformIO usa il profilo `esp32-s3-devkitc-1`,
  con flash da 16 MB e configurazione PSRAM `qio_opi`. Verifica che la tua scheda
  corrisponda a queste impostazioni.
- Modulo MPU6050 compatibile con alimentazione e segnali I²C a 3,3 V.
- Due pulsanti normalmente aperti, oppure due coppie di terminali che chiudano
  un contatto elettrico verso GND.
- Cavetti di collegamento ed eventualmente una breadboard.
- Cavo USB dati per alimentare la scheda e caricare il firmware.
- Computer con supporto per mouse BLE HID e PlatformIO installato
  (per esempio tramite l'estensione PlatformIO IDE per Visual Studio Code).

Le dipendenze dichiarate in [platformio.ini](firmware/platformio.ini) sono il
framework Arduino per ESP32 e **NimBLE-Arduino 2.5.2**.

<a id="it-collegamenti"></a>
### Collegamenti ESP32-S3, scheda MPU6050 e terminali

I collegamenti seguenti corrispondono al firmware attivo in
[main.cpp](firmware/src/main.cpp). I numeri indicano i **GPIO**, non la posizione
fisica dei piedini sul connettore della scheda.

#### Scheda sensore MPU6050

| ESP32-S3 | Terminale MPU6050 | Funzione |
| --- | --- | --- |
| 3V3 | VCC, su un modulo compatibile con 3,3 V | Alimentazione del modulo |
| GND | GND | Massa comune |
| GPIO8 | SDA | Dati I²C |
| GPIO9 | SCL | Clock I²C |
| GND | AD0 | Selezione dell'indirizzo I²C `0x68` |
| Nessun collegamento | INT, XDA e XCL, se presenti | Non utilizzati dal firmware |

Il bus I²C è configurato a 400 kHz. Verifica le specifiche di alimentazione del
tuo modulo prima di collegare VCC; SDA e SCL devono usare livelli logici a 3,3 V.

#### Terminali dei pulsanti / contatti

| Terminale o pulsante | Primo capo | Secondo capo | Azione quando il contatto è chiuso |
| --- | --- | --- | --- |
| Sinistro | GPIO15 | GND | Pulsante sinistro premuto |
| Destro | GPIO16 | GND | Pulsante destro premuto |
| Eventuali contatti aggiuntivi | GPIO17 / GPIO18 | Lasciare scollegati per l'uso attuale | Nessuna azione mouse |

GPIO15 e GPIO16 usano `INPUT_PULLUP`: a riposo il contatto è aperto e il livello
è HIGH; chiudendolo verso GND diventa LOW. La pull-up è interna, quindi per questi
contatti non serve una resistenza di pull-up esterna. GPIO17 e GPIO18 sono
configurati come ingressi con pull-up, ma non generano eventi mouse.

```text
ESP32-S3                         MPU6050
3V3 ---------------------------- VCC (modulo compatibile con 3,3 V)
GND ---------------------------- GND
GPIO8 -------------------------- SDA
GPIO9 -------------------------- SCL
GND ---------------------------- AD0

GPIO15 ---- [contatto sinistro] -- GND
GPIO16 ---- [contatto destro] ---- GND

Computer -- USB ---------------- ESP32-S3 (alimentazione / caricamento / log)
Computer <------ Bluetooth -----> ESP32-S3 (mouse BLE HID)
```

Tutti i collegamenti GND condividono la stessa massa. I contatti del firmware
principale devono chiudersi verso GND: collegare soltanto due GPIO tra loro
non equivale a premere un pulsante.

<a id="it-installazione"></a>
### Compilazione e caricamento

1. Apri la cartella `firmware` in PlatformIO IDE, oppure usa i comandi seguenti
   dalla cartella principale del repository.
2. Collega l'ESP32-S3 al computer con un cavo USB dati.
3. Compila e carica il firmware:

   ```bash
   pio run -d firmware -e esp32s3
   pio run -d firmware -e esp32s3 -t upload
   ```

4. Apri il monitor seriale a **115200 baud**:

   ```bash
   pio device monitor -d firmware -b 115200
   ```

Se PlatformIO non seleziona la porta corretta, elenca le porte con
`pio device list`, aggiungi `--upload-port PORTA` al comando di caricamento
e `--port PORTA` al comando del monitor, sostituendo `PORTA` con quella della scheda.

La configurazione `build_src_filter = +<main.cpp>` compila soltanto il firmware
principale; i programmi sperimentali restano esclusi.

<a id="it-avvio"></a>
### Avvio e associazione Bluetooth

1. All'accensione o al riavvio, tieni fermo il sensore durante la calibrazione
   (circa un secondo di campionamento, dopo il ritardo iniziale).
2. Nel monitor seriale controlla il messaggio `[MPU] pronto`. Il firmware usa
   attualmente messaggi seriali in italiano.
3. Apri le impostazioni Bluetooth del computer e associa **Eous Mouse**.
4. Ruota il sensore per muovere il cursore e chiudi i contatti per provare i clic.

Dopo una disconnessione l'ESP32-S3 riprende l'annuncio Bluetooth. Per ricalibrare
il giroscopio riavvia la scheda mantenendo fermo il sensore.

<a id="it-regolazioni"></a>
### Regolazioni

Le costanti si trovano all'inizio di [main.cpp](firmware/src/main.cpp).

| Costante | Valore attuale | Effetto |
| --- | --- | --- |
| `LEFT_PIN` / `RIGHT_PIN` | `15` / `16` | GPIO dei contatti sinistro e destro |
| `SDA_PIN` / `SCL_PIN` | `8` / `9` | GPIO del bus I²C |
| `MPU_ADDR` | `0x68` | Indirizzo del sensore con AD0 a GND |
| `SENSITIVITY` | `0.2f` | Aumentandola, il cursore si muove più velocemente |
| `DEADZONE` | `1.0f` | Soglia in gradi/s sotto cui il movimento viene ignorato |
| `DEBOUNCE_MS` | `20` | Tempo di antirimbalzo dei contatti |
| `SEND_MS` | `20` | Intervallo tra i report mouse, in millisecondi |

Per invertire un verso di movimento modifica il segno nelle espressioni di `vx`
o `vy` nel `loop()`. Dopo ogni modifica ricompila e ricarica il firmware.

<a id="it-struttura"></a>
### Struttura del progetto e programmi sperimentali

| Percorso | Contenuto |
| --- | --- |
| [firmware/platformio.ini](firmware/platformio.ini) | Scheda, ambiente di compilazione e dipendenze |
| [firmware/src/main.cpp](firmware/src/main.cpp) | Firmware attivo: mouse BLE, MPU6050 e due contatti |
| [firmware/src/touch_test_2.cpp](firmware/src/touch_test_2.cpp) | Esperimento: contatti verso GND oppure scansione dei collegamenti tra GPIO15–18; attualmente `MODE 2` |
| [firmware/test/touch_test_1.cpp](firmware/test/touch_test_1.cpp) | Esperimento: touch capacitivo su GPIO4 e contatto verso GND su GPIO15 |
| [firmware/test/mpu_test_1.cpp](firmware/test/mpu_test_1.cpp) | Esperimento: scansione I²C e lettura di accelerometro, giroscopio e temperatura |
| [firmware/test/mpu_test_2.cpp](firmware/test/mpu_test_2.cpp) | Prototipo mouse BLE con giroscopio o movimento di prova a quadrato |
| [firmware/test/ble_test.cpp](firmware/test/ble_test.cpp) | Esperimento BLE: dispositivo “Eous” che notifica un contatore |

Questi esperimenti sono programmi autonomi con proprie funzioni `setup()` e
`loop()`, non una suite di test automatizzati. Per eseguirne uno va predisposta
una compilazione separata che selezioni un solo programma. In particolare,
la scansione tra GPIO di `touch_test_2.cpp` usa un cablaggio diverso dai contatti
verso GND del firmware principale.

<a id="it-problemi"></a>
### Risoluzione dei problemi

| Problema | Controlli |
| --- | --- |
| Sensore non disponibile o calibrazione fallita | Controlla alimentazione, massa comune, SDA su GPIO8, SCL su GPIO9 e AD0 a GND, poi riavvia. I clic restano disponibili. |
| Cursore che si muove a riposo | Riavvia tenendo fermo il sensore; se necessario regola `DEADZONE`. |
| Clic non rilevati | Verifica che ogni contatto chiuda il GPIO corretto verso GND e che il mouse sia connesso. |
| Mouse non visibile o associazione non riuscita | Controlla alimentazione e messaggi seriali; se esiste una vecchia associazione, rimuovila dal computer e ripeti l'associazione. |
| Nessun messaggio seriale | Controlla cavo USB dati, porta selezionata e velocità di 115200 baud; riavvia con il monitor aperto per leggere i messaggi iniziali. |

<a id="it-licenza"></a>
### Licenza

Il progetto è distribuito con [licenza MIT](LICENSE).

[Torna alla scelta della lingua](#eous) · [Vai alla versione inglese](#english)

---

<a id="english"></a>
## English

Eous is a Bluetooth Low Energy (BLE HID) mouse prototype based on the ESP32-S3.
An MPU6050 gyroscope controls the pointer, while two buttons or contacts wired
to ground provide left and right clicks. The computer uses its built-in Bluetooth
mouse support, without a dedicated application.

### Table of contents

- [How it works](#en-operation)
- [Components and tools](#en-components)
- [ESP32-S3, MPU6050 board and terminal connections](#en-wiring)
- [Building and uploading](#en-installation)
- [Startup and Bluetooth pairing](#en-startup)
- [Adjustments](#en-adjustments)
- [Project structure and experimental programs](#en-structure)
- [Troubleshooting](#en-troubleshooting)
- [License](#en-license)

<a id="en-operation"></a>
### How it works

- The device appears on the computer as **Eous Mouse**.
- Rotation around the sensor's Z axis controls horizontal movement; rotation
  around its X axis controls vertical movement.
- Closing the contact between GPIO15 and GND presses the left mouse button;
  GPIO16 to GND presses the right button. Keeping a contact closed holds the
  button down, including for dragging.
- The firmware debounces contacts for 20 ms and sends the mouse state every
  20 ms when the computer has enabled HID notifications.
- The gyroscope is calibrated at startup. If the sensor is unavailable or
  calibration fails, the buttons keep working.

The current version does not implement scrolling or a middle click. The Bluetooth
battery level is set to 100% as a placeholder; it is not a real measurement.

<a id="en-components"></a>
### Components and tools

- ESP32-S3 board: the PlatformIO project uses the `esp32-s3-devkitc-1` profile
  with 16 MB flash and the `qio_opi` PSRAM configuration. Check that your board
  matches these settings.
- MPU6050 module compatible with a 3.3 V supply and 3.3 V I²C signals.
- Two normally open buttons, or two pairs of terminals that close an electrical
  contact to GND.
- Jumper wires and, optionally, a breadboard.
- USB data cable to power the board and upload the firmware.
- Computer with BLE HID mouse support and PlatformIO installed (for example,
  through the PlatformIO IDE extension for Visual Studio Code).

The dependencies declared in [platformio.ini](firmware/platformio.ini) are the
Arduino framework for ESP32 and **NimBLE-Arduino 2.5.2**.

<a id="en-wiring"></a>
### ESP32-S3, MPU6050 board and terminal connections

These connections match the active firmware in [main.cpp](firmware/src/main.cpp).
The numbers refer to **GPIO numbers**, not physical header positions on the board.

#### MPU6050 sensor board

| ESP32-S3 | MPU6050 terminal | Purpose |
| --- | --- | --- |
| 3V3 | VCC, on a 3.3 V compatible module | Module power supply |
| GND | GND | Common ground |
| GPIO8 | SDA | I²C data |
| GPIO9 | SCL | I²C clock |
| GND | AD0 | Selects I²C address `0x68` |
| No connection | INT, XDA and XCL, if present | Not used by the firmware |

The I²C bus runs at 400 kHz. Check your module's power supply specifications
before connecting VCC; SDA and SCL must use 3.3 V logic levels.

#### Button / contact terminals

| Terminal or button | First end | Second end | Action when the contact is closed |
| --- | --- | --- | --- |
| Left | GPIO15 | GND | Left mouse button held down |
| Right | GPIO16 | GND | Right mouse button held down |
| Any additional contacts | GPIO17 / GPIO18 | Leave disconnected for current use | No mouse action |

GPIO15 and GPIO16 use `INPUT_PULLUP`: an open contact reads HIGH, and closing it
to GND reads LOW. The pull-up is internal, so these contacts do not need external
pull-up resistors. GPIO17 and GPIO18 are configured as inputs with pull-ups but
do not generate mouse events.

```text
ESP32-S3                         MPU6050
3V3 ---------------------------- VCC (3.3 V compatible module)
GND ---------------------------- GND
GPIO8 -------------------------- SDA
GPIO9 -------------------------- SCL
GND ---------------------------- AD0

GPIO15 ---- [left contact] ------ GND
GPIO16 ---- [right contact] ----- GND

Computer -- USB ---------------- ESP32-S3 (power / upload / logs)
Computer <------ Bluetooth -----> ESP32-S3 (BLE HID mouse)
```

All GND connections share the same ground. The main firmware requires contacts
that close to GND: connecting only two GPIO pins to each other does not act as
a button press.

<a id="en-installation"></a>
### Building and uploading

1. Open the `firmware` folder in PlatformIO IDE, or run the following commands
   from the repository root.
2. Connect the ESP32-S3 to your computer with a USB data cable.
3. Build and upload the firmware:

   ```bash
   pio run -d firmware -e esp32s3
   pio run -d firmware -e esp32s3 -t upload
   ```

4. Open the serial monitor at **115200 baud**:

   ```bash
   pio device monitor -d firmware -b 115200
   ```

If PlatformIO does not select the correct port, list ports with `pio device list`,
add `--upload-port PORT` to the upload command and `--port PORT` to the monitor
command, replacing `PORT` with your board's port.

The setting `build_src_filter = +<main.cpp>` builds only the main firmware;
experimental programs are excluded.

<a id="en-startup"></a>
### Startup and Bluetooth pairing

1. At power-on or reset, keep the sensor still during calibration (about one
   second of sampling, after the initial delay).
2. Check the serial monitor for `[MPU] pronto` (sensor ready). The firmware's
   serial messages are currently in Italian.
3. Open your computer's Bluetooth settings and pair **Eous Mouse**.
4. Rotate the sensor to move the pointer and close the contacts to test clicks.

After a disconnection, the ESP32-S3 resumes Bluetooth advertising. To recalibrate
the gyroscope, restart the board while keeping the sensor still.

<a id="en-adjustments"></a>
### Adjustments

The constants are at the beginning of [main.cpp](firmware/src/main.cpp).

| Constant | Current value | Effect |
| --- | --- | --- |
| `LEFT_PIN` / `RIGHT_PIN` | `15` / `16` | GPIO pins for the left and right contacts |
| `SDA_PIN` / `SCL_PIN` | `8` / `9` | I²C bus GPIO pins |
| `MPU_ADDR` | `0x68` | Sensor address with AD0 connected to GND |
| `SENSITIVITY` | `0.2f` | Increase it for faster pointer movement |
| `DEADZONE` | `1.0f` | Threshold in degrees/s below which movement is ignored |
| `DEBOUNCE_MS` | `20` | Contact debounce time |
| `SEND_MS` | `20` | Interval between mouse reports, in milliseconds |

To reverse a movement direction, change the sign in the `vx` or `vy` expressions
inside `loop()`. Rebuild and upload the firmware after each change.

<a id="en-structure"></a>
### Project structure and experimental programs

| Path | Contents |
| --- | --- |
| [firmware/platformio.ini](firmware/platformio.ini) | Board, build environment and dependencies |
| [firmware/src/main.cpp](firmware/src/main.cpp) | Active firmware: BLE mouse, MPU6050 and two contacts |
| [firmware/src/touch_test_2.cpp](firmware/src/touch_test_2.cpp) | Experiment: contacts to GND or connection scanning between GPIO15–18; currently `MODE 2` |
| [firmware/test/touch_test_1.cpp](firmware/test/touch_test_1.cpp) | Experiment: capacitive touch on GPIO4 and a contact to GND on GPIO15 |
| [firmware/test/mpu_test_1.cpp](firmware/test/mpu_test_1.cpp) | Experiment: I²C scan and accelerometer, gyroscope and temperature readings |
| [firmware/test/mpu_test_2.cpp](firmware/test/mpu_test_2.cpp) | BLE mouse prototype with gyroscope input or square test movement |
| [firmware/test/ble_test.cpp](firmware/test/ble_test.cpp) | BLE experiment: an “Eous” device that sends counter notifications |

These experiments are standalone programs with their own `setup()` and `loop()`
functions, rather than an automated test suite. Running one requires a separate
build configuration that selects a single program. In particular, the GPIO
connection scan in `touch_test_2.cpp` uses different wiring from the main
firmware's contacts to GND.

<a id="en-troubleshooting"></a>
### Troubleshooting

| Problem | Checks |
| --- | --- |
| Sensor unavailable or calibration failed | Check power, common ground, SDA on GPIO8, SCL on GPIO9 and AD0 to GND, then restart. Clicks remain available. |
| Pointer moves while the sensor is still | Restart while keeping the sensor still; adjust `DEADZONE` if needed. |
| Clicks are not detected | Check that each contact closes the correct GPIO to GND and that the mouse is connected. |
| Mouse is not visible or pairing fails | Check power and serial messages; if an old pairing exists, remove it from the computer and pair again. |
| No serial messages | Check the USB data cable, selected port and 115200 baud rate; restart with the monitor open to read startup messages. |

<a id="en-license"></a>
### License

The project is distributed under the [MIT license](LICENSE).

[Back to language selection](#eous) · [Go to the Italian version](#italiano)
