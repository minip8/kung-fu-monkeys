#!/usr/bin/env python3
"""Serve the browser build of kfm_client (or the render probe) on localhost.

    source ~/emsdk/emsdk_env.sh && cmake --workflow --preset wasm
    python3 tools/web/serve.py                                  # the client
    python3 tools/web/serve.py --page kfm_render_probe.html     # the render probe
    python3 tools/web/serve.py --coop-coep                      # cross-origin isolated

Serves build/wasm/apps/client (or, for --page kfm_render_probe.html,
build/wasm/apps/render-probe; --dir overrides either) with the right MIME types
(application/wasm is required for streaming compilation) and no caching, so a
rebuild shows up on reload. --coop-coep adds Cross-Origin-Opener-Policy:
same-origin and Cross-Origin-Embedder-Policy: require-corp, which make the page
cross-origin isolated (SharedArrayBuffer, wasm threads); the shipping client is
single-threaded and does not need them.

Query-string flags become kfm_client command-line flags (shell.html):
?autostart&bots=3&seed=7&overlay runs kfm_client --autostart --bots 3 --seed 7 --overlay.
"""

import argparse
import functools
import http.server
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WASM_APPS = REPO_ROOT / "build" / "wasm" / "apps"
DEFAULT_PAGE = "kfm_client.html"
# Build directory of each known page; any other --page needs --dir.
PAGE_DIRS = {
    "kfm_client.html": WASM_APPS / "client",
    "kfm_render_probe.html": WASM_APPS / "render-probe",
}
QUERY_EXAMPLES = (
    "?autostart&bots=3&overlay",
    "?autostart&bots=9&seed=7",
    "?bots=1",
)


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".html": "text/html; charset=utf-8",
        ".js": "text/javascript; charset=utf-8",
        ".mjs": "text/javascript; charset=utf-8",
        ".wasm": "application/wasm",
        ".data": "application/octet-stream",
        ".map": "application/json",
    }

    def __init__(self, *args, coop_coep=False, **kwargs):
        self.coop_coep = coop_coep
        super().__init__(*args, **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        if self.coop_coep:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join(__doc__.splitlines()[1:]),
    )
    parser.add_argument("--page", default=DEFAULT_PAGE,
                        help=f"page to serve (default {DEFAULT_PAGE}; also kfm_render_probe.html)")
    parser.add_argument("--dir", type=pathlib.Path, default=None,
                        help="directory to serve (default: the page's build/wasm/apps folder)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--coop-coep", action="store_true",
                        help="send COOP/COEP headers (cross-origin isolation for threads)")
    args = parser.parse_args()

    directory = args.dir if args.dir is not None else PAGE_DIRS.get(args.page)
    if directory is None:
        print(f"error: unknown page {args.page}; pass --dir", file=sys.stderr)
        return 1
    if not (directory / args.page).is_file():
        print(f"error: {directory}/{args.page} not found; build the wasm preset first",
              file=sys.stderr)
        return 1
    handler = functools.partial(Handler, directory=str(directory), coop_coep=args.coop_coep)
    with http.server.ThreadingHTTPServer((args.host, args.port), handler) as server:
        base = f"http://{args.host}:{server.server_address[1]}/{args.page}"
        print(f"serving {directory}{' with COOP/COEP' if args.coop_coep else ''}")
        print(f"  {base}")
        if args.page == DEFAULT_PAGE:
            for query in QUERY_EXAMPLES:
                print(f"  {base}{query}")
        sys.stdout.flush()
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
