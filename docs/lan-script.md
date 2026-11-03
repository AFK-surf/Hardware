# LAN scripts

A LAN script is a JavaScript function that a gateway runs on its own network, for device protocols
that take more than one request or need encryption (a Gree air conditioner's scan, bind and command;
a Xiaomi miIO lamp's hello and encrypted command). The backend sends it with the Connector's
`lan_script` method ([connector-protocol.md](connector-protocol.md) §4). The gateway knows no device
protocol: the script does, and it reaches the network only through the gateway's own LAN methods.

The runtime is `firmware/components/lan_script`, on [Micro QuickJS](https://github.com/bellard/mquickjs).
The Comma computer station runs the same runtime (Cue `systems/station/native/runner`), so a script
gives the same answers on a computer as on a board. The backend runs no script.

## The script

The source is the body of `function (args)`. It returns a JSON value: the result. The language is
Micro QuickJS's: ES5 in strict mode (declare variables with `var`; no `let`, arrow functions, classes
or holes in arrays; `Date` has `Date.now()` and `new Date(ms)` only). `JSON`, `Math`, `RegExp`,
`String`, `Array` and typed arrays are there.

Bytes are standard base64 strings everywhere, as in the Connector's methods.

| Name | What it does |
| --- | --- |
| `args` | The request's `args`, parsed (`{}` when there are none) |
| `lan.http(params)` | `http_request` with these params (text mode only); returns its result. Throws its error |
| `lan.udp(params)` | `udp_request`; returns `{responses: [{from, payload_b64}], timed_out}` |
| `lan.discover(params)` | `lan_discover` (on gateways that have it) |
| `bytes.fromText(s)`, `bytes.toText(b)` | UTF-8; invalid bytes read as U+FFFD |
| `bytes.fromHex(h)`, `bytes.toHex(b)` | Lower-case hex out |
| `bytes.fromArray(a)`, `bytes.toArray(b)` | Numbers 0 to 255 |
| `bytes.concat([b, …])`, `bytes.slice(b, start, end)`, `bytes.length(b)` | As `Array.prototype.slice` for indexes |
| `crypto.encrypt(o)`, `crypto.decrypt(o)` | AES. `o`: `mode` (`ecb`, `cbc` or `gcm`), `key` (16, 24 or 32 bytes), `iv` (16 bytes for CBC, 12 for GCM), `data`, `padding` (`none` or `pkcs7`, ECB and CBC), `aad` and `tag` (GCM). GCM encryption returns `{data, tag}`; a tag that does not match throws |
| `crypto.hash(alg, b)`, `crypto.hmac(alg, key, b)` | `md5`, `sha1` or `sha256` |
| `crypto.randomBytes(n)` | Up to 1024 bytes |
| `sleep(ms)` | Until `ms` pass or the script's time runs out |
| `console.log(…)` | A line in the answer's `logs`; values that are not strings as JSON |

A LAN call goes to the same destinations and has the same limits as the method itself. Each call
counts against the script's time.

## Limits

The gateway advertises them in `capabilities.lan_gateway.script`:

| Field | Meaning | T-Circle | DevKit | Computer |
| --- | --- | ---: | ---: | ---: |
| `api` | This API's version; a change to what a script sees raises it | 1 | 1 | 1 |
| `max_source_bytes` | Longest source | 32768 | 8192 | 65536 |
| `max_result_bytes` | Longest JSON of the result | 32768 | 4096 | 65536 |
| `max_timeout_ms` | Longest `timeout_ms` | 30000 | 30000 | 30000 |
| `heap_bytes` | The script's whole memory | 262144 | 40960 | 1048576 |

A script stops when its time runs out, when it runs out of memory, or when the user turns off device
operations. All `console.log` lines together keep at most 4 KiB; later ones are dropped and set
`logs_truncated`.

## The answer

```json
{"result": {"values": {"Pow": 1}}, "logs": ["bound"]}
{"error": "Error: device_not_answering: …\n    at  (script:12:5)", "logs": []}
```

A script that throws, times out or runs out of memory is still a `response`, with `error`: the
exception and its stack. Line numbers count from the first line of the source.
