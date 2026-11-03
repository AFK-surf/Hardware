// SPDX-License-Identifier: Apache-2.0
// Sends the request's file to args.host:args.port after a short head, for the board's file streaming
// check: fake_connector.py --script send_file.js --script-file <file>
//   --script-args '{"host":"<a Mac on the LAN running nc -l 9101 > out.bin>","port":9101}'
// out.bin must then be "HEAD\n" followed by the file, byte for byte.
var r = lan.tcp({ host: args.host, port: args.port, send_b64: bytes.fromText("HEAD\n"), send_file: true,
  close_write: true, idle_ms: 2000, timeout_ms: 30000 });
return { file_bytes: args.file_bytes, closed: r.closed, timed_out: r.timed_out };
