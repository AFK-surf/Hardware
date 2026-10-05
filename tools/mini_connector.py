#!/usr/bin/env python3
"""Smallest working Comma Connector (A1 spike). stdlib + websockets only.

Speaks the /v1/connect protocol the way salix-connect does in device mode,
but serves only `http_request` and `device_access`. The ESP32 firmware (A2/A3)
copies this behaviour; see ../docs/connector-protocol.md.

    export SALIX_CONNECTOR_TOKEN=salix_conn_...
    .venv/bin/python mini_connector.py --server ws://127.0.0.1:4000 --name t-circle

Probe flags (--no-instance-id, --no-scope-param, --no-metadata, --skills, ...)
exist only to answer the A1 questions; the firmware should use the defaults.
"""

import argparse
import asyncio
import http.client
import json
import logging
import os
import platform
import random
import socket
import ssl
import time
import uuid
from urllib.parse import urlencode, urlsplit

import websockets

SCOPE_READ_ONLY = "local_file_read"  # server name for "operations not allowed"
SCOPE_FULL = ""
MAX_FRAME = 16 * 1024 * 1024  # server max_frame_size (salix_web/connect.ex)
MAX_BODY = 10 * 1024 * 1024  # Go maxFile; firmware will use far less
HEARTBEAT_S = 15
HTTP_METHODS = {"GET", "POST", "PUT", "PATCH", "DELETE"}

log = logging.getLogger("mini")


class AccessState:
    """device_access preference, persisted like Go's device-access.json."""

    def __init__(self, path):
        self.path = path
        self.scope = SCOPE_READ_ONLY  # device mode defaults to read-only
        try:
            with open(path) as f:
                stored = json.load(f).get("scope")
            if stored in (SCOPE_FULL, SCOPE_READ_ONLY):
                self.scope = stored
        except FileNotFoundError:
            pass

    @property
    def allows_operations(self):
        return self.scope == SCOPE_FULL

    def set(self, allow):
        self.scope = SCOPE_FULL if allow else SCOPE_READ_ONLY
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"scope": self.scope}, f)
        os.replace(tmp, self.path)


def http_request(params):
    """Same params/result shape as salix-connect methodHTTPRequest (main.go)."""
    method = str(params.get("method") or "POST").upper()
    if method not in HTTP_METHODS:
        raise ValueError(f"unsupported HTTP method: {method}")
    raw_url = params.get("url") or ""
    if not raw_url:
        raise ValueError("'url' is required")
    u = urlsplit(raw_url)
    if not u.scheme or not u.netloc:
        raise ValueError("'url' must be an absolute http or https URL")
    if u.scheme not in ("http", "https"):
        raise ValueError("'url' must use http or https")

    timeout = int(params.get("timeout_seconds") or 120)
    if timeout <= 0:
        timeout = 120
    limit = int(params.get("max_bytes") or MAX_BODY)
    if limit <= 0 or limit > MAX_BODY:
        limit = MAX_BODY

    body = params.get("body") or ""
    headers = {str(k): str(v) for k, v in (params.get("headers") or {}).items() if k}
    if body and not any(k.lower() == "content-type" for k in headers):
        headers["Content-Type"] = "application/json"

    conn_cls = http.client.HTTPSConnection if u.scheme == "https" else http.client.HTTPConnection
    kwargs = {"timeout": timeout}
    if u.scheme == "https":
        kwargs["context"] = ssl.create_default_context()
    conn = conn_cls(u.hostname, u.port, **kwargs)
    path = (u.path or "/") + (f"?{u.query}" if u.query else "")
    try:
        # Redirects are not followed (Go: http.ErrUseLastResponse).
        conn.request(method, path, body=body.encode() if body else None, headers=headers)
        resp = conn.getresponse()
        raw = resp.read(limit + 1)
        resp_headers = {}
        for k, v in resp.getheaders():
            resp_headers.setdefault(http_canonical(k), []).append(v)
    finally:
        conn.close()
    truncated = len(raw) > limit
    raw = raw[:limit]
    return {
        "status": resp.status,
        "headers": resp_headers,
        "body": raw.decode("utf-8", errors="replace"),
        "truncated": truncated,
        "final_url": raw_url,
        "method": method,
        "body_bytes": len(raw),
    }


def http_canonical(key):
    return "-".join(p[:1].upper() + p[1:].lower() for p in key.split("-"))


class MiniConnector:
    def __init__(self, args):
        self.args = args
        self.access = AccessState(args.state)
        # Stable per process, new after restart (Go: processInstanceID).
        self.instance_id = str(uuid.uuid4())
        self.ws = None

    def url(self):
        q = {"name": self.args.name}
        if self.args.alias:
            q["alias"] = self.args.alias
        if not self.args.no_os_arch:
            q["os"] = self.args.os
            q["arch"] = self.args.arch
        if not self.args.no_scope_param:
            q["scope"] = self.access.scope
        return self.args.server.rstrip("/") + "/v1/connect?" + urlencode(q)

    def metadata(self):
        caps = {
            "scope": self.access.scope,
            "device_access": True,
            "execution_boundary": "lan-gateway",
            "persistent_processes": False,
            "computer_use_tool": False,
            "host_access_required": False,
            "runtime_probe": False,
            "meeting_runtime": False,
            "component_versions": {"mini-connector": "a1-spike"},
        }
        msg = {"type": "metadata", "capabilities": caps}
        if self.args.skills is not None:
            msg["skills"] = json.loads(self.args.skills)
        else:
            msg["skills"] = []
        if not self.args.no_system_info:
            msg["system_info"] = {
                "client_source": "mini-connector",
                "hostname": self.args.hostname,
                "os_type": self.args.os,
                "arch": self.args.arch,
                "collected_at": int(time.time()),
            }
        return msg

    async def send(self, msg):
        await self.ws.send(json.dumps(msg, separators=(",", ":")))

    async def run_forever(self):
        backoff = 1.0
        while True:
            started = time.monotonic()
            try:
                await self.run_once()
            except websockets.InvalidStatus as e:
                status = e.response.status_code
                body = e.response.body.decode(errors="replace")[:300]
                log.error("handshake rejected: HTTP %s %s", status, body)
                if status in (401, 403) and not self.args.retry_auth:
                    return
            except (OSError, websockets.ConnectionClosed, asyncio.TimeoutError) as e:
                log.warning("connection lost: %r", e)
            if time.monotonic() - started > 60:
                backoff = 1.0
            delay = backoff * (0.5 + random.random())
            log.info("reconnecting in %.1fs", delay)
            await asyncio.sleep(delay)
            backoff = min(backoff * 2, 30.0)

    async def run_once(self):
        headers = {"Authorization": f"Bearer {self.args.token}"}
        if not self.args.no_instance_id:
            headers["X-Salix-Connector-Instance-ID"] = self.instance_id
        url = self.url()
        log.info("dial %s", url)
        async with websockets.connect(
            url,
            additional_headers=headers,
            max_size=MAX_FRAME,
            open_timeout=15,
            ping_interval=None,  # app-level heartbeat only, like salix-connect
            compression=None,
            proxy=None,
        ) as ws:
            self.ws = ws
            if not self.args.no_metadata:
                await self.send(self.metadata())
            await self.read_loop(ws)

    async def read_loop(self, ws):
        while True:
            # Server heartbeats every 15 s; 45 s of silence = dead (Go: webSocketReadTimeout).
            raw = await asyncio.wait_for(ws.recv(), timeout=45)
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            t = msg.get("type")
            if t == "connected":
                log.info(
                    "connected run=%s device=%s gen=%s",
                    msg.get("connector_run_id"),
                    msg.get("device_id"),
                    msg.get("connection_generation"),
                )
            elif t == "heartbeat":
                if not self.args.no_heartbeat_reply:
                    await self.send({"type": "heartbeat"})
            elif t == "request":
                asyncio.create_task(self.handle_request(msg))
            else:
                log.debug("ignored frame: %s", raw[:200])

    async def handle_request(self, msg):
        rid, method, params = msg.get("id"), msg.get("method"), msg.get("params") or {}
        log.info("request %s %s %s", rid, method, json.dumps(params)[:200])
        try:
            if method == "device_access":
                result = await self.device_access(params)
            elif method == "http_request":
                if not self.access.allows_operations:
                    raise PermissionError(f'method "{method}" is not permitted for a {SCOPE_READ_ONLY} connector')
                result = await asyncio.to_thread(http_request, params)
            else:
                raise LookupError(f"unknown method: {method}")
            reply = {"id": rid, "type": "response", "result": result}
        except Exception as e:  # every failure becomes a typed error frame
            reply = {"id": rid, "type": "error", "error": str(e) or type(e).__name__}
        log.info("reply %s %s", rid, reply["type"] if reply["type"] == "response" else reply["error"])
        await self.send(reply)

    async def device_access(self, params):
        allow = params.get("allow_operations")
        if not isinstance(allow, bool):
            raise ValueError("allow_operations must be a boolean")
        self.access.set(allow)
        await self.send(self.metadata())  # republish scope so routing updates
        return {"allows_operations": allow}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--server", default=os.environ.get("SALIX_SERVER", "ws://127.0.0.1:4000"))
    p.add_argument("--token", default=os.environ.get("SALIX_CONNECTOR_TOKEN"))
    p.add_argument("--name", default="mini-connector")
    p.add_argument("--alias", default="")
    p.add_argument("--hostname", default="T-Circle (mini)")
    p.add_argument("--os", default="esp-idf")
    p.add_argument("--arch", default="xtensa")
    p.add_argument("--state", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".mini-device-access.json"))
    p.add_argument("--skills", help="JSON for the metadata skills field (default [])")
    p.add_argument("--no-instance-id", action="store_true")
    p.add_argument("--no-scope-param", action="store_true")
    p.add_argument("--no-os-arch", action="store_true")
    p.add_argument("--no-system-info", action="store_true")
    p.add_argument("--no-metadata", action="store_true")
    p.add_argument("--no-heartbeat-reply", action="store_true")
    p.add_argument("--retry-auth", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()
    if not args.token:
        p.error("set SALIX_CONNECTOR_TOKEN or --token")
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    logging.getLogger("websockets").setLevel(logging.INFO)
    try:
        asyncio.run(MiniConnector(args).run_forever())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    socket.setdefaulttimeout(30)
    main()
