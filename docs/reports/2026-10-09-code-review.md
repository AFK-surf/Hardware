# Code review: remaining firmware issues

2026-10-09. Static review plus two HTTP reproductions using the actual firmware implementation,
the existing host-test stubs, and injected transport reads. No firmware fixes or board tests were
performed during this review. These findings are additional to the limitations in the G3 report.

## 1. Operation permission survives a change of credentials

`connector.c` loads `connector/allow_ops` once at task startup. `device_config_save()` replaces the
workspace, group, token, and other credentials, but does not clear that permission or its in-memory
copy. A board previously allowed to operate can therefore be re-provisioned to a different workspace
through the 3-second pairing flow and immediately advertise operation permission there.

Bind operation consent to the credential identity or workspace, and reset it when that identity
changes. Credential rotation within the same authorized identity needs an explicit policy.
Confirmed by code paths; cross-workspace board reproduction remains to be done.

## 2. Queued LAN requests can outlive their connection or caller deadline

`lan_worker()` skips jobs only when `job.session != s_session`. Disconnect clears `s_up`, but the
session counter changes only at the next dial. During reconnect backoff, queued HTTP/UDP work from
the closed connection can still run. The queue also records no arrival time or deadline; each
HTTP timeout starts when the worker begins the job, after any preceding requests finish. A control
request can execute after its caller has already timed out.

Invalidate queued work at disconnect, check connection liveness before starting it, and enforce a
deadline that includes queue time. Confirmed by code paths; timing tests remain to be added.

## 3. A failed read can turn an incomplete HTTP body into success

`lan_http_run()` treats every negative transport read as end-of-stream and tells `http_parser` that
the response ended. For a response with no Content-Length or chunked framing, a connection failure
is consequently accepted as a complete body.

Reproduced: inject `HTTP/1.1 200 OK`, `Connection: close`, and the body `PARTIAL`, followed by
`ERR_TCP_TRANSPORT_CONNECTION_FAILED`. The firmware returns status 200, body `PARTIAL`, and
`truncated: false`. Distinguish an orderly FIN from a failed read; a failed transfer must return an
error rather than a successful partial result.

## 4. Informational HTTP responses end the request too early

`on_message_complete()` sets the response's `complete` flag even for an informational response.
If `100 Continue` arrives in a separate read before the final response, the read loop ends there.

Reproduced: inject a separate `100 Continue`, followed by a valid `200 OK` with body `HELLO`. Only
the first read is consumed, and the firmware returns status 100 with an empty body. Skip supported
informational responses and continue to the final response, without retaining interim headers.

## 5. Offline dialing blocks the UI task for up to 20 seconds

`ui_task()` calls `wifi_wait(20000)` directly before dialing. The same task polls touch and the BOOT
button, so a tap while the stored network is unavailable prevents it from handling pairing/reset
holds or further taps until the wait finishes. Use a nonblocking pending-dial state or move the
network wait out of the UI loop. Confirmed by code paths; board reproduction remains to be done.

## Existing test baseline

Before these review reproductions, the current host suites passed: discovery 1,221 checks, HTTP
263, UDP 747, TCP tunnel 1,162 (the count varies with scheduling), and 19 Python tests. The runs
reported deprecated `sprintf` warnings in third-party cJSON and unclosed-socket ResourceWarnings
in the Python simulator tests. Passing these suites does not cover the five cases above.
