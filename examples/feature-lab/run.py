#!/usr/bin/env python3
"""Interactive local QtALH lab; requires Python 3 and built QtALH/EPICS Base."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.request import Request, urlopen

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CHANNELS = ("pressure", "temperature", "chatter", "maintenance")
HELP = """
Commands in this terminal (acknowledge alarms in the QtALH window):
  minor                 Pressure -> MINOR
  major                 Pressure -> MAJOR
  normal                Pressure -> normal (latched acknowledgement may remain)
  standing              Temperature -> MAJOR until you clear it
  maintenance           Maintenance -> MAJOR for shelving/notification tests
  chatter               Six alarm/recovery cycles, about 12 seconds
  clear                 All four values -> normal
  set CHANNEL VALUE     Example: set maintenance 0
  status                Display values, severities, and IOC acknowledgement fields
  gap                   Stop IOC for 3 seconds, then restart with normal values
  restart               Restart QtALH, keeping IOC and saved lab settings
  help                  Show these commands
  quit                  Stop this lab's QtALH, IOC, receiver, and CA repeater
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--style", choices=("motif", "fusion"), default="fusion")
    parser.add_argument("--silent", action="store_true", help="Start QtALH with alarm sound silenced")
    parser.add_argument("--mode", choices=("local", "global", "passive"), default="local")
    parser.add_argument("--port", type=int, default=15068, help="CA port; repeater uses port + 1")
    parser.add_argument("--webhook-port", type=int, default=18080)
    parser.add_argument("--epics-bin", type=Path,
                        default=ROOT.parent / "epics-base/bin/linux-x86_64")
    parser.add_argument("--smoke-test", action="store_true", help="Check lab without opening a GUI")
    args = parser.parse_args()
    app = ROOT / "bin/Linux-x86_64/qtalh"
    for program in (app, *(args.epics_bin / p for p in ("softIoc", "caput", "caget", "caRepeater"))):
        if not os.access(program, os.X_OK):
            parser.error(f"Missing executable: {program}")
    if not 1024 <= args.port < 65535 or not 1024 <= args.webhook_port <= 65535:
        parser.error("Choose ports from 1024 to 65535, leaving room for CA port + 1")
    # Fail early if another lab/service is using the chosen ports.
    for port, kind in ((args.port, socket.SOCK_STREAM), (args.port, socket.SOCK_DGRAM),
                       (args.port + 1, socket.SOCK_DGRAM)):
        with socket.socket(socket.AF_INET, kind) as probe:
            if kind == socket.SOCK_STREAM:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(("127.0.0.1", port))
    state = HERE / ".state"
    state.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(EPICS_CA_NAME_SERVERS="", EPICS_CA_AUTO_ADDR_LIST="NO", EPICS_CA_ADDR_LIST=f"127.0.0.1:{args.port}",
               EPICS_CA_SERVER_PORT=str(args.port), EPICS_CA_REPEATER_PORT=str(args.port + 1),
               EPICS_CAS_INTF_ADDR_LIST="127.0.0.1", EPICS_CAS_AUTO_BEACON_ADDR_LIST="NO",
               EPICS_CAS_BEACON_ADDR_LIST=f"127.0.0.1:{args.port + 1}",
               XDG_CONFIG_HOME=str(state / "config"))
    url = f"http://127.0.0.1:{args.webhook_port}/notifications"

    class Receiver(BaseHTTPRequestHandler):
        def do_POST(self):
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= 1024 * 1024:
                    raise ValueError("Invalid payload size")
                message = json.loads(self.rfile.read(size))
                if not isinstance(message, dict):
                    raise ValueError("Expected a JSON object")
                with (state / "webhooks.jsonl").open("a", encoding="utf-8") as log:
                    log.write(json.dumps(message, ensure_ascii=False) + "\n")
                print(f"\nNotification received: {message.get('kind', 'unknown')} "
                      f"({len(message.get('alarms', []))} alarms)", flush=True)
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"received\n")
            except (ValueError, UnicodeError):
                self.send_error(400, "Expected a JSON notification")

        def log_message(self, *unused):
            pass

    receiver = HTTPServer(("127.0.0.1", args.webhook_port), Receiver)
    receiver.timeout = 1
    worker = threading.Thread(target=receiver.serve_forever, daemon=True)
    worker.start()
    children = []
    logs = []

    def start(command, log_name, **kwargs):
        log = (state / log_name).open("a", encoding="utf-8")
        logs.append(log)
        child = subprocess.Popen([str(p) for p in command], env=env, cwd=state,
                                 stdout=log, stderr=subprocess.STDOUT, **kwargs)
        children.append(child)
        return child

    def stop(child):
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if child.stdin:
            child.stdin.close()

    def ca(tool, *values):
        result = subprocess.run([str(args.epics_bin / tool), "-w", "1", *map(str, values)],
                                env=env, capture_output=True, text=True, timeout=5)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip())
        return result.stdout.strip()

    def put(channel, value):
        if channel not in CHANNELS:
            raise ValueError("Channel must be " + ", ".join(CHANNELS))
        print(ca("caput", "qtalh_lab:" + channel, value), flush=True)

    def start_ioc():
        child = start([args.epics_bin / "softIoc", "-d", HERE / "alarms.db"], "ioc.log",
                      stdin=subprocess.PIPE)
        for attempt in range(10):
            if child.poll() is not None:
                raise RuntimeError(f"IOC exited; inspect {state / 'ioc.log'}")
            try:
                ca("caget", "qtalh_lab:pressure")
                return child
            except (RuntimeError, subprocess.TimeoutExpired):
                time.sleep(0.2)
        raise RuntimeError(f"IOC did not connect; inspect {state / 'ioc.log'}")

    def start_gui():
        flags = [] if args.mode == "local" else ["-global"]
        if args.mode == "passive":
            flags.append("-S")
        if args.silent:
            flags.append("-s")
        child = start([app, "-style", args.style, "-p", ROOT / "qtalh/tests/alarm.ogg",
                       "-mainwindow", "-noerrorpopup",
                       *flags, HERE / "alarms.alhConfig"], "qtalh.log")
        time.sleep(0.5)
        if child.poll() is not None:
            raise RuntimeError(f"QtALH exited; inspect {state / 'qtalh.log'}")
        return child

    try:
        start([args.epics_bin / "caRepeater", "-v"], "repeater.log")
        time.sleep(0.2)
        ioc = start_ioc()
        if args.smoke_test:
            subprocess.run([str(app), "--validate", str(HERE / "alarms.alhConfig")],
                           env=env, check=True, timeout=10)
            for channel in CHANNELS:
                initial = ca("caget", "-t", f"qtalh_lab:{channel}.SEVR")
                if initial != "NO_ALARM":
                    raise RuntimeError(f"{channel} did not start normal: {initial}")
                for value, severity in ((7, "MINOR"), (12, "MAJOR"), (0, "NO_ALARM")):
                    put(channel, value)
                    actual = ca("caget", "-t", f"qtalh_lab:{channel}.SEVR")
                    if actual != severity:
                        raise RuntimeError(f"Expected {severity}, got {actual}")
            request = Request(url, data=b'{"kind":"test","alarms":[]}',
                              headers={"Content-Type": "application/json"})
            with urlopen(request, timeout=5) as response:
                if response.status != 200:
                    raise RuntimeError("Webhook receiver failed")
            print("PASS: configuration, all four IOC alarms, and local webhook receiver")
            return
        gui = start_gui()
        print(f"\nQtALH feature lab ready ({args.mode}, {args.style}).\n"
              f"Webhook destination URL: {url}\n"
              f"Lab settings and captured messages: {state}\n{HELP}", flush=True)
        while True:
            try:
                words = input("lab> ").split()
                if not words:
                    continue
                command = words[0]
                if command in ("quit", "exit"):
                    break
                if command == "help":
                    print(HELP)
                elif command in ("minor", "major", "normal"):
                    put("pressure", {"minor": 7, "major": 12, "normal": 0}[command])
                elif command in ("standing", "maintenance"):
                    put("temperature" if command == "standing" else "maintenance", 12)
                elif command == "clear":
                    for channel in CHANNELS:
                        put(channel, 0)
                elif command == "set" and len(words) == 3:
                    put(words[1], float(words[2]))
                elif command == "chatter":
                    for cycle in range(6):
                        put("chatter", 0)
                        time.sleep(1)
                        put("chatter", 12)
                        time.sleep(1)
                    put("chatter", 0)
                elif command == "status":
                    for channel in CHANNELS:
                        pv = "qtalh_lab:" + channel
                        print(ca("caget", pv, pv + ".SEVR", pv + ".ACKS"))
                elif command == "gap":
                    stop(ioc)
                    time.sleep(3)
                    ioc = start_ioc()
                elif command == "restart":
                    stop(gui)
                    gui = start_gui()
                else:
                    print("Unknown command; enter help.")
            except (ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
                print(f"Lab command failed: {error}", flush=True)
    except (EOFError, KeyboardInterrupt):
        print("\nStopping lab.")
    finally:
        for child in reversed(children):
            stop(child)
        receiver.shutdown()
        receiver.server_close()
        worker.join(timeout=2)
        for log in logs:
            log.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Lab could not complete: {error}", file=sys.stderr)
        sys.exit(1)
