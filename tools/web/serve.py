#!/usr/bin/env python3
"""Serve the browser build of kfm_client on localhost.

    source ~/emsdk/emsdk_env.sh && cmake --workflow --preset wasm
    python3 tools/web/serve.py            # then open http://127.0.0.1:8000/kfm_client.html

Serves build/wasm/apps/client (or --dir) with the right MIME types (application/wasm
is required for streaming compilation) and no caching, so a rebuild shows up on reload.
"""

import argparse
import functools
import http.server
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_DIR = REPO_ROOT / "build" / "wasm" / "apps" / "client"


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

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dir", type=pathlib.Path, default=DEFAULT_DIR)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()

    if not (args.dir / "kfm_client.html").is_file():
        print(f"error: {args.dir}/kfm_client.html not found; build the wasm preset first",
              file=sys.stderr)
        return 1
    handler = functools.partial(Handler, directory=str(args.dir))
    with http.server.ThreadingHTTPServer((args.host, args.port), handler) as server:
        print(f"serving {args.dir} at http://{args.host}:{args.port}/kfm_client.html")
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
