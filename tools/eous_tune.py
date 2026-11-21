#!/usr/bin/env python3
"""Calibrazione e regolazione di Eous dal PC, attraverso la porta seriale USB.

Uso:
    python eous_tune.py              apre la console interattiva
    python eous_tune.py drift        esegue un solo comando ed esce

Richiede pyserial (pip install pyserial). Chiudi il monitor seriale di PlatformIO
prima di avviarlo: la porta va usata da un solo programma alla volta.
"""
import argparse
import cmd
import collections
import contextlib
import csv
import math
import time

import serial
from serial.tools import list_ports

STILL_DPS = 6.0       # scostamento massimo dalla media perché la mano sia considerata ferma
MOVING_DPS = 15.0     # sopra questa velocità la mano si sta muovendo
QUIET_S = 1.0         # ferma per questo tempo = movimento finito
RESTING_DPS = 2.0     # scostamento massimo con il sensore appoggiato (comando bias)
TOLERANCE_DEG = 4.0   # errore accettato nel test di deriva: la mano non torna mai al grado esatto

DESCRIPTIONS = {
    "sens": "pixel per grado di rotazione",
    "deadzone": "°/s sotto cui il cursore resta fermo",
    "acc_lo": "accelerazione: velocità della mano (°/s) da cui inizia",
    "acc_hi": "accelerazione: velocità della mano (°/s) da cui è massima",
    "acc_max": "accelerazione: moltiplicatore massimo (1 = spenta)",
    "filt_hz": "filtro: frequenza di taglio a mano ferma, Hz (0 = spento)",
    "filt_beta": "filtro: Hz di taglio in più per ogni °/s di velocità",
    "clutch": "frizione su GPIO17: 0 = no, 1 = premuta per muovere, 2 = premuta per bloccare",
    "click_ms": "ms di cursore fermo dopo ogni pressione o rilascio di un pulsante",
    "autobias": "1 = l'offset si corregge da solo quando il sensore è fermo",
}

# Un campione per ogni report del mouse. t: secondi dal primo campione; acc: g;
# gyro: °/s medi dall'ultimo report (offset incluso); dx, dy: pixel inviati; flags: stato.
Sample = collections.namedtuple("Sample", "t acc gyro dx dy flags")
SATURATED, CLUTCHED, CLICKING = 1, 2, 4  # bit di Sample.flags


class DeviceError(Exception):
    pass


def add(a, b):
    return [x + y for x, y in zip(a, b)]


def sub(a, b):
    return [x - y for x, y in zip(a, b)]


def scale(a, k):
    return [x * k for x in a]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def norm(a):
    return math.sqrt(dot(a, a))


def mean(vectors):
    return scale([sum(column) for column in zip(*vectors)], 1 / len(vectors))


def fmt(vector, digits=2):
    return "  ".join(f"{x:+.{digits}f}" for x in vector)


def quaternion_multiply(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return [
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    ]


def integrate(samples, bias):
    """Trasforma le velocità angolari in angoli, in due modi.

    - somma asse per asse: è il conto che fa il firmware (angolo += velocità * tempo);
    - orientamento 3D: compone le piccole rotazioni nell'ordine in cui avvengono
      (quaternione) e dice di quanti gradi la mano è davvero lontana dalla partenza.
    """
    per_axis = [0.0, 0.0, 0.0]
    orientation = [1.0, 0.0, 0.0, 0.0]
    for previous, current in zip(samples, samples[1:]):
        dt = current.t - previous.t
        rate = sub(scale(add(previous.gyro, current.gyro), 0.5), bias)
        per_axis = add(per_axis, scale(rate, dt))
        speed = norm(rate)
        if speed > 0:
            half = math.radians(speed * dt) / 2
            step = [math.cos(half)] + scale(rate, math.sin(half) / speed)
            orientation = quaternion_multiply(orientation, step)
    distance = math.degrees(2 * math.acos(min(1.0, abs(orientation[0]))))
    return per_axis, distance


def is_still(samples, limit):
    center = mean([s.gyro for s in samples])
    return all(norm(sub(s.gyro, center)) <= limit for s in samples)


def wait_still(samples, seconds):
    """Legge finché la mano resta ferma per `seconds` e restituisce quei campioni."""
    run = []
    first = None
    for sample in samples:
        first = sample.t if first is None else first
        if sample.t - first > 20:
            raise DeviceError("la mano non resta ferma abbastanza a lungo")
        run.append(sample)
        while not is_still(run, STILL_DPS):
            run.pop(0)
        if run[-1].t - run[0].t >= seconds:
            return run


def capture_movement(samples, still_seconds, max_seconds=60):
    """Aspetta la mano ferma, poi registra un movimento finché la mano si ferma di nuovo.

    Restituisce (campioni da fermo, campioni del movimento). L'offset del
    giroscopio va calcolato sui primi: è quello valido in questo momento.
    """
    still = wait_still(samples, still_seconds)
    bias = mean([s.gyro for s in still])
    print("  VIA!")
    recorded = [still[-1]]
    last_active = None
    for sample in samples:
        recorded.append(sample)
        if norm(sub(sample.gyro, bias)) > MOVING_DPS:
            last_active = sample.t
        if last_active is not None and sample.t - last_active >= QUIET_S:
            return still, recorded
        if sample.t - recorded[0].t > max_seconds:
            raise DeviceError("tempo scaduto: nessun movimento concluso")


def accel_gain(params, speed):
    """Stessa curva del firmware (accelGain in main.cpp)."""
    lo, hi = params["acc_lo"], params["acc_hi"]
    if hi <= lo:
        return 1.0 if speed < lo else params["acc_max"]
    t = min(1.0, max(0.0, (speed - lo) / (hi - lo)))
    return 1.0 + (params["acc_max"] - 1.0) * t


class Device:
    def __init__(self, port):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.1
        # Aprendo la porta Linux attiva le linee DTR e RTS, e sulle schede ESP32 la
        # combinazione "DTR spenta, RTS attiva" riavvia il chip. pyserial spegnerebbe
        # prima DTR: con dsrdtr=True non la tocca, così spengo prima RTS e poi DTR.
        self.ser.dsrdtr = True
        self.ser.rts = False
        self.ser.open()
        with contextlib.suppress(OSError):  # porte senza queste linee
            self.ser.dtr = False
        self.buffer = b""
        self.backlog = []  # righe di dati arrivate mentre si aspettava la risposta a un comando
        self.bias_updates = 0  # quante volte il firmware ha comunicato un nuovo offset
        self.info = {}
        self.params = {}
        self._connect()

    def _line(self, deadline):
        """Prossima riga ricevuta, oppure None se si arriva a `deadline` senza una riga completa."""
        while b"\n" not in self.buffer:
            if time.monotonic() > deadline:
                return None
            self.buffer += self.ser.read(self.ser.in_waiting or 1)
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode("ascii", "replace").strip()

    def command(self, text, timeout=2.0):
        """Invia un comando e restituisce le righe di risposta (senza la riga OK finale)."""
        self.ser.write((text + "\n").encode("ascii"))
        deadline = time.monotonic() + timeout
        reply = []
        while (line := self._line(deadline)) is not None:
            if line == "OK":
                return reply
            if line.startswith("ERR"):
                raise DeviceError(f"{text}: {line[4:]}")
            if line[:2] in ("I,", "P,"):
                reply.append(line)
            elif line[:2] in ("D,", "B,"):
                self.backlog.append(line)
        raise DeviceError(f"nessuna risposta al comando '{text}'")

    def refresh(self):
        info, params = {}, {}
        try:
            for line in self.command("get", timeout=1.0):
                kind, name, value = line.split(",")
                (info if kind == "I" else params)[name] = float(value)
        except ValueError:
            raise DeviceError("risposta non valida") from None
        if "sens" not in params or "lsb_per_dps" not in info:
            raise DeviceError("risposta incompleta")
        self.info, self.params = info, params

    def _connect(self):
        # Se la scheda si è appena accesa o riavviata, risponde solo dopo la calibrazione iniziale.
        deadline = time.monotonic() + 12
        waiting = False
        while True:
            try:
                self.refresh()
                break
            except DeviceError:
                if time.monotonic() > deadline:
                    raise DeviceError(
                        "la scheda non risponde. Hai caricato il firmware aggiornato? "
                        "Il monitor seriale è chiuso?") from None
                if not waiting:
                    print("In attesa della scheda (se si è riavviata, tieni fermo il sensore)...")
                    waiting = True
        if waiting:
            # Scarta le risposte ai tentativi andati a vuoto e rilegge i parametri.
            time.sleep(0.3)
            self.ser.reset_input_buffer()
            self.buffer = b""
            self.refresh()
        self.command("stream 0")  # nel caso un programma chiuso male abbia lasciato il flusso acceso

    def set(self, name, value):
        self.command(f"set {name} {value:.6g}")

    @contextlib.contextmanager
    def stream(self):
        """Attiva il flusso dei dati grezzi e lo spegne all'uscita dal blocco `with`."""
        self.ser.reset_input_buffer()
        self.buffer = b""
        self.backlog = []
        self.ser.write(b"stream 1\n")
        try:
            yield self._samples()
        finally:
            self.command("stream 0")

    def _samples(self):
        lsb_dps, lsb_g = self.info["lsb_per_dps"], self.info["lsb_per_g"]
        t = 0.0
        last_us = None
        while True:
            line = self.backlog.pop(0) if self.backlog else self._line(time.monotonic() + 2.0)
            if line is None:
                raise DeviceError("il sensore non manda dati: controlla i messaggi [MPU] all'avvio")
            parts = line.split(",")
            if parts[0] == "B" and len(parts) == 4:
                # Il firmware ha corretto da solo l'offset del giroscopio.
                with contextlib.suppress(ValueError):
                    self.params.update(zip(("bias_x", "bias_y", "bias_z"), map(float, parts[1:])))
                    self.bias_updates += 1
                continue
            if parts[0] != "D" or len(parts) != 11:
                continue
            try:
                us, ax, ay, az, gx, gy, gz, dx, dy, flags = (int(p) for p in parts[1:])
            except ValueError:
                continue
            if last_us is not None:
                t += ((us - last_us) & 0xFFFFFFFF) / 1e6  # il contatore riparte da zero dopo 71 minuti
            last_us = us
            yield Sample(t, scale([ax, ay, az], 1 / lsb_g), scale([gx, gy, gz], 1 / lsb_dps),
                         dx, dy, flags)

    def record(self, seconds):
        with self.stream() as samples:
            recorded = []
            for sample in samples:
                recorded.append(sample)
                if sample.t >= seconds:
                    return recorded


class Shell(cmd.Cmd):
    intro = "Eous: calibrazione e regolazione. Scrivi help per i comandi, esci per uscire."
    prompt = "eous> "
    doc_header = "Comandi (help <comando> per i dettagli):"

    def __init__(self, device):
        super().__init__()
        self.dev = device

    def onecmd(self, line):
        try:
            return super().onecmd(line)
        except (DeviceError, serial.SerialException) as error:
            print(f"Errore: {error}")
        except KeyboardInterrupt:
            print("\nInterrotto.")

    def emptyline(self):
        pass

    def default(self, line):
        if line == "EOF":
            print()
            return True
        print(f"Comando sconosciuto: {line}. Scrivi help.")

    def do_esci(self, arg):
        """Esce dal programma."""
        return True

    def rows(self):
        p = self.dev.params
        return [p[f"x_g{a}"] for a in "xyz"], [p[f"y_g{a}"] for a in "xyz"]

    def device_bias(self):
        return [self.dev.params[f"bias_{a}"] for a in "xyz"]

    # --- parametri ---

    def do_mostra(self, arg):
        """Mostra i parametri attuali del dispositivo."""
        self.dev.refresh()
        for name, description in DESCRIPTIONS.items():
            print(f"  {name:9} {self.dev.params[name]:8.3g}   {description}")
        for label, row in zip("XY", self.rows()):
            terms = " ".join(f"{k:+.3f}·g{a}" for k, a in zip(row, "xyz"))
            print(f"  cursore {label} = {terms}")
        print(f"  offset giroscopio: {fmt(self.device_bias())} °/s")

    def do_set(self, arg):
        """set NOME VALORE [NOME VALORE ...]: cambia uno o più parametri, subito attivi.
        Esempio: set acc_max 3 acc_lo 40. Usa `salva` per conservarli dopo lo spegnimento."""
        words = arg.split()
        try:
            pairs = [(name, float(value)) for name, value in zip(words[::2], words[1::2])]
        except ValueError:
            pairs = []
        if not pairs or len(words) % 2:
            print("Uso: set NOME VALORE [NOME VALORE ...]")
            return
        for name, value in pairs:
            self.dev.set(name, value)
        self.do_mostra("")

    def do_salva(self, arg):
        """Scrive i parametri nella memoria del dispositivo: restano dopo lo spegnimento."""
        self.dev.command("save")
        print("Parametri salvati.")

    def do_reset(self, arg):
        """Riporta i parametri ai valori di fabbrica e cancella quelli salvati."""
        self.dev.command("reset")
        self.do_mostra("")

    def do_curva(self, arg):
        """Mostra la curva di accelerazione: a ogni velocità della mano, quanto corre il cursore."""
        self.dev.refresh()
        p = self.dev.params
        limit = 127 / (self.dev.info["send_ms"] / 1000)  # un report porta al massimo 127 pixel
        print("  mano °/s   guadagno   cursore px/s")
        for speed in (0.5, 2, 5, 10, 25, 50, 100, 200, 400, 800):
            gain = accel_gain(p, speed)
            pixels = 0 if speed < p["deadzone"] else speed * p["sens"] * gain
            note = "zona morta" if pixels == 0 else "limite del report" if pixels > limit else ""
            print(f"  {speed:8g}   {gain:8.2f}   {min(pixels, limit):12.0f}   {note}")

    # --- osservare i dati ---

    def do_live(self, arg):
        """Mostra i dati del sensore in tempo reale. Ctrl+C per fermare."""
        bias = self.device_bias()
        with self.dev.stream() as samples:
            for index, sample in enumerate(samples):
                if index % 5 == 0:
                    print(f"\r  giroscopio {fmt(sub(sample.gyro, bias), 1):>26} °/s"
                          f" | accelerometro {fmt(sample.acc)} g"
                          f" | cursore {sample.dx:+4d} {sample.dy:+4d} ", end="", flush=True)

    def do_registra(self, arg):
        """registra FILE [SECONDI]: salva i dati grezzi in un file CSV (10 secondi se non indicato)."""
        words = arg.split()
        try:
            path, seconds = words[0], float(words[1]) if len(words) > 1 else 10.0
        except (IndexError, ValueError):
            print("Uso: registra FILE [SECONDI]")
            return
        print(f"Registro per {seconds:g} secondi...")
        recorded = self.dev.record(seconds)
        with open(path, "w", newline="") as file:
            writer = csv.writer(file)
            writer.writerow(["t_s", "ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps", "dx", "dy", "flags"])
            for s in recorded:
                writer.writerow([f"{s.t:.4f}", *(f"{v:.4f}" for v in s.acc),
                                 *(f"{v:.3f}" for v in s.gyro), s.dx, s.dy, s.flags])
        print(f"{len(recorded)} campioni salvati in {path}")

    # --- calibrazioni ---

    def do_bias(self, arg):
        """Misura offset e rumore del giroscopio da fermo e aggiorna l'offset del dispositivo."""
        print("Appoggia il sensore su un piano e non toccarlo: misuro per 5 secondi...")
        recorded = self.dev.record(5.0)
        if not is_still(recorded, RESTING_DPS):
            print("Il sensore si è mosso durante la misura: ripeti.")
            return
        rates = [s.gyro for s in recorded]
        bias = mean(rates)
        noise = [math.sqrt(sum((r[a] - bias[a]) ** 2 for r in rates) / len(rates)) for a in range(3)]
        self.dev.refresh()
        change = norm(sub(bias, self.device_bias()))
        print("                X      Y      Z")
        print(f"  offset    {fmt(bias)}  °/s")
        print(f"  rumore    {'  '.join(f'{n:5.2f}' for n in noise)}  °/s (deviazione standard)")
        print(f"  L'offset usato finora dal dispositivo differiva di {change:.2f} °/s.")
        for axis, value in zip("xyz", bias):
            self.dev.set(f"bias_{axis}", value)
        print("  Offset aggiornato.")
        print(f"  Zona morta: il rumore del sensore permetterebbe {max(0.2, 6 * max(noise)):.1f} °/s; "
              f"ora è {self.dev.params['deadzone']:g}. Di solito comanda il tremolio della mano: prova.")

    def do_assi(self, arg):
        """Calibra gli assi: impara dai tuoi movimenti quale rotazione significa destra e quale su.
        Serve se il sensore non è montato dritto o se un verso è invertito."""
        axes = []
        for direction in ("verso DESTRA", "verso l'ALTO"):
            input("Porta la mano al centro, in posizione comoda, e premi Invio... ")
            print(f"  Tienila ferma; al VIA ruota il polso {direction} di circa 45° e resta lì.")
            with self.dev.stream() as samples:
                still, recorded = capture_movement(samples, 1.0)
            rotation, _ = integrate(recorded, mean([s.gyro for s in still]))
            angle = norm(rotation)
            if angle < 15:
                print(f"  Movimento troppo piccolo ({angle:.0f}°): ripeti il comando.")
                return
            axes.append(scale(rotation, 1 / angle))
            print(f"  Rotazione di {angle:.0f}° attorno all'asse (X Y Z) = {fmt(axes[-1])}")
        right, up = axes
        overlap = dot(right, up)
        if abs(overlap) > 0.5:
            print("I due movimenti sono troppo simili tra loro: ripeti il comando.")
            return
        # Cerco le due righe che danno (1, 0) per "destra" e (0, 1) per "su", anche
        # se i due assi misurati non sono esattamente perpendicolari.
        k = 1 / (1 - overlap * overlap)
        new_x = scale(sub(right, scale(up, overlap)), k)
        new_y = scale(sub(up, scale(right, overlap)), -k)  # sullo schermo Y positivo = giù
        self.dev.refresh()
        old_x, old_y = self.rows()
        print(f"Angolo tra i due assi: {math.degrees(math.acos(overlap)):.0f}° (ideale 90°).")
        print("Direzione del cursore (X, Y) per i tuoi due movimenti; ideale: destra +1 0, su 0 -1")
        for label, axis in (("destra", right), ("su", up)):
            print(f"  {label:7} prima {dot(old_x, axis):+.2f} {dot(old_y, axis):+.2f}"
                  f"   dopo {dot(new_x, axis):+.2f} {dot(new_y, axis):+.2f}")
        if input("Applico la nuova mappatura? [S/n] ").strip().lower() in ("", "s", "si", "sì"):
            for row, values in (("x", new_x), ("y", new_y)):
                for axis, value in zip("xyz", values):
                    self.dev.set(f"{row}_g{axis}", value)
            print("Applicata. Provala e usa `salva` per conservarla.")

    def do_drift(self, arg):
        """Test di deriva: fai un movimento e torna al punto di partenza; il programma dice
        se sensore e firmware tornano a zero e, se no, da dove nasce l'errore."""
        print("Appoggia la mano in una posizione che puoi ritrovare uguale (per esempio sul")
        print("tavolo, contro un libro). Al VIA fai il movimento da provare, senza pause più")
        print("lunghe di un secondo, poi torna nella STESSA posizione e resta fermo.")
        self.dev.refresh()
        with self.dev.stream() as samples:
            still, recorded = capture_movement(samples, 1.5)
        bias = mean([s.gyro for s in still])
        per_axis, distance = integrate(recorded, bias)
        end = [s for s in recorded if s.t >= recorded[-1].t - QUIET_S / 2]
        up_start, up_end = mean([s.acc for s in still]), mean([s.acc for s in end])
        cosine = dot(up_start, up_end) / (norm(up_start) * norm(up_end))
        tilt = math.degrees(math.acos(max(-1.0, min(1.0, cosine))))

        p = self.dev.params
        x_row, y_row = self.rows()
        duration = recorded[-1].t - recorded[0].t
        period = self.dev.info["send_ms"] / 1000
        lost = sum(1 for a, b in zip(recorded, recorded[1:]) if b.t - a.t > 1.5 * period)
        saturated = sum(1 for s in recorded if s.flags & SATURATED)
        frozen = sum(1 for s in recorded if s.flags & (CLUTCHED | CLICKING))
        peak = max(norm(sub(s.gyro, bias)) for s in recorded)
        expected = [dot(x_row, per_axis) * p["sens"], dot(y_row, per_axis) * p["sens"]]
        sent = [sum(s.dx for s in recorded), sum(s.dy for s in recorded)]
        bias_error = sub(bias, self.device_bias())  # quello che il firmware non toglie

        print(f"\n  Durata {duration:.1f} s, {len(recorded)} campioni ({lost} persi), "
              f"velocità massima {peak:.0f} °/s, campioni saturati: {saturated}")
        print(f"  Somma asse per asse   X {per_axis[0]:+.1f}°  Y {per_axis[1]:+.1f}°  Z {per_axis[2]:+.1f}°"
              f"  -> cursore {round(expected[0]):+d} {round(expected[1]):+d} px")
        print(f"  Cursore inviato dal firmware: {sent[0]:+d} {sent[1]:+d} px (positivi = destra, giù)")
        print(f"  Orientamento 3D: la mano risulta a {distance:.1f}° dalla posizione di partenza")
        print(f"  Inclinazione (accelerometro): {tilt:.1f}° di differenza tra inizio e fine")
        print(f"  Offset: dispositivo {fmt(self.device_bias())} | misurato ora {fmt(bias)} °/s\n")

        if tilt > TOLERANCE_DEG:
            print("Esito: la mano non è tornata nella stessa posizione, il test non vale. Ripeti.")
            return
        tolerance_px = TOLERANCE_DEG * p["sens"]
        findings = []
        if saturated:
            findings.append("Il giroscopio ha saturato: i movimenti più rapidi sono stati sottostimati.")
        if norm(bias_error) > 0.3:
            rate = [dot(x_row, bias_error) * p["sens"], dot(y_row, bias_error) * p["sens"]]
            findings.append(
                f"L'offset del dispositivo è sbagliato di {norm(bias_error):.2f} °/s: mentre ti muovi il "
                f"cursore deriva di circa {round(rate[0]):+d} {round(rate[1]):+d} px al secondo (destra, giù). "
                "Esegui `bias`.")
        if distance > TOLERANCE_DEG:
            findings.append(
                f"Per il sensore la mano non è tornata al punto di partenza ({distance:.1f}° in "
                f"{duration:.0f} s). O l'hai appoggiata ruotata rispetto a prima, o l'offset non è stabile.")
        elif norm(expected) > tolerance_px:
            findings.append(
                "La mano è tornata al punto di partenza, ma la somma asse per asse no. È un effetto "
                "geometrico: le rotazioni nello spazio non si sommano asse per asse, e l'errore cresce "
                "quando il polso ruota anche su se stesso. Nessuna calibrazione lo toglie: serve "
                "correggere gli assi con l'accelerometro nel firmware.")
        if frozen:
            findings.append(
                f"Per {frozen * period:.1f} s il cursore è rimasto fermo per la frizione o per il blocco "
                "al click: il movimento fatto in quel tempo non è stato inviato.")
        elif norm(sub(sent, expected)) > tolerance_px:
            findings.append(
                "Il firmware ha inviato un movimento diverso dalla somma degli angoli: è l'effetto di "
                "zona morta, accelerazione (acc_max) o limite del report. Con l'accelerazione attiva è "
                "normale: andata veloce e ritorno lento non si compensano.")
        if not findings:
            findings.append(
                f"Sensore e firmware tornano al punto di partenza (entro {tolerance_px:.0f} px). Se sullo "
                "schermo il cursore non torna, la causa è dopo il firmware: di solito l'accelerazione "
                "del puntatore del sistema operativo.")
        print("Esito:")
        for finding in findings:
            print(f"- {finding}")


def find_port():
    ports = [p.device for p in list_ports.comports() if p.vid is not None]
    if len(ports) == 1:
        return ports[0]
    if not ports:
        raise SystemExit("Nessuna scheda USB trovata: collega l'ESP32 o indica la porta con --port.")
    raise SystemExit(f"Più porte trovate ({', '.join(ports)}): scegline una con --port.")


def main():
    parser = argparse.ArgumentParser(description="Calibrazione e regolazione di Eous via seriale USB.")
    parser.add_argument("--port", help="porta seriale, es. /dev/ttyACM0 (default: l'unica scheda collegata)")
    parser.add_argument("comando", nargs="*", help="comando da eseguire subito; senza, apre la console")
    args = parser.parse_args()
    try:
        device = Device(args.port or find_port())
    except (DeviceError, serial.SerialException) as error:
        raise SystemExit(f"Errore: {error}")
    shell = Shell(device)
    if args.comando:
        shell.onecmd(" ".join(args.comando))
        return
    with contextlib.suppress(KeyboardInterrupt):
        shell.cmdloop()


if __name__ == "__main__":
    main()
