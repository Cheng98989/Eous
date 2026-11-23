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
- [Calibrazione e regolazione dal PC](#it-calibrazione)
- [Gesti](#it-gesti)
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
- Il contatto tra GPIO17 e GND è la frizione: finché è chiuso il cursore resta
  fermo, come quando si solleva un mouse per riposizionare la mano. Il
  comportamento si può invertire o disattivare.
- Il firmware applica un antirimbalzo di 20 ms ai contatti, legge il sensore
  ogni 5 ms e invia lo stato del mouse ogni 10 ms quando il computer ha abilitato
  le notifiche HID.
- Un filtro smorza il tremolio a mano lenta senza ritardare i movimenti veloci,
  e il cursore resta fermo per un istante dopo ogni pressione o rilascio di un
  pulsante, perché il gesto del clic non lo sposti.
- L'offset del giroscopio viene misurato all'avvio e corretto ogni volta che il
  sensore resta fermo per due secondi. Se all'avvio il sensore si muove, viene
  usato l'ultimo offset salvato. Se il sensore non è disponibile, i pulsanti
  continuano a funzionare.
- Sensibilità, zona morta, accelerazione del cursore e orientamento del sensore
  si regolano dal computer attraverso la porta seriale, senza ricompilare.
- In parallelo al movimento del cursore, il firmware riconosce gesti dei pad e
  della mano e li comunica al computer via seriale. Per ora non eseguono comandi.

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
| Frizione (facoltativa) | GPIO17 | GND | Cursore fermo finché il contatto è chiuso |
| Pad dei gesti (facoltativo) | GPIO18 | GND | Nessuna azione mouse: solo gesti |

I quattro GPIO sono i pad delle dita e GND va al pollice: ogni dito chiude il
proprio contatto toccando il pollice. Usano `INPUT_PULLUP`: a riposo il contatto è
aperto e il livello è HIGH; chiudendolo verso GND diventa LOW. La pull-up è interna,
quindi non serve una resistenza esterna. Senza nulla collegato a GPIO17 la frizione
non interviene. Tutti e quattro i pad producono anche i gesti descritti più avanti.

```text
ESP32-S3                         MPU6050
3V3 ---------------------------- VCC (modulo compatibile con 3,3 V)
GND ---------------------------- GND
GPIO8 -------------------------- SDA
GPIO9 -------------------------- SCL
GND ---------------------------- AD0

GPIO15 ---- [contatto sinistro] -- GND
GPIO16 ---- [contatto destro] ---- GND
GPIO17 ---- [frizione] ----------- GND
GPIO18 ---- [pad dei gesti] ------ GND

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
   (circa un secondo di campionamento, dopo il ritardo iniziale). Se si muove,
   il firmware riprova due volte e poi usa l'ultimo offset salvato.
2. Nel monitor seriale controlla il messaggio `[MPU] pronto`. Il firmware usa
   attualmente messaggi seriali in italiano.
3. Apri le impostazioni Bluetooth del computer e associa **Eous Mouse**.
4. Ruota il sensore per muovere il cursore e chiudi i contatti per provare i clic.

Dopo una disconnessione l'ESP32-S3 riprende l'annuncio Bluetooth. Per ricalibrare
il giroscopio basta appoggiare il sensore e lasciarlo fermo per due secondi.

<a id="it-regolazioni"></a>
### Regolazioni

Le costanti si trovano all'inizio di [main.cpp](firmware/src/main.cpp).

| Costante | Valore attuale | Effetto |
| --- | --- | --- |
| `LEFT_PIN` / `RIGHT_PIN` | `15` / `16` | GPIO dei contatti sinistro e destro |
| `CLUTCH_PIN` | `17` | GPIO del contatto della frizione |
| `SDA_PIN` / `SCL_PIN` | `8` / `9` | GPIO del bus I²C |
| `MPU_ADDR` | `0x68` | Indirizzo del sensore con AD0 a GND |
| `DEBOUNCE_MS` | `20` | Tempo di antirimbalzo dei contatti |
| `SAMPLE_MS` | `5` | Intervallo tra le letture del sensore, in millisecondi |
| `SAMPLES_PER_REPORT` | `2` | Letture del sensore per ogni report mouse (un report ogni 10 ms) |

Dopo ogni modifica ricompila e ricarica il firmware. Sensibilità, zona morta,
accelerazione e versi di movimento non sono costanti: si cambiano dal computer,
come descritto nella sezione seguente.

<a id="it-calibrazione"></a>
### Calibrazione e regolazione dal PC

[tools/eous_tune.py](tools/eous_tune.py) comunica con il firmware attraverso la
porta seriale USB. Richiede Python 3 e `pyserial`; il monitor seriale deve essere
chiuso, perché la porta va usata da un solo programma alla volta.

```bash
pip install pyserial
python tools/eous_tune.py
```

| Comando | Effetto |
| --- | --- |
| `mostra` | Elenca i parametri attuali |
| `set NOME VALORE` | Cambia un parametro, subito attivo |
| `salva` / `reset` | Conserva i parametri nella memoria del dispositivo / ripristina quelli iniziali |
| `curva` | Mostra la velocità del cursore per ogni velocità della mano |
| `bias` | Misura offset e rumore del giroscopio a sensore fermo |
| `assi` | Impara da due movimenti quale rotazione significa destra e quale su |
| `drift` | Registra un movimento con ritorno al punto di partenza e indica da dove nasce l'eventuale deriva |
| `live` / `registra FILE` | Mostra i dati del sensore in tempo reale / li salva in CSV |
| `gesti` | Mostra i gesti riconosciuti dal firmware e i pad chiusi |

| Parametro | Valore iniziale | Effetto |
| --- | --- | --- |
| `sens` | `10` | Pixel per grado di rotazione |
| `deadzone` | `1` | Velocità in gradi/s sotto cui il cursore resta fermo |
| `acc_lo` / `acc_hi` | `50` / `400` | Velocità della mano, in gradi/s, tra cui l'accelerazione cresce |
| `acc_max` | `1` | Moltiplicatore massimo dell'accelerazione; `1` la disattiva |
| `filt_hz` | `3` | Filtro: frequenza di taglio a mano ferma, in Hz; più bassa smorza di più, `0` lo spegne |
| `filt_beta` | `0.1` | Filtro: di quanti Hz sale il taglio per ogni grado/s di velocità della mano |
| `click_ms` | `120` | Millisecondi di cursore fermo dopo ogni pressione o rilascio di un pulsante; `0` lo disattiva |
| `clutch` | `2` | Frizione: `0` non usata, `1` contatto chiuso per muovere, `2` contatto chiuso per bloccare |
| `autobias` | `1` | `1` corregge l'offset del giroscopio da solo a sensore fermo |
| `x_gx` … `y_gz` | X = −gz, Y = −gx | Contributo di ogni asse del giroscopio al movimento del cursore |

[tools/eous_gui.py](tools/eous_gui.py) offre le stesse regolazioni in una pagina
del browser, con un cursore virtuale che mostra il movimento inviato dal firmware:

```bash
python tools/eous_gui.py
```

La pagina è servita solo su `127.0.0.1` e non richiede connessione a Internet.
Console e interfaccia grafica non possono usare la porta seriale insieme.

Il protocollo è testuale, una riga per comando: `get`, `set nome valore`,
`stream 0|1`, `save`, `reset`. Si può quindi provare anche dal monitor seriale.

<a id="it-gesti"></a>
### Gesti

Il firmware riconosce i gesti sull'ESP32, in parallelo al movimento del cursore:
un gesto della mano sposta anche il cursore. Ogni gesto viene inviato sulla seriale
come riga `G,tipo,dettaglio,durata ms,ampiezza gradi,picco gradi/s`, visibile anche
nel monitor seriale. L'interfaccia grafica li mostra nel riquadro «Gesti
riconosciuti dal firmware»; dalla console si usa il comando `gesti`.

| Tipo | Dettaglio | Quando |
| --- | --- | --- |
| `tocco` | GPIO del pad | Pad chiuso meno di `hold_ms` e non ripetuto entro `dbl_ms` |
| `doppio` | GPIO del pad | Secondo tocco entro `dbl_ms` dal primo |
| `tenuto` / `rilascio` | GPIO del pad | Pad chiuso oltre `hold_ms` / sua riapertura, con la durata totale |
| `combo` | GPIO dei pad, per esempio `15+16` | Due o più pad chiusi insieme; annulla i gesti dei singoli pad |
| `scatto` | `destra`, `su_destra`, `su`, … (8 direzioni) | Movimento rapido del polso: almeno `g_angle` gradi a `g_peak` gradi/s |
| `rotazione` | `oraria` / `antioraria`, vista dal polso | Rotazione della mano su se stessa di almeno `flip_deg` gradi in meno di `flip_ms` |
| `scossa` | Numero di tratti | Almeno `shake_n` movimenti rapidi alternati avanti e indietro |
| `ritorno` | Direzione | Gesto opposto al precedente entro 400 ms: la mano che torna, ignorato |
| `scartato` | Motivo | Movimento quasi riconosciuto: mostra le misure per regolare le soglie |

Il riconoscimento del movimento divide la velocità della mano in tratti, che
finiscono quando la mano rallenta o inverte la direzione. Il primo tratto veloce
decide direzione e tipo del gesto; una serie di tratti veloci alternati è una
scossa. Per la mano destra, girare il palmo dal basso verso l'alto dovrebbe
risultare una rotazione oraria.

| Parametro | Valore iniziale | Effetto |
| --- | --- | --- |
| `hold_ms` | `400` | Millisecondi dopo cui un pad chiuso diventa «tenuto» |
| `dbl_ms` | `250` | Pausa massima tra i due tocchi di un doppio tocco; è anche il ritardo con cui arriva un tocco semplice |
| `g_peak` | `150` | Velocità di picco minima, in gradi/s, perché un movimento conti come gesto |
| `g_angle` | `15` | Ampiezza minima di uno scatto, in gradi |
| `flip_deg` / `flip_ms` | `90` / `400` | Ampiezza minima e durata massima di una rotazione rapida |
| `shake_n` | `3` | Tratti alternati minimi di una scossa |

<a id="it-struttura"></a>
### Struttura del progetto e programmi sperimentali

| Percorso | Contenuto |
| --- | --- |
| [firmware/platformio.ini](firmware/platformio.ini) | Scheda, ambiente di compilazione e dipendenze |
| [firmware/src/main.cpp](firmware/src/main.cpp) | Firmware attivo: mouse BLE, MPU6050 e due contatti |
| [tools/eous_tune.py](tools/eous_tune.py) | Calibrazione, regolazione e gesti dal PC attraverso la seriale USB |
| [tools/eous_gui.py](tools/eous_gui.py), [tools/eous_gui.html](tools/eous_gui.html) | Interfaccia grafica nel browser: cursore virtuale e regolazioni |
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
| Cursore che si muove a riposo | Appoggia il sensore e lascialo fermo per due secondi: l'offset si corregge da solo. Se necessario aumenta `deadzone`. |
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
- [Calibration and tuning from the PC](#en-calibration)
- [Gestures](#en-gestures)
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
- The contact between GPIO17 and GND is the clutch: while it is closed the
  pointer stays still, like lifting a mouse to reposition your hand. The
  behaviour can be inverted or turned off.
- The firmware debounces contacts for 20 ms, reads the sensor every 5 ms and
  sends the mouse state every 10 ms when the computer has enabled HID
  notifications.
- A filter damps tremor when the hand moves slowly without delaying fast
  movements, and the pointer stays still for a moment after each button press
  or release, so the click gesture does not move it.
- The gyroscope offset is measured at startup and corrected whenever the sensor
  stays still for two seconds. If the sensor is moving at startup, the last
  stored offset is used. If the sensor is unavailable, the buttons keep working.
- Sensitivity, deadzone, pointer acceleration and sensor orientation are tuned
  from the computer over the serial port, without rebuilding.
- Alongside pointer movement, the firmware recognizes pad and hand gestures and
  reports them to the computer over serial. They do not run any command yet.

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
| Clutch (optional) | GPIO17 | GND | Pointer held still while the contact is closed |
| Gesture pad (optional) | GPIO18 | GND | No mouse action: gestures only |

The four GPIO pins are the finger pads and GND goes to the thumb: each finger
closes its own contact by touching the thumb. They use `INPUT_PULLUP`: an open
contact reads HIGH, and closing it to GND reads LOW. The pull-up is internal, so no
external resistor is needed. With nothing connected to GPIO17 the clutch never
engages. All four pads also produce the gestures described below.

```text
ESP32-S3                         MPU6050
3V3 ---------------------------- VCC (3.3 V compatible module)
GND ---------------------------- GND
GPIO8 -------------------------- SDA
GPIO9 -------------------------- SCL
GND ---------------------------- AD0

GPIO15 ---- [left contact] ------ GND
GPIO16 ---- [right contact] ----- GND
GPIO17 ---- [clutch] ------------ GND
GPIO18 ---- [gesture pad] ------- GND

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
   second of sampling, after the initial delay). If it moves, the firmware
   retries twice and then uses the last stored offset.
2. Check the serial monitor for `[MPU] pronto` (sensor ready). The firmware's
   serial messages are currently in Italian.
3. Open your computer's Bluetooth settings and pair **Eous Mouse**.
4. Rotate the sensor to move the pointer and close the contacts to test clicks.

After a disconnection, the ESP32-S3 resumes Bluetooth advertising. To recalibrate
the gyroscope, put the sensor down and leave it still for two seconds.

<a id="en-adjustments"></a>
### Adjustments

The constants are at the beginning of [main.cpp](firmware/src/main.cpp).

| Constant | Current value | Effect |
| --- | --- | --- |
| `LEFT_PIN` / `RIGHT_PIN` | `15` / `16` | GPIO pins for the left and right contacts |
| `CLUTCH_PIN` | `17` | GPIO pin for the clutch contact |
| `SDA_PIN` / `SCL_PIN` | `8` / `9` | I²C bus GPIO pins |
| `MPU_ADDR` | `0x68` | Sensor address with AD0 connected to GND |
| `DEBOUNCE_MS` | `20` | Contact debounce time |
| `SAMPLE_MS` | `5` | Interval between sensor readings, in milliseconds |
| `SAMPLES_PER_REPORT` | `2` | Sensor readings per mouse report (one report every 10 ms) |

Rebuild and upload the firmware after each change. Sensitivity, deadzone,
acceleration and movement directions are not constants: they are changed from
the computer, as described in the next section.

<a id="en-calibration"></a>
### Calibration and tuning from the PC

[tools/eous_tune.py](tools/eous_tune.py) talks to the firmware over the USB
serial port. It requires Python 3 and `pyserial`; close the serial monitor first,
because only one program at a time should use the port. The tool's commands and
messages are currently in Italian.

```bash
pip install pyserial
python tools/eous_tune.py
```

| Command | Effect |
| --- | --- |
| `mostra` | Lists the current parameters |
| `set NAME VALUE` | Changes a parameter, effective immediately |
| `salva` / `reset` | Stores the parameters in the device's memory / restores the initial ones |
| `curva` | Shows the pointer speed for each hand speed |
| `bias` | Measures gyroscope offset and noise while the sensor is still |
| `assi` | Learns from two movements which rotation means right and which means up |
| `drift` | Records a movement that returns to its starting point and reports where any drift comes from |
| `live` / `registra FILE` | Shows the sensor data in real time / saves it as CSV |
| `gesti` | Shows the gestures recognized by the firmware and the closed pads |

| Parameter | Initial value | Effect |
| --- | --- | --- |
| `sens` | `10` | Pixels per degree of rotation |
| `deadzone` | `1` | Speed in degrees/s below which the pointer stays still |
| `acc_lo` / `acc_hi` | `50` / `400` | Hand speeds, in degrees/s, between which acceleration ramps up |
| `acc_max` | `1` | Maximum acceleration multiplier; `1` turns it off |
| `filt_hz` | `3` | Filter: cutoff frequency with the hand still, in Hz; lower damps more, `0` turns it off |
| `filt_beta` | `0.1` | Filter: how many Hz the cutoff rises per degree/s of hand speed |
| `click_ms` | `120` | Milliseconds the pointer stays still after each button press or release; `0` turns it off |
| `clutch` | `2` | Clutch: `0` unused, `1` contact closed to move, `2` contact closed to hold still |
| `autobias` | `1` | `1` corrects the gyroscope offset automatically while the sensor is still |
| `x_gx` … `y_gz` | X = −gz, Y = −gx | Contribution of each gyroscope axis to pointer movement |

[tools/eous_gui.py](tools/eous_gui.py) offers the same adjustments in a browser
page, with a virtual pointer that shows the movement sent by the firmware:

```bash
python tools/eous_gui.py
```

The page is served on `127.0.0.1` only and needs no Internet connection. The
console and the graphical interface cannot use the serial port at the same time.

The protocol is plain text, one command per line: `get`, `set name value`,
`stream 0|1`, `save`, `reset`. It can therefore be tried from the serial monitor too.

<a id="en-gestures"></a>
### Gestures

The firmware recognizes gestures on the ESP32, alongside pointer movement: a hand
gesture also moves the pointer. Each gesture is sent over serial as a line
`G,type,detail,duration ms,amplitude degrees,peak degrees/s`, also visible in the
serial monitor. The graphical interface shows them in the «Gesti riconosciuti dal
firmware» panel; in the console, use the `gesti` command. Gesture names are in
Italian.

| Type | Detail | When |
| --- | --- | --- |
| `tocco` (tap) | Pad GPIO | Pad closed for less than `hold_ms` and not repeated within `dbl_ms` |
| `doppio` (double tap) | Pad GPIO | Second tap within `dbl_ms` of the first |
| `tenuto` / `rilascio` (hold / release) | Pad GPIO | Pad closed longer than `hold_ms` / its release, with the total duration |
| `combo` | Pad GPIOs, for example `15+16` | Two or more pads closed together; cancels the single-pad gestures |
| `scatto` (flick) | `destra`, `su_destra`, `su`, … (8 directions) | Quick wrist movement: at least `g_angle` degrees at `g_peak` degrees/s |
| `rotazione` (rotation) | `oraria` / `antioraria` (clockwise / counterclockwise, seen from the wrist) | Hand rolling on itself by at least `flip_deg` degrees in under `flip_ms` |
| `scossa` (shake) | Number of strokes | At least `shake_n` quick alternating back-and-forth movements |
| `ritorno` (return) | Direction | Gesture opposite to the previous one within 400 ms: the hand returning, ignored |
| `scartato` (discarded) | Reason | Almost-recognized movement: shows the measures to help tune thresholds |

Movement recognition splits the hand's speed into strokes, which end when the hand
slows down or reverses. The first fast stroke decides the gesture's direction and
type; a series of fast alternating strokes is a shake. For the right hand, turning
the palm from down to up should register as a clockwise rotation.

| Parameter | Initial value | Effect |
| --- | --- | --- |
| `hold_ms` | `400` | Milliseconds after which a closed pad becomes a hold |
| `dbl_ms` | `250` | Maximum pause between the two taps of a double tap; also the delay before a single tap is reported |
| `g_peak` | `150` | Minimum peak speed, in degrees/s, for a movement to count as a gesture |
| `g_angle` | `15` | Minimum flick amplitude, in degrees |
| `flip_deg` / `flip_ms` | `90` / `400` | Minimum amplitude and maximum duration of a quick rotation |
| `shake_n` | `3` | Minimum alternating strokes of a shake |

<a id="en-structure"></a>
### Project structure and experimental programs

| Path | Contents |
| --- | --- |
| [firmware/platformio.ini](firmware/platformio.ini) | Board, build environment and dependencies |
| [firmware/src/main.cpp](firmware/src/main.cpp) | Active firmware: BLE mouse, MPU6050 and two contacts |
| [tools/eous_tune.py](tools/eous_tune.py) | Calibration, tuning and gestures from the PC over USB serial |
| [tools/eous_gui.py](tools/eous_gui.py), [tools/eous_gui.html](tools/eous_gui.html) | Graphical interface in the browser: virtual pointer and adjustments |
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
| Pointer moves while the sensor is still | Put the sensor down and leave it still for two seconds: the offset corrects itself. Raise `deadzone` if needed. |
| Clicks are not detected | Check that each contact closes the correct GPIO to GND and that the mouse is connected. |
| Mouse is not visible or pairing fails | Check power and serial messages; if an old pairing exists, remove it from the computer and pair again. |
| No serial messages | Check the USB data cable, selected port and 115200 baud rate; restart with the monitor open to read startup messages. |

<a id="en-license"></a>
### License

The project is distributed under the [MIT license](LICENSE).

[Back to language selection](#eous) · [Go to the Italian version](#italiano)
