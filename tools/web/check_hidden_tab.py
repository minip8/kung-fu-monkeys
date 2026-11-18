#!/usr/bin/env python3
"""Does the browser client survive a hidden tab? (architecture §6.5, §6.6, v0.92)

A developer tool, not a CI step. It builds nothing: build kfm_server natively
and the wasm preset first, then run this. It starts kfm_server, serves the
browser client, opens it in Chromium (Playwright) on ?connect= so it plays a
match, then hides the page for each --away duration and checks the session
afterwards.

    cmake --preset server-headless && cmake --build --preset server-headless --target kfm_server
    source ~/emsdk/emsdk_env.sh && cmake --preset wasm && cmake --build --preset wasm --target kfm_client
    ~/.venvs/kfm-web/bin/python tools/web/check_hidden_tab.py --away 30 120 600 [--hide emulate|tab|freeze]

How the page is hidden (--hide):
  emulate   (default) what a browser does to a hidden page, done by the page itself:
            requestAnimationFrame callbacks are held (none runs while away) and
            document.visibilityState reads "hidden" (with a visibilitychange
            event). Headless Chromium never hides a page (every page stays
            "visible" with 60 animation frames a second whatever is in front,
            measured with Playwright 1.64 and Chromium 156, new and old headless,
            minimised or behind another tab, with or without focus emulation),
            so this is the faithful part a test can reach. What it does not
            show is the browser's timer throttling of a hidden page, which the
            background pump avoids by running its timer in a Worker; the netcode
            tests cover a pump that runs only once a second.
  tab       a second tab in front of it (Page.bringToFront): what a player does.
            It hides nothing in headless Chromium, nor in a headed one under
            WSLg as Playwright starts it (still "visible", 60 frames a second);
            the hidden check then fails, which says so rather than pretend.
  freeze    the page's main thread is stopped (Debugger.pause; headless Chromium
            ignores Page.setWebLifecycleState "frozen" for a page it considers
            visible): no script of the page runs, as for a frozen or discarded
            tab, a suspended mobile browser or an OS sleep. Nothing can keep the
            link alive; past the server's 15 s timeout the client must reconnect
            on its own and land in its kept slot.
The script prints the page's visibilityState and how many animation frames ran
while it was away, so the hiding is shown, not assumed.

Checks per --away duration (PASS/FAIL):
  hidden      the page was hidden (emulate/tab: visibilityState "hidden" and no
              animation frame while away; freeze: the page was frozen)
  link        emulate/tab: the server never dropped the peer (no "leave peer=N" line)
              and the client printed "in the background"; freeze: the server logged the
              timeout and the client its loss, then "reconnect peer=" and "Online" again
  resync      on return the client asked for one snapshot (the server's "resync
              peer=" line) and is "Online" again
  in-sync     for --settle seconds after the return: no further resync (a checksum
              mismatch would ask for one), no leave, no "Connection lost"
  console     no console errors and no uncaught page errors

Software GL as in check_browser.py (SwiftShader). Exit status 0 when every check
passed. --chromium points at a specific chrome or chrome-headless-shell binary.
"""

import argparse
import functools
import http.server
import pathlib
import queue
import re
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
SERVER_CANDIDATES = [
    REPO_ROOT / "build" / "server-headless" / "apps" / "server" / "kfm_server",
    REPO_ROOT / "build" / "release" / "apps" / "server" / "kfm_server",
    REPO_ROOT / "build" / "dev" / "apps" / "server" / "kfm_server",
]
WASM_DIR = REPO_ROOT / "build" / "wasm" / "apps" / "client"
PAGE = "kfm_client.html"
CHROMIUM_ARGS = [
    "--use-angle=swiftshader",
    "--enable-unsafe-swiftshader",
    "--ignore-gpu-blocklist",
    "--enable-webgl",
]
# Counts animation frames independently of the client's own loop, and lets the
# script hide the page the way a browser does (--hide emulate).
PAGE_SCRIPT = """
(() => {
  const realRaf = window.requestAnimationFrame.bind(window);
  const held = [];
  let hidden = false;
  window.requestAnimationFrame = (cb) => {
    if (hidden) { held.push(cb); return 0; }
    return realRaf(cb);
  };
  Object.defineProperty(Document.prototype, 'visibilityState',
                        { configurable: true, get() { return hidden ? 'hidden' : 'visible'; } });
  Object.defineProperty(Document.prototype, 'hidden',
                        { configurable: true, get() { return hidden; } });
  window.__kfmSetHidden = (h) => {
    hidden = h;
    document.dispatchEvent(new Event('visibilitychange'));
    if (!h) for (const cb of held.splice(0)) realRaf(cb);
  };
  window.__kfmRaf = 0;
  (function count() { window.__kfmRaf++; window.requestAnimationFrame(count); })();
})();
"""


class QuietHandler(Handler):
    """serve.py's handler without the per-request log lines."""

    def log_message(self, format, *args):  # noqa: A002 - the base class's name
        pass


class Report:
    def __init__(self):
        self.failed = False

    def check(self, name: str, ok: bool, detail: str) -> None:
        print(f"{'PASS' if ok else 'FAIL'}  {name:10} {detail}", flush=True)
        self.failed |= not ok

    @staticmethod
    def info(name: str, detail: str) -> None:
        print(f"INFO  {name:10} {detail}", flush=True)


class Lines:
    """A subprocess's or a page's lines, collected from another thread."""

    def __init__(self):
        self.lines: list[str] = []
        self._lock = threading.Lock()

    def add(self, line: str) -> None:
        with self._lock:
            self.lines.append(line)

    def since(self, mark: int) -> list[str]:
        with self._lock:
            return list(self.lines[mark:])

    def mark(self) -> int:
        with self._lock:
            return len(self.lines)


class Server:
    def __init__(self, binary: pathlib.Path, port: int, bots: int):
        self.out = Lines()
        self.proc = subprocess.Popen(
            [str(binary), "--port", str(port), "--bind", "127.0.0.1", "--bots", str(bots),
             "--auto-start", "--stats-every", "5"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self._done: "queue.Queue[None]" = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            self.out.add(line.rstrip("\n"))
        self._done.put(None)

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


# Playwright's sync API delivers page events (console lines) only while one
# of its calls runs, so every wait goes through the page.
PUMP = {"page": None}


def pause(seconds: float) -> None:
    if PUMP["page"] is not None:
        PUMP["page"].wait_for_timeout(seconds * 1000.0)
    else:
        time.sleep(seconds)


def wait_for(lines: Lines, mark: int, pattern: str, timeout: float) -> str | None:
    deadline = time.monotonic() + timeout
    rx = re.compile(pattern)
    while True:
        for line in lines.since(mark):
            if rx.search(line):
                return line
        if time.monotonic() >= deadline:
            return None
        pause(0.1)


def default_server() -> pathlib.Path:
    for candidate in SERVER_CANDIDATES:
        if candidate.is_file():
            return candidate
    return SERVER_CANDIDATES[0]


def run(args) -> int:
    report = Report()
    if not args.server.is_file():
        print(f"error: {args.server} not found; build kfm_server first", file=sys.stderr)
        return 1
    if not (args.dir / PAGE).is_file():
        print(f"error: {args.dir}/{PAGE} not found; build the wasm preset first", file=sys.stderr)
        return 1

    server = Server(args.server, args.port, args.bots)
    if wait_for(server.out, 0, r"listening|signaling|ws://", 10.0) is None:
        report.info("server", "no listening line yet; going on")
    handler = functools.partial(QuietHandler, directory=str(args.dir))
    http_server = http.server.ThreadingHTTPServer(("127.0.0.1", args.http_port), handler)
    threading.Thread(target=http_server.serve_forever, daemon=True).start()
    url = (f"http://127.0.0.1:{http_server.server_address[1]}/{PAGE}"
           f"?connect=ws://127.0.0.1:{args.port}&token=hidden-check&nick=Hidden&no-settings")
    print(f"opening {url}  (hide: {args.hide}, away: {args.away} s)", flush=True)

    console = Lines()
    page_errors: list[str] = []
    try:
        with sync_playwright() as pw:
            launch = {"headless": not args.headed, "args": CHROMIUM_ARGS + list(args.chromium_arg)}
            if args.chromium:
                launch["executable_path"] = args.chromium
            browser = pw.chromium.launch(**launch)
            context = browser.new_context(viewport={"width": 960, "height": 540})
            page = context.new_page()
            PUMP["page"] = page
            page.add_init_script(PAGE_SCRIPT)
            page.on("console", lambda m: console.add(f"[{m.type}] {m.text}"))
            page.on("pageerror", lambda exc: page_errors.append(str(exc)))
            cdp = context.new_cdp_session(page)
            page.goto(url, wait_until="load")
            online = wait_for(console, 0, r"kfm_client: Online on", 60.0)
            joined = wait_for(server.out, 0, r"join peer=\d+ slots=", 1.0)
            report.check("online", online is not None and joined is not None,
                         f"{online or 'the client never said Online'} / {joined or 'no join'}")
            if online is None or joined is None:
                raise RuntimeError("not online")
            peer = re.search(r"join peer=(\d+)", joined).group(1)
            pause(args.warmup)

            for away in args.away:
                print(f"--- away {away} s", flush=True)
                smark, cmark = server.out.mark(), console.mark()
                raf_before = page.evaluate("() => window.__kfmRaf")
                other = None
                if args.hide == "tab":
                    other = context.new_page()
                    other.goto("about:blank")
                    other.bring_to_front()
                elif args.hide == "emulate":
                    page.evaluate("() => window.__kfmSetHidden(true)")
                if args.hide == "freeze":
                    # Page.setWebLifecycleState("frozen") does nothing to a page
                    # headless Chromium considers visible; pausing the main
                    # thread in the debugger stops it the same way.
                    cdp.send("Debugger.enable")
                    cdp.send("Debugger.pause")
                    time.sleep(away)
                    cdp.send("Debugger.resume")
                    cdp.send("Debugger.disable")
                    state, raf_away = "frozen", None
                else:
                    pause(1.0)
                    state = page.evaluate("() => document.visibilityState")
                    raf_before = page.evaluate("() => window.__kfmRaf")
                    pause(max(0.0, away - 1.0))
                    raf_away = page.evaluate("() => window.__kfmRaf") - raf_before
                if other is not None:
                    other.close()
                    page.bring_to_front()
                elif args.hide == "emulate":
                    page.evaluate("() => window.__kfmSetHidden(false)")
                back = server.out.mark()
                hidden_ok = state in ("hidden", "frozen") and (raf_away is None or raf_away == 0)
                report.check("hidden", hidden_ok,
                             f"visibilityState={state}, animation frames while away: "
                             f"{'-' if raf_away is None else raf_away}")

                during = server.out.since(smark)[: back - smark]
                left = [line for line in during if f"leave peer={peer} " in line]
                if args.hide == "freeze":
                    lost = wait_for(console, cmark, r"Connection lost", 15.0)
                    again = wait_for(server.out, smark, r"reconnect peer=\d+", 30.0)
                    online = wait_for(console, cmark, r"kfm_client: Online on", 30.0)
                    expect_drop = away > 15
                    ok = (bool(left) == expect_drop and (lost is not None) == expect_drop
                          and online is not None and (again is not None) == expect_drop)
                    report.check("link", ok, f"leave: {left[0] if left else 'none'}; client: "
                                 f"{lost or 'no loss'}; server: {again or 'no reconnect'}")
                    if again is not None:
                        peer = re.search(r"reconnect peer=(\d+)", again).group(1)
                else:
                    background = wait_for(console, cmark, r"in the background", 0.5)
                    report.check("link", not left and background is not None,
                                 f"leave: {left[0] if left else 'none'}; "
                                 f"client: {background or 'never in the background'}")
                resync = wait_for(server.out, back, rf"resync peer={peer}\b|reconnect peer=", 10.0)
                # Still (or again, after a reconnect) running: no phase line since
                # "Online", or a new "Online" after the reconnect.
                phases = [line for line in console.since(cmark) if "kfm_client:" in line
                          and "in the background" not in line]
                running = not phases or "Online on" in phases[-1]
                report.check("resync", resync is not None and running,
                             f"{resync or 'no resync on return'}; "
                             f"{phases[-1] if phases else 'the client stayed Online'}")
                settle = server.out.mark()
                csettle = console.mark()
                pause(args.settle)
                extra = [line for line in server.out.since(settle)
                         if f"resync peer={peer}" in line or f"leave peer={peer} " in line]
                lost = [line for line in console.since(csettle) if "Connection lost" in line]
                stats = [line for line in server.out.since(settle) if line.startswith("stats ")
                         or " stats " in line]
                report.check("in-sync", not extra and not lost,
                             f"{len(extra)} resync/leave lines, {len(lost)} losses in "
                             f"{args.settle:.0f} s after the return")
                if stats:
                    report.info("server", stats[-1])
            browser.close()
    except RuntimeError:
        pass
    finally:
        http_server.shutdown()
        server.stop()

    errors = [line for line in console.lines if line.startswith("[error]")]
    report.check("console", not errors and not page_errors,
                 f"{len(errors)} console errors, {len(page_errors)} page errors")
    for line in errors + [f"[pageerror] {e}" for e in page_errors]:
        print(f"      {line}")
    if args.verbose or report.failed:
        for line in console.lines:
            print(f"      [page] {line}")
        for line in server.out.lines:
            print(f"      [server] {line}")
    print("RESULT", "FAIL" if report.failed else "PASS")
    return 1 if report.failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join(__doc__.splitlines()[1:]),
    )
    parser.add_argument("--server", type=pathlib.Path, default=default_server(),
                        help="native kfm_server (default build/server-headless, release, dev)")
    parser.add_argument("--dir", type=pathlib.Path, default=WASM_DIR,
                        help="directory holding kfm_client.html (default build/wasm/apps/client)")
    parser.add_argument("--port", type=int, default=9010, help="the server's signaling port")
    parser.add_argument("--http-port", type=int, default=0, help="HTTP port (default ephemeral)")
    parser.add_argument("--bots", type=int, default=1, help="server bots playing against the page")
    parser.add_argument("--away", type=float, nargs="+", default=[30.0],
                        help="seconds the page stays hidden, one run each, in order")
    parser.add_argument("--hide", choices=["emulate", "tab", "freeze"], default="emulate")
    parser.add_argument("--warmup", type=float, default=5.0, help="seconds online before hiding")
    parser.add_argument("--settle", type=float, default=10.0,
                        help="seconds watched after each return")
    parser.add_argument("--chromium", default=None, help="chrome or chrome-headless-shell binary")
    parser.add_argument("--chromium-arg", action="append", default=[],
                        help="extra Chromium flag (repeatable)")
    parser.add_argument("--headed", action="store_true", help="show the browser window")
    parser.add_argument("--verbose", action="store_true", help="print every page and server line")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
