#!/usr/bin/env python3
"""Interfaccia grafica di Eous nel browser: cursore virtuale e regolazioni in tempo reale.

Uso:
    python eous_gui.py        apre http://127.0.0.1:8765 nel browser

Questo programma tiene aperta la porta seriale e fa da ponte: manda alla pagina
(eous_gui.html) i dati che arrivano dal dispositivo e al dispositivo i comandi
della pagina. Non può girare insieme a eous_tune.py o al monitor seriale.
"""
import argparse
import contextlib
import json
import math
import pathlib
import queue
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

from eous_tune import RESTING_DPS, Device, DeviceError, find_port, is_still, mean

PAGE = pathlib.Path(__file__).with_name("eous_gui.html")


class Hub:
    """Distribuisce gli eventi a tutte le pagine aperte (Server-Sent Events)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.clients = []
        self.remembered = {}  # ultimo evento di ogni tipo da dare a chi si collega dopo

    def subscribe(self):
        client = queue.Queue(maxsize=1000)
        with self.lock:
            for message in self.remembered.values():
                client.put_nowait(message)
            self.clients.append(client)
        return client

    def unsubscribe(self, client):
        with self.lock:
            self.clients.remove(client)

    def publish(self, event, data, remember=False):
        message = f"event: {event}\ndata: {json.dumps(data)}\n\n".encode()
        with self.lock:
            if remember:
                self.remembered[event] = message
            for client in self.clients:
                with contextlib.suppress(queue.Full):  # pagina bloccata: perde dati, non ferma gli altri
                    client.put_nowait(message)


def serial_worker(device, hub, commands, stop):
    """Unico thread che usa la seriale: inoltra i campioni ed esegue i comandi in coda."""

    def status(text, error=False):
        hub.publish("status", {"text": text, "error": error}, remember=True)

    def publish_params():
        device.refresh()
        hub.publish("params", {**device.info, **device.params}, remember=True)

    def forward(samples, seconds):
        """Inoltra i campioni alla pagina per `seconds` e li restituisce."""
        taken = []
        for sample in samples:
            hub.publish("sample", [round(sample.t, 4), *(round(g, 2) for g in sample.gyro), sample.dx, sample.dy])
            taken.append(sample)
            if sample.t - taken[0].t >= seconds or stop.is_set():
                return taken

    def run(command, samples):
        action, *arguments = command
        if action == "set":
            device.set(*arguments)
            publish_params()
        elif action == "save":
            device.command("save")
            status("Parametri salvati nel dispositivo.")
        elif action == "reset":
            device.command("reset")
            publish_params()
            status("Parametri riportati ai valori iniziali.")
        elif action == "bias":
            status("Appoggia il sensore su un piano e non toccarlo: misuro tra 2 secondi...")
            forward(samples, 2.0)
            status("Misuro l'offset: non toccare il sensore...")
            still = forward(samples, 3.0)
            if stop.is_set():
                return
            if not is_still(still, RESTING_DPS):
                status("Il sensore si è mosso durante la misura: offset non cambiato. Riprova.", error=True)
                return
            bias = mean([s.gyro for s in still])
            for axis, value in zip("xyz", bias):
                device.set(f"bias_{axis}", value)
            publish_params()
            status("Offset aggiornato: " + "  ".join(f"{v:+.2f}" for v in bias) + " °/s.")

    try:
        publish_params()
        status("Collegato.")
        with device.stream() as samples:
            while not stop.is_set():
                forward(samples, 0)
                while not commands.empty():
                    run(commands.get(), samples)
    except (DeviceError, serial.SerialException, OSError) as error:
        status(f"Collegamento con il dispositivo perso: {error}. Riavvia eous_gui.py.", error=True)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def from_our_page(self):
        """Rifiuta le richieste di altri siti aperti nel browser: anche loro possono raggiungere 127.0.0.1."""
        port = self.server.server_port
        allowed = {f"127.0.0.1:{port}", f"localhost:{port}"}
        origin = self.headers.get("Origin")
        return self.headers.get("Host") in allowed and (origin is None or origin.split("//")[-1] in allowed)

    def do_GET(self):
        if not self.from_our_page():
            self.send_error(403)
        elif self.path == "/":
            body = PAGE.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/events":
            self.send_events()
        else:
            self.send_error(404)

    def send_events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        client = self.server.hub.subscribe()
        try:
            while True:
                try:
                    message = client.get(timeout=5)
                except queue.Empty:
                    message = b": ping\n\n"  # tiene viva la connessione e scopre le pagine chiuse
                self.wfile.write(message)
                self.wfile.flush()
        except OSError:
            pass
        finally:
            self.server.hub.unsubscribe(client)

    def do_POST(self):
        if not self.from_our_page():
            self.send_error(403)
            return
        action = self.path.lstrip("/")
        if action == "set":
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
                name, value = body["name"], float(body["value"])
            except (ValueError, KeyError, TypeError):
                name, value = None, math.nan
            if name not in self.server.param_names or not math.isfinite(value):
                self.send_error(400)
                return
            self.server.commands.put(("set", name, value))
        elif action in ("save", "reset", "bias"):
            self.server.commands.put((action,))
        else:
            self.send_error(404)
            return
        self.send_response(204)
        self.end_headers()


def main():
    parser = argparse.ArgumentParser(description="Interfaccia grafica di Eous nel browser.")
    parser.add_argument("--port", help="porta seriale, es. /dev/ttyACM0 (default: l'unica scheda collegata)")
    parser.add_argument("--http", type=int, default=8765, help="porta della pagina web (default 8765)")
    parser.add_argument("--no-browser", action="store_true", help="non aprire il browser da solo")
    args = parser.parse_args()
    try:
        device = Device(args.port or find_port())
    except (DeviceError, serial.SerialException) as error:
        raise SystemExit(f"Errore: {error}")

    # Solo 127.0.0.1: la pagina non è raggiungibile dagli altri computer della rete.
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.http), Handler)
    except OSError as error:
        raise SystemExit(f"Errore: non posso usare la porta {args.http} ({error}). Prova --http 8766.")
    server.hub = Hub()
    server.commands = queue.Queue()
    server.param_names = set(device.params)
    stop = threading.Event()
    worker = threading.Thread(target=serial_worker, args=(device, server.hub, server.commands, stop))
    worker.start()

    url = f"http://127.0.0.1:{args.http}"
    print(f"Interfaccia aperta su {url} (Ctrl+C per chiudere)")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        stop.set()  # il worker esce dal blocco `with` e spegne il flusso dei dati
        worker.join(timeout=3)


if __name__ == "__main__":
    main()
