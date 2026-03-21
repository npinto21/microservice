#!/usr/bin/env python3
import json
import signal
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


OUTPUT_PATH = sys.argv[1] if len(sys.argv) > 1 else "/tmp/p21_otlp_capture.jsonl"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 43218


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        record = {
            "path": self.path,
            "headers": {k: v for k, v in self.headers.items()},
            "body": body,
        }
        with open(OUTPUT_PATH, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(record) + "\n")
        payload = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        return


def main():
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)

    def stop_handler(signum, frame):
        server.shutdown()

    signal.signal(signal.SIGTERM, stop_handler)
    signal.signal(signal.SIGINT, stop_handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
