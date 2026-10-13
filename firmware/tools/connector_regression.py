#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Live-board regressions against a local Connector and controlled HTTP peer.

Build a separate board image with COMMA_CONNECT_URL_OVERRIDE=ws://<computer>:8765/v1/connect.
Only the dummy override token is accepted. The original operation permission is restored after
checks; credentials are never read or changed. Restore the normal firmware after testing.
"""
import argparse
import asyncio
import base64
from collections import Counter
import contextlib
import hashlib
import json
from pathlib import Path
import socket
import struct
from types import SimpleNamespace
from urllib.parse import urlsplit

from fake_connector import Session, WebSocket, handshake, log

BLOB = bytes(range(256)) * 4096


class EchoUDP(asyncio.DatagramProtocol):
    def __init__(self, hits):
        self.hits = hits

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self.hits[data] += 1
        self.transport.sendto(data, addr)


class Regression:
    def __init__(self, args):
        self.args = args
        self.hits = Counter()
        self.udp_hits = Counter()
        self.started = {}
        self.release = asyncio.Event()
        self.results = []
        self.original_allow = None
        self.phase = 0
        self.finished = asyncio.Event()
        self.initial_board = None
        self.failure = None

    def check(self, name, condition, detail=None):
        self.results.append({'name': name, 'passed': bool(condition), 'detail': detail})
        log(f"{'PASS' if condition else 'FAIL'} {name}" + (f': {detail}' if detail else ''))
        self.save()
        if not condition:
            raise AssertionError(name)

    def save(self):
        Path(self.args.report).write_text(json.dumps({'board': self.initial_board,
            'original_allow_operations': self.original_allow, 'phase': self.phase,
            'complete': self.phase == 2, 'failure': self.failure,
            'checks': self.results}, indent=2) + '\n')

    async def http(self, reader, writer):
        try:
            head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 5)
            path = urlsplit(head.split(b' ', 2)[1].decode()).path
            self.hits[path] += 1
            self.started.setdefault(path, asyncio.Event()).set()
            if path == '/continue':
                writer.write(b'HTTP/1.1 100 Continue\r\nX-Interim: discard\r\n\r\n')
                await writer.drain()
                await asyncio.sleep(0.15)
            elif path == '/hints':
                writer.write(b'HTTP/1.1 103 Early Hints\r\nLink: discard\r\n\r\n')
            if path in ('/fin', '/reset'):
                writer.write(b'HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nPARTIAL')
                await writer.drain()
                await asyncio.sleep(0.15)
                if path == '/reset':
                    writer.get_extra_info('socket').setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    writer.transport.abort()
                return
            body = BLOB if path == '/blob' else b'x' * 65536 if path == '/text-limit' else b'HELLO'
            writer.write(f'HTTP/1.1 200 OK\r\nContent-Length: {len(body)}\r\nX-Final: keep\r\nConnection: close\r\n\r\n'.encode())
            await writer.drain()
            if path == '/slow':
                await asyncio.sleep(2)
            elif path == '/hold':
                await asyncio.wait_for(self.release.wait(), 10)
            writer.write(body)
            await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError, asyncio.TimeoutError):
            pass
        finally:
            writer.close()
            with contextlib.suppress(ConnectionError):
                await writer.wait_closed()

    async def access(self, session, allow):
        p = await session.request('device_access', {'allow_operations': allow}, 10)
        frame = p.final or {}
        expected = '' if allow else 'local_file_read'
        self.check(f'operation permission {allow}',
            frame.get('result', {}).get('allows_operations') is allow and
            session.metadata['capabilities']['scope'] == expected)

    async def fetch(self, session, path, timeout=10, **extra):
        params = {'method': 'GET', 'url': f'http://{session.local_ip}:{self.args.http_port}{path}',
                  'timeout_seconds': timeout, **extra}
        return await session.request('http_request', params, timeout + 5)

    async def udp(self, session, payload):
        return await session.request('udp_request', {'host': session.local_ip,
            'port': self.args.udp_port, 'payload_b64': base64.b64encode(payload).decode(),
            'timeout_ms': 1000, 'max_responses': 1}, 10)

    async def first(self, s):
        self.original_allow = s.metadata['capabilities']['scope'] == ''
        await self.access(s, False)
        p = await self.fetch(s, '/denied')
        self.check('read-only request never reaches HTTP peer',
                   (p.final or {}).get('type') == 'error' and self.hits['/denied'] == 0)
        await self.access(s, True)
        for path in ('/continue', '/hints', '/fin', '/reset'):
            for chunks in (False, True):
                extra = {'response_mode': 'chunks'} if chunks else {}
                p = await self.fetch(s, path, **extra)
                frame = p.final or {}
                if path == '/reset':
                    self.check(f'{path} chunks={chunks} returns error', frame.get('type') == 'error', frame.get('error'))
                else:
                    result = frame.get('result', {})
                    expected = b'PARTIAL' if path == '/fin' else b'HELLO'
                    body = bytes(p.data) if chunks else result.get('body', '').encode()
                    self.check(f'{path} chunks={chunks} final response', result.get('status') == 200 and body == expected)
                    headers = result.get('headers', {})
                    self.check(f'{path} chunks={chunks} discards interim headers', 'X-Interim' not in headers and 'Link' not in headers)
        cap = s.metadata['capabilities']['lan_gateway']['max_bytes']
        for n in range(3):
            p = await self.fetch(s, '/text-limit', max_bytes=cap)
            self.check(f'text cap round {n+1}', (p.final or {}).get('result', {}).get('body_bytes') == cap)
            p = await self.fetch(s, '/blob', timeout=60, response_mode='chunks')
            result = (p.final or {}).get('result', {})
            self.check(f'1 MiB chunks round {n+1}', bytes(p.data) == BLOB and not p.problems and
                       result.get('sha256') == hashlib.sha256(BLOB).hexdigest(), f'{len(p.data)} bytes')
            payload = b'comma-regression-udp-' + bytes([n, 0, 255])
            p = await self.udp(s, payload)
            result = (p.final or {}).get('result', {})
            answers = result.get('responses', [])
            self.check(f'UDP binary echo round {n+1}', len(answers) == 1 and
                base64.b64decode(answers[0]['payload_b64']) == payload and
                result.get('timed_out') is False)

        self.started['/slow'] = asyncio.Event()
        slow = asyncio.create_task(self.fetch(s, '/slow', timeout=5))
        await asyncio.wait_for(self.started['/slow'].wait(), 5)
        expired = asyncio.create_task(self.fetch(s, '/expired', timeout=1, method='POST'))
        await slow
        p = await expired
        self.check('expired queued POST never reaches HTTP peer',
                   (p.final or {}).get('type') == 'error' and self.hits['/expired'] == 0, (p.final or {}).get('error'))

        self.started['/hold'] = asyncio.Event()
        hold = asyncio.create_task(self.fetch(s, '/hold', timeout=8))
        await asyncio.wait_for(self.started['/hold'].wait(), 5)
        denied = asyncio.create_task(self.fetch(s, '/revoked', timeout=5, method='POST'))
        denied_udp = asyncio.create_task(self.udp(s, b'revoked-udp'))
        await asyncio.sleep(0.2)
        await self.access(s, False)
        self.release.set()
        await hold
        p = await denied
        self.check('revoked queued POST never reaches HTTP peer',
                   (p.final or {}).get('type') == 'error' and self.hits['/revoked'] == 0)
        p = await denied_udp
        self.check('revoked queued UDP never reaches UDP peer',
                   (p.final or {}).get('type') == 'error' and self.udp_hits[b'revoked-udp'] == 0)
        await self.access(s, True)

        # Keep one job in flight, queue a side effect, then close the originating Connector.
        self.release.clear()
        self.started['/hold'] = asyncio.Event()
        hold = asyncio.create_task(self.fetch(s, '/hold', timeout=8))
        await asyncio.wait_for(self.started['/hold'].wait(), 5)
        old = asyncio.create_task(self.fetch(s, '/disconnected', timeout=5, method='POST'))
        old_udp = asyncio.create_task(self.udp(s, b'disconnected-udp'))
        await asyncio.sleep(0.2)
        self.phase = 1
        self.save()
        log('Closing Connector with an in-flight job and a queued POST')
        s.ws.writer.close()
        await asyncio.sleep(0.15)
        self.release.set()
        for task in (hold, old, old_udp):
            task.cancel()
        await asyncio.gather(hold, old, old_udp, return_exceptions=True)

    async def connected(self, reader, writer):
        tasks = []
        session = None
        try:
            hs = await handshake(reader, writer)
            if not hs or hs[2].get('authorization') != 'Bearer salix_conn_dev_override':
                log('Rejected connection: use a development URL override image')
                return
            session = Session(WebSocket(reader, writer), SimpleNamespace(ack_pause=0), writer.get_extra_info('sockname')[0])
            await session.ws.send_json({'type': 'connected', 'connector_run_id': 'run_regression', 'connection_generation': self.phase + 1})
            tasks = [asyncio.create_task(session.reader()), asyncio.create_task(session.heartbeats())]
            await asyncio.wait_for(session.metadata_event.wait(), 15)
            board = session.metadata.get('system_info', {}).get('client_source')
            if self.initial_board is None:
                self.initial_board = board
            if board != self.initial_board:
                raise RuntimeError('A different board connected mid-test')
            if self.phase == 0:
                await self.first(session)
            elif self.phase == 1:
                await asyncio.sleep(2)
                self.check('disconnected queued POST never reaches HTTP peer', self.hits['/disconnected'] == 0)
                self.check('disconnected queued UDP never reaches UDP peer', self.udp_hits[b'disconnected-udp'] == 0)
                p = await self.fetch(session, '/recovered')
                self.check('new connection executes new requests', (p.final or {}).get('result', {}).get('body') == 'HELLO')
                await self.access(session, self.original_allow)
                self.phase = 2
                self.save()
                log('DONE: regression checks passed; original permission restored')
                self.finished.set()
                await tasks[0]
            else:
                if self.original_allow is not None:
                    await self.access(session, self.original_allow)
                await tasks[0]
        except (ConnectionError, asyncio.IncompleteReadError):
            if self.phase not in (1, 2):
                self.failure = 'Unexpected disconnection; test incomplete'
                self.phase = -1
                self.save()
                log(self.failure)
                self.finished.set()
        except Exception as exc:
            self.failure = f'{type(exc).__name__}: {exc}'
            self.phase = -1
            log(f'ERROR: {type(exc).__name__}: {exc}')
            if session and self.original_allow is not None and not writer.is_closing():
                with contextlib.suppress(Exception):
                    await self.access(session, self.original_allow)
            self.save()
            self.finished.set()
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            writer.close()

    async def run(self):
        udp, _ = await asyncio.get_running_loop().create_datagram_endpoint(
            lambda: EchoUDP(self.udp_hits), local_addr=('0.0.0.0', self.args.udp_port))
        http = await asyncio.start_server(self.http, '0.0.0.0', self.args.http_port)
        ws = await asyncio.start_server(self.connected, '0.0.0.0', self.args.port)
        log(f'Waiting for one board on :{self.args.port}; HTTP peer :{self.args.http_port}')
        try:
            async with http, ws:
                await self.finished.wait()
                # Stay connected for serial observation until the operator restores normal firmware.
                await asyncio.Event().wait()
        finally:
            udp.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--http-port', type=int, default=8766)
    parser.add_argument('--udp-port', type=int, default=8767)
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    try:
        asyncio.run(Regression(args).run())
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
