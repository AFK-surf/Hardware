# SPDX-License-Identifier: Apache-2.0
"""Raw HTTP server for lan_http_test: one scenario per path. Prints its port, then serves."""
import json
import socket
import sys
import threading
import time


def read_request(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    method, path, _ = lines[0].split(" ", 2)
    headers = [line.split(": ", 1) for line in lines[1:]]
    length = int(next((v for k, v in headers if k.lower() == "content-length"), "0"))
    while len(rest) < length:
        rest += conn.recv(4096)
    return method, path, headers, rest


def pattern(n):
    """Every byte value, in a sequence that doesn't repeat every 256 bytes. test.c builds the same."""
    return bytes((i ^ (i >> 8) ^ (i >> 16)) & 0xFF for i in range(n))


def handle(conn):
    req = read_request(conn)
    if not req:
        conn.close()
        return
    method, path, headers, body = req
    send = conn.sendall
    if path == "/cl":
        send(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello")
    elif path == "/chunked":
        send(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
    elif path == "/close":
        send(b"HTTP/1.0 200 OK\r\nServer: tiny\r\n\r\nno length here")
    elif path == "/big":
        send(b"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n" + b"x" * 100000)
    elif path == "/exact":
        send(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789")
    elif path == "/redirect":
        send(b"HTTP/1.1 302 Found\r\nLocation: http://192.168.1.50/elsewhere\r\nContent-Length: 0\r\n\r\n")
    elif path == "/dup":
        send(b"HTTP/1.1 200 OK\r\nset-cookie: a=1\r\nSET-COOKIE: b=2\r\nx-thing: y\r\nContent-Length: 0\r\n\r\n")
    elif path == "/binary":
        send(b"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nA\x00B\xff\"\\\n\xe4\xbd")
    elif path == "/utf8":
        payload = "灯 on ✓".encode()
        send(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(payload) + payload)
    elif path == "/slow":
        time.sleep(3)
        send(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
    elif path == "/early-close":
        send(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly ten b")
    elif path == "/garbage":
        send(b"SSH-2.0-OpenSSH\r\n\r\n")
    elif path.startswith("/pattern/"):  # binary, Content-Length
        data = pattern(int(path.split("/")[2]))
        send(b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n" % len(data))
        send(data)
    elif path.startswith("/pattern-chunked/"):  # binary, Transfer-Encoding: chunked, uneven chunks
        data = pattern(int(path.split("/")[2]))
        send(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
        at, step = 0, 1
        while at < len(data):
            piece = data[at:at + step]
            send(b"%x\r\n" % len(piece) + piece + b"\r\n")
            at += len(piece)
            step = step * 3 + 1 if step < 20000 else 777
        send(b"0\r\n\r\n")
    elif path.startswith("/pattern-close/"):  # binary, HTTP/1.0, ends at close
        data = pattern(int(path.split("/")[2]))
        send(b"HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\n\r\n" + data)
    elif path == "/status500":
        send(b"HTTP/1.1 500 Internal Server Error\r\nContent-Length: 4\r\n\r\noops")
    else:  # /echo and anything else: what arrived
        out = json.dumps({"method": method, "path": path, "headers": headers, "body": body.decode("latin-1")}).encode()
        send(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(out) + out)
    try:
        conn.shutdown(socket.SHUT_WR)
    except OSError:
        pass
    time.sleep(0.05)
    conn.close()


def main():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(16)
    print(srv.getsockname()[1], flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
