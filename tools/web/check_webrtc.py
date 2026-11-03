#!/usr/bin/env python3
"""End-to-end check of WebRtcTransport: browser client against a native server.

A developer tool, not a CI step. It builds nothing: build the native probe and
the wasm preset first, then run this. It starts the native echo server, serves
the browser probe, opens it in headless Chromium (Playwright) and reads the
probe's verdict.

    cmake --build --preset dev --target kfm_net_probe       # or release
    source ~/emsdk/emsdk_env.sh && cmake --workflow --preset wasm
    python3 -m venv ~/.venvs/kfm-web && ~/.venvs/kfm-web/bin/pip install playwright
    ~/.venvs/kfm-web/bin/playwright install chromium-headless-shell   # or pass --chromium
    ~/.venvs/kfm-web/bin/python tools/web/check_webrtc.py [--messages 200]

What happens:
  1. kfm_net_probe --serve PORT --bind 127.0.0.1 runs as a subprocess (--probe
     overrides the path; the default is build/release, then build/dev).
  2. build/wasm/apps/net-probe is served on 127.0.0.1 (serve.py's handler).
  3. Chromium opens kfm_net_probe.html?connect=ws://127.0.0.1:PORT&messages=N:
     the page connects over WebRTC (datachannel-wasm over RTCPeerConnection),
     measures an idle RTT with pings, sends N unreliable + N reliable messages
     of varied sizes up to each lane's maximum, verifies every echo and sets
     window.kfmNetProbeResult to "KFM_NET_PROBE PASS ..." or "... FAIL ...".

Checks (each PASS/FAIL):
  server      the native server started and printed its listening line
  page        the page loads (HTTP 200)
  probe       window.kfmNetProbeResult appears within --timeout and says PASS
  server-log  the server logged the browser's connect and, once the page closed
              its transport, the disconnect with matching echo counters
  console     no console errors and no uncaught page errors

Chromium needs no special flags. The browser's host candidate is an mDNS
name (<uuid>.local, Chromium's default without media permissions) that the
server cannot resolve; it does not have to: the browser checks the server's
host candidate (its interface address; libjuice offers no loopback candidate,
so on WSL2 that is the eth0 address, not 127.0.0.1) and the server learns the
browser's address as peer-reflexive from that first STUN check. --bind only
restricts the signaling socket. --chromium-arg adds flags for experiments.

Playwright's browser download must match the installed Playwright; pass
--chromium with an existing chrome-headless-shell (e.g. one in
~/.cache/ms-playwright) when it does not, like check_browser.py.
"""

import argparse
import functools
import http.server
import pathlib
import queue
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from serve import Handler  # noqa: E402  (tools/web/serve.py)

try:
    from playwright.sync_api import sync_playwright
except ImportError:  # pragma: no cover - depends on the developer's environment
    print("error: Playwright is not installed (pip install playwright)", file=sys.stderr)
    sys.exit(2)

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
PROBE_CANDIDATES = [
    REPO_ROOT / "build" / "release" / "apps" / "net-probe" / "kfm_net_probe",
    REPO_ROOT / "build" / "dev" / "apps" / "net-probe" / "kfm_net_probe",
]
WASM_DIR = REPO_ROOT / "build" / "wasm" / "apps" / "net-probe"
PAGE = "kfm_net_probe.html"
RESULT_PREFIX = "KFM_NET_PROBE"


class QuietHandler(Handler):
    """serve.py's handler without the per-request log lines."""

    def log_message(self, format, *args):  # noqa: A002 - the base class's name
        pass


class Report:
    def __init__(self):
        self.failed = False

    def check(self, name: str, ok: bool, detail: str) -> None:
        print(f"{'PASS' if ok else 'FAIL'}  {name:10} {detail}")
        self.failed |= not ok

    @staticmethod
    def info(name: str, detail: str) -> None:
        print(f"INFO  {name:10} {detail}")


class Server:
    """kfm_net_probe --serve as a subprocess; its stdout lines are collected."""

    def __init__(self, probe: pathlib.Path, port: int):
        self.lines: list[str] = []
        self._queue: "queue.Queue[str | None]" = queue.Queue()
        self.proc = subprocess.Popen(
            [str(probe), "--serve", str(port), "--bind", "127.0.0.1"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def wait_for(self, needle: str, timeout: float) -> str | None:
        """Returns the first line (old or new) containing needle, or None."""
        for line in self.lines:
            if needle in line:
                return line
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self._queue.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                break
            self.lines.append(line)
            if needle in line:
                return line
        return None

    def stop(self) -> int:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.wait_for("\0", 2.0)  # drain the rest of the output
        return self.proc.returncode


def default_probe() -> pathlib.Path:
    for candidate in PROBE_CANDIDATES:
        if candidate.is_file():
            return candidate
    return PROBE_CANDIDATES[0]


def field(summary: str, name: str) -> str:
    for token in summary.split():
        if token.startswith(name + "="):
            return token.split("=", 1)[1]
    return "?"


def run(args) -> int:
    report = Report()
    if not args.probe.is_file():
        print(f"error: {args.probe} not found; build kfm_net_probe natively first", file=sys.stderr)
        return 1
    if not (args.dir / PAGE).is_file():
        print(f"error: {args.dir}/{PAGE} not found; build the wasm preset first", file=sys.stderr)
        return 1

    server = Server(args.probe, args.port)
    listening = server.wait_for(f"{RESULT_PREFIX} serving", 10.0)
    report.check("server", listening is not None,
                 listening or f"no listening line from {args.probe}")
    if listening is None:
        server.stop()
        for line in server.lines:
            print(f"      [server] {line}")
        print("RESULT FAIL")
        return 1

    handler = functools.partial(QuietHandler, directory=str(args.dir))
    http_server = http.server.ThreadingHTTPServer(("127.0.0.1", args.http_port), handler)
    threading.Thread(target=http_server.serve_forever, daemon=True).start()
    url = (f"http://127.0.0.1:{http_server.server_address[1]}/{PAGE}"
           f"?connect=ws://127.0.0.1:{args.port}&messages={args.messages}")
    print(f"opening {url}")

    console_errors, console_lines, page_errors = [], [], []
    result = None
    disconnect = None
    started = time.monotonic()
    try:
        with sync_playwright() as pw:
            launch = {"headless": not args.headed, "args": list(args.chromium_arg)}
            if args.chromium:
                launch["executable_path"] = args.chromium
            browser = pw.chromium.launch(**launch)
            page = browser.new_page()

            def on_console(msg):
                line = f"[{msg.type}] {msg.text}"
                console_lines.append(line)
                if msg.type == "error":
                    console_errors.append(line)

            page.on("console", on_console)
            page.on("pageerror", lambda exc: page_errors.append(str(exc)))

            response = page.goto(url, wait_until="load")
            report.check("page", response is not None and response.ok,
                         f"HTTP {response.status if response else '-'}")
            try:
                page.wait_for_function("() => typeof window.kfmNetProbeResult === 'string'",
                                       timeout=args.timeout * 1000)
                result = page.evaluate("() => window.kfmNetProbeResult")
            except Exception:  # noqa: BLE001 - Playwright raises its own TimeoutError
                result = None
            browser_seconds = time.monotonic() - started
            report.check("probe", result is not None and result.startswith(f"{RESULT_PREFIX} PASS"),
                         result or f"no result within {args.timeout:.0f} s")
            report.info("browser", f"{browser.version}; page done in {browser_seconds:.1f} s")
            # The page closes its transport once done: the server logs the
            # disconnect with this peer's counters.
            disconnect = server.wait_for("disconnected", 10.0)
            browser.close()
    finally:
        http_server.shutdown()
        server.stop()

    connected = any("connected" in line and "disconnected" not in line for line in server.lines)
    counters_ok = disconnect is not None and f"echoed_reliable={args.messages}" in disconnect
    report.check("server-log", connected and counters_ok,
                 disconnect or "no connect/disconnect logged by the server")

    report.check("console", not console_errors and not page_errors,
                 f"{len(console_errors)} console errors, {len(page_errors)} page errors, "
                 f"{len(console_lines)} console lines")
    for line in console_errors + [f"[pageerror] {e}" for e in page_errors]:
        print(f"      {line}")
    if args.verbose or report.failed:
        for line in console_lines:
            print(f"      [page] {line}")
        for line in server.lines:
            print(f"      [server] {line}")
    if result is not None:
        report.info("rtt", f"idle {field(result, 'rtt_idle_ms(min/med/p95/max)')} ms, "
                           f"bulk unreliable {field(result, 'rtt_bulk_unreliable_ms')} ms, "
                           f"bulk reliable {field(result, 'rtt_bulk_reliable_ms')} ms "
                           "(min/median/p95/max)")
    print("RESULT", "FAIL" if report.failed else "PASS")
    return 1 if report.failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join(__doc__.splitlines()[1:]),
    )
    parser.add_argument("--probe", type=pathlib.Path, default=default_probe(),
                        help="native kfm_net_probe (default build/release, then build/dev)")
    parser.add_argument("--dir", type=pathlib.Path, default=WASM_DIR,
                        help="directory holding kfm_net_probe.html (default build/wasm/...)")
    parser.add_argument("--port", type=int, default=9000, help="signaling port of the server")
    parser.add_argument("--http-port", type=int, default=0, help="HTTP port (default ephemeral)")
    parser.add_argument("--messages", type=int, default=200, help="messages per lane")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="seconds to wait for the probe's result")
    parser.add_argument("--chromium", default=None, help="chrome or chrome-headless-shell binary")
    parser.add_argument("--chromium-arg", action="append", default=[],
                        help="extra Chromium flag (repeatable)")
    parser.add_argument("--headed", action="store_true", help="show the browser window")
    parser.add_argument("--verbose", action="store_true",
                        help="print every console and server line")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
