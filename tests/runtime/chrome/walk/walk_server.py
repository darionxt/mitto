"""A throwaway server for darionxt/mitto#207, never merged.

Serves walk.html and walk.js from this directory over HTTP and HTTPS on the loopback address, and writes what a page
posts to /result?run=NAME into OUT/NAME.json. It needs only the standard library, so it runs on any Python 3.12.
"""

import argparse
import http.server
import pathlib
import ssl
import threading
import urllib.parse

HERE = pathlib.Path(__file__).resolve().parent
FILES = {"/walk.html": "text/html; charset=utf-8", "/walk.js": "text/javascript; charset=utf-8"}


def make_handler(out: pathlib.Path, log: pathlib.Path) -> type[http.server.BaseHTTPRequestHandler]:
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:  # noqa: N802
            path = urllib.parse.urlsplit(self.path).path
            if path not in FILES:
                self.send_error(404)
                return
            body = (HERE / path[1:]).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", FILES[path])
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self) -> None:  # noqa: N802
            parts = urllib.parse.urlsplit(self.path)
            if parts.path != "/result":
                self.send_error(404)
                return
            run = urllib.parse.parse_qs(parts.query).get("run", ["unnamed"])[0]
            name = "".join(c for c in run if c.isalnum() or c in "-_.") or "unnamed"
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            partial = out / f"{name}.json.part"
            partial.write_bytes(body)
            partial.replace(out / f"{name}.json")
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, format: str, *args: object) -> None:  # noqa: A002
            with lock, log.open("a", encoding="utf-8") as file:
                file.write(f"{self.address_string()} {self.headers.get('Host')} {format % args}\n")

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--http-port", type=int, required=True)
    parser.add_argument("--https-port", type=int, required=True)
    parser.add_argument("--cert", type=pathlib.Path, required=True)
    parser.add_argument("--key", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    handler = make_handler(args.out, args.out / "server.log")
    plain = http.server.ThreadingHTTPServer(("127.0.0.1", args.http_port), handler)
    secure = http.server.ThreadingHTTPServer(("127.0.0.1", args.https_port), handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    secure.socket = context.wrap_socket(secure.socket, server_side=True)
    threading.Thread(target=secure.serve_forever, daemon=True).start()
    plain.serve_forever()


if __name__ == "__main__":
    main()
