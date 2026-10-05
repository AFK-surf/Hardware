"""A minimal MCP server (streamable HTTP, plain JSON responses) to stand in for a LAN device.

Register it as a device-placed remote MCP pointing at http://<this machine's LAN IP>:8765/mcp and
every MCP call reaches it as a Connector http_request from the gateway device. Each request is
logged with the caller's IP, so you can see that it came from the T-Circle and not the cloud.

    python3 lan_mcp_server.py [port]
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOOLS = [
    {
        "name": "lan_status",
        "description": "Status of a test device on the office LAN: its local time and who asked.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "echo",
        "description": "Echo a message back from the LAN test device.",
        "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]},
    },
]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, status, body=b"", content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        print(f"{time.strftime('%H:%M:%S')} {self.client_address[0]} GET {self.path}", flush=True)
        if self.path == "/mcp":
            self._send(405, b"")
        else:
            self._send(200, b"LAN test device is up\n", "text/plain")

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        try:
            msg = json.loads(raw)
        except ValueError:
            self._send(400, b'{"error":"bad json"}')
            return
        method = msg.get("method")
        print(f"{time.strftime('%H:%M:%S')} {self.client_address[0]} POST {self.path} {method}", flush=True)
        if "id" not in msg:  # a notification
            self._send(202)
            return
        if method == "initialize":
            result = {
                "protocolVersion": msg.get("params", {}).get("protocolVersion", "2025-03-26"),
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "lan-test-device", "version": "0.1.0"},
            }
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            name = msg.get("params", {}).get("name")
            args = msg.get("params", {}).get("arguments") or {}
            if name == "lan_status":
                text = f"LAN test device ok, local time {time.strftime('%H:%M:%S')}, asked by {self.client_address[0]}"
            elif name == "echo":
                text = f"echo from the LAN: {args.get('text', '')}"
            else:
                body = {"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32602, "message": f"unknown tool {name}"}}
                self._send(200, json.dumps(body).encode())
                return
            result = {"content": [{"type": "text", "text": text}]}
        elif method == "ping":
            result = {}
        else:
            body = {"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32601, "message": f"no method {method}"}}
            self._send(200, json.dumps(body).encode())
            return
        self._send(200, json.dumps({"jsonrpc": "2.0", "id": msg["id"], "result": result}).encode())


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"listening on 0.0.0.0:{port}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
