// SPDX-License-Identifier: Apache-2.0
// Computes for the script's whole time and never waits: run it on a board with
// `fake_connector.py --script busy.js` and timeout_ms 30000. It must end with the error
// "the script timed out" after about 30 s, with no task watchdog message and no reset on the console.
var n = 0;
for (;;) n = (n * 31 + 7) % 1000003;
