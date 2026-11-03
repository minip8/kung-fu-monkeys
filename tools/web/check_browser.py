#!/usr/bin/env python3
"""Smoke-check the browser build of kfm_client in headless Chromium (Playwright).

A developer tool, not a CI step. It builds and serves nothing: build the wasm
preset, start tools/web/serve.py, then point this at the page.

    python3 -m venv ~/.venvs/kfm-web && ~/.venvs/kfm-web/bin/pip install playwright
    ~/.venvs/kfm-web/bin/playwright install chromium-headless-shell   # or pass --chromium
    python3 tools/web/serve.py &
    ~/.venvs/kfm-web/bin/python tools/web/check_browser.py \\
        'http://127.0.0.1:8000/kfm_client.html?autostart&bots=3&overlay' --out /tmp/kfm-shots

Checks (each PASS/FAIL, plus INFO lines):
  page        the page loads and the Emscripten runtime initialises
  console     no console errors and no uncaught page errors
  webgl2      the canvas has a WebGL2 context whose attributes include desynchronized: true
  renders     the canvas is not a single flat colour
  animates    two screenshots taken a second apart differ (bots move, timers run)
  frames      requestAnimationFrame deltas over --frame-seconds (INFO only: software
              rasterisers such as SwiftShader are slow, so the numbers are reported, not judged)
  input       P1 holds W (move up) for a second, then presses J (throw): the screenshots
              before and after are saved for review and the share of changed pixels is
              reported (INFO only: bots move too, so a pixel diff cannot isolate P1; with
              ?overlay the debug window's Entities section lists the banana count; P1's ammo
              dot above its head turns dark once it has thrown)
  audio       the client opens its audio device on the first input event (browser
              autoplay policy, architecture §7): the "kfm_client: audio open" or
              "kfm_client: silent: ..." line it prints after the key presses and the
              state of SDL's AudioContext ("running" once resumed) (INFO only:
              whether headless Chromium has an audio output depends on the machine)

Screenshots: <out>/01-loaded.png, 02-later.png, 03-before-input.png, 04-after-move.png,
05-after-throw.png. Exit status 0 when every PASS/FAIL check passed.

Software GL: headless Chromium has no GPU here, so the browser is started with
--use-angle=swiftshader --enable-unsafe-swiftshader. --chromium points at a specific
chrome or chrome-headless-shell binary (for example one already in ~/.cache/ms-playwright
that does not match the installed Playwright's expected revision).
"""

import argparse
import pathlib
import struct
import sys
import time
import zlib

try:
    from playwright.sync_api import sync_playwright
except ImportError:  # pragma: no cover - depends on the developer's environment
    print("error: Playwright is not installed (pip install playwright)", file=sys.stderr)
    sys.exit(2)

CHROMIUM_ARGS = [
    "--use-angle=swiftshader",
    "--enable-unsafe-swiftshader",
    "--ignore-gpu-blocklist",
    "--enable-webgl",
]

FRAME_TIMES_JS = """
(seconds) => new Promise((resolve) => {
  const deltas = [];
  let last = null;
  const end = performance.now() + seconds * 1000;
  function tick(now) {
    if (last !== null) deltas.push(now - last);
    last = now;
    if (now < end) requestAnimationFrame(tick); else resolve(deltas);
  }
  requestAnimationFrame(tick);
})
"""

CONTEXT_ATTRIBUTES_JS = """
() => {
  const canvas = document.querySelector('canvas');
  if (!canvas) return { error: 'no canvas' };
  // Returns the context SDL already created (same type), or null if the
  // canvas holds a different kind of context.
  const gl = canvas.getContext('webgl2');
  if (!gl) return { error: 'getContext("webgl2") returned null' };
  return { attributes: gl.getContextAttributes(), width: canvas.width, height: canvas.height,
           version: gl.getParameter(gl.VERSION), renderer: gl.getParameter(gl.RENDERER) };
}
"""


# --- minimal PNG decoder (8-bit RGB/RGBA, non-interlaced: what Chromium writes) ---

def decode_png(data: bytes):
    """Returns (width, height, channels, rows) with rows as bytes per scanline."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    idat = bytearray()
    width = height = channels = 0
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace != 0 or color not in (2, 6):
                raise ValueError(f"unsupported PNG (depth {depth}, colour {color})")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = []
    prev = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filt = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if filt == 1:
                line[x] = (line[x] + a) & 0xFF
            elif filt == 2:
                line[x] = (line[x] + b) & 0xFF
            elif filt == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        rows.append(bytes(line))
        prev = line
    return width, height, channels, rows


def distinct_colours(png: bytes, step: int = 7) -> int:
    width, height, channels, rows = decode_png(png)
    seen = set()
    for y in range(0, height, step):
        row = rows[y]
        for x in range(0, width, step):
            seen.add(row[x * channels:x * channels + 3])
            if len(seen) > 64:
                return len(seen)
    return len(seen)


def changed_fraction(png_a: bytes, png_b: bytes, step: int = 3, threshold: int = 24) -> float:
    wa, ha, ca, rows_a = decode_png(png_a)
    wb, hb, cb, rows_b = decode_png(png_b)
    if (wa, ha) != (wb, hb):
        return 1.0
    changed = total = 0
    for y in range(0, ha, step):
        ra, rb = rows_a[y], rows_b[y]
        for x in range(0, wa, step):
            pa, pb = ra[x * ca:x * ca + 3], rb[x * cb:x * cb + 3]
            total += 1
            if sum(abs(i - j) for i, j in zip(pa, pb)) > threshold:
                changed += 1
    return changed / total if total else 0.0


# --- the checks -----------------------------------------------------------------

class Report:
    def __init__(self):
        self.failed = False

    def check(self, name: str, ok: bool, detail: str) -> None:
        print(f"{'PASS' if ok else 'FAIL'}  {name:9} {detail}")
        self.failed |= not ok

    @staticmethod
    def info(name: str, detail: str) -> None:
        print(f"INFO  {name:9} {detail}")


def summarise_frames(deltas):
    if not deltas:
        return "no requestAnimationFrame callbacks"
    ordered = sorted(deltas)
    mean = sum(deltas) / len(deltas)
    pct = lambda q: ordered[min(len(ordered) - 1, int(q * len(ordered)))]  # noqa: E731
    return (f"{len(deltas)} frames, mean {mean:.1f} ms ({1000 / mean:.1f} fps), "
            f"median {pct(0.5):.1f}, p95 {pct(0.95):.1f}, max {ordered[-1]:.1f} ms")


def run(args) -> int:
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    report = Report()
    console_errors, console_lines, page_errors = [], [], []

    with sync_playwright() as pw:
        launch = {"headless": not args.headed, "args": CHROMIUM_ARGS}
        if args.chromium:
            launch["executable_path"] = args.chromium
        browser = pw.chromium.launch(**launch)
        page = browser.new_page(viewport={"width": args.width, "height": args.height})

        def on_console(msg):
            line = f"[{msg.type}] {msg.text}"
            console_lines.append(line)
            if msg.type == "error":
                console_errors.append(line)

        page.on("console", on_console)
        page.on("pageerror", lambda exc: page_errors.append(str(exc)))

        response = page.goto(args.url, wait_until="load")
        loaded = response is not None and response.ok
        # shell.html hides #status in onRuntimeInitialized.
        try:
            page.wait_for_function(
                "() => { const s = document.getElementById('status'); return !s || s.hidden; }",
                timeout=args.timeout * 1000)
            runtime = True
        except Exception:  # noqa: BLE001 - Playwright raises its own TimeoutError
            runtime = False
        report.check("page", loaded and runtime,
                     f"HTTP {response.status if response else '-'}, runtime "
                     f"{'initialised' if runtime else 'did not initialise'}")

        time.sleep(args.wait)
        shot1 = page.screenshot(path=str(out / "01-loaded.png"))
        time.sleep(1.0)
        shot2 = page.screenshot(path=str(out / "02-later.png"))

        ctx = page.evaluate(CONTEXT_ATTRIBUTES_JS)
        if "error" in ctx:
            report.check("webgl2", False, ctx["error"])
        else:
            attrs = ctx["attributes"]
            report.check("webgl2", attrs.get("desynchronized") is True,
                         f"{ctx['width']}x{ctx['height']} {ctx['version']} on {ctx['renderer']}; "
                         f"attributes {attrs}")

        colours = distinct_colours(shot1)
        report.check("renders", colours > 4, f"{colours} distinct colours in a sampled grid")
        moved = changed_fraction(shot1, shot2)
        report.check("animates", moved > 0.0005, f"{moved * 100:.2f}% of pixels changed in 1 s")

        isolated = page.evaluate("() => window.crossOriginIsolated === true")
        report.info("isolation",
                    f"crossOriginIsolated {isolated} (true only with serve.py --coop-coep)")

        deltas = page.evaluate(FRAME_TIMES_JS, args.frame_seconds)
        report.info("frames", summarise_frames(deltas))

        page.evaluate("() => document.querySelector('canvas').focus()")
        before = page.screenshot(path=str(out / "03-before-input.png"))
        page.keyboard.down("KeyW")
        time.sleep(1.0)
        after_move = page.screenshot(path=str(out / "04-after-move.png"))
        page.keyboard.up("KeyW")
        page.keyboard.press("KeyJ")
        time.sleep(0.12)  # the banana is still next to P1, P1's ammo dot has gone dark
        page.screenshot(path=str(out / "05-after-throw.png"))
        changed = changed_fraction(before, after_move) * 100
        report.info("input", f"held W 1 s then pressed J; {changed:.2f}% of pixels changed; review "
                             "03/04/05 (P1 moves up, then a banana appears in front of it)")

        time.sleep(0.5)
        audio = [line for line in console_lines
                 if "kfm_client: audio" in line or "kfm_client: silent" in line]
        state = page.evaluate("() => (window.Module && Module.SDL3 && Module.SDL3.audioContext)"
                              " ? Module.SDL3.audioContext.state : 'no AudioContext'")
        report.info("audio", (audio[-1] if audio else
                              "no audio line: the device was not opened after the key presses")
                    + f"; AudioContext {state}")

        report.check("console", not console_errors and not page_errors,
                     f"{len(console_errors)} console errors, {len(page_errors)} page errors, "
                     f"{len(console_lines)} console lines")
        for line in console_errors + [f"[pageerror] {e}" for e in page_errors]:
            print(f"      {line}")
        if args.verbose:
            for line in console_lines:
                print(f"      {line}")
        browser.close()

    print(f"screenshots in {out}")
    print("RESULT", "FAIL" if report.failed else "PASS")
    return 1 if report.failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join(__doc__.splitlines()[1:]),
    )
    parser.add_argument("url", help="page URL, e.g. "
                        "http://127.0.0.1:8000/kfm_client.html?autostart&bots=3&overlay")
    parser.add_argument("--out", default="kfm-browser-check", help="screenshot directory")
    parser.add_argument("--chromium", default=None, help="chrome or chrome-headless-shell binary")
    parser.add_argument("--headed", action="store_true", help="show the browser window")
    parser.add_argument("--wait", type=float, default=4.0, 
                        help="seconds to let the match run first")
    parser.add_argument("--timeout", type=float, default=30.0, 
                        help="seconds to wait for the runtime")
    parser.add_argument("--frame-seconds", type=float, default=2.0, help="rAF measurement window")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--verbose", action="store_true", help="print every console line")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
