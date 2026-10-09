# TP-LINK TL-IPC45AW · RTSP flow, captured on the office LAN (2026-10-09)

Captured from a Mac on the office network (192.168.5.13) with a ~150-line Python client (`DESCRIBE → SETUP → PLAY → first IDR → TEARDOWN`, TCP interleaved, Digest). This is real camera behaviour for the backend's RTSP client tests (G3, #3098). `response=` digests are redacted; the password never appears.

## What the camera does (things a client must handle)

1. **401 carries two `WWW-Authenticate` headers, Basic first, Digest second.** Pick the Digest one by scheme; don't take the first header. Digest has **no `qop`**: `response = MD5(HA1:nonce:HA2)`, `HA1 = MD5(admin:TP-LINK IP-Camera:password)`, `HA2 = MD5(METHOD:uri)`.
2. **`uri` in Authorization must equal the request-line URI exactly**, including `:554`. It changes per request: DESCRIBE / PLAY / TEARDOWN use the aggregate URL `rtsp://192.168.5.33:554/stream2`; SETUP uses the track URL `…/stream2/track1`.
3. The nonce from the first 401 is reused for the whole session (all later requests 200 with it).
4. `Content-Base` ends with `/` (`rtsp://…/stream2/`); the track control is relative (`track1`).
5. `Session: 6E362053;timeout=15`: strip `;timeout=…` before echoing the session id. Keep-alive needed only if the grab took > 15 s.
6. The SDP has **three** media: video H.264 (track1), audio PCMA (track2), `application/TP-LINK` metadata (track3). Only SETUP track1.
7. **Interleaved RTP frames (`$`, ch, len) can arrive mixed with RTSP replies**: after PLAY, the TEARDOWN reply came behind RTP frames. A reply parser must skip `$` frames instead of assuming text.
8. `/stream2` (sub stream) is **H.264** `profile-level-id=64001E`, 640×360, `packetization-mode=1`, SPS/PPS both in `sprop-parameter-sets` **and** in-band (NAL 7, 8). The IDR comes as **FU-A (NAL 28)** fragments.
9. `/stream2` timing: **first complete IDR 0.16 s after PLAY, 32 packets, ~42 KB** of RTP. Annex-B (SPS+PPS+IDR) = 42 087 bytes; ffmpeg decodes it to a 640×360 JPEG of ~25 KB.
10. **`/stream1` (main) is H.265/HEVC**, not H.264 (`sprop-vps/sps/pps`, NAL types 0/2/4 → HEVC FU type 49 needed), and runs at **~2.5 MB per 10 s (~250 KB/s ≈ 2 Mbps)**, which is the T-Circle tunnel's measured ceiling (249 KB/s). In 10 s my H.264-only parser found no IDR. **Use `/stream2` by default**; `/stream1` needs an HEVC depacketizer and would saturate the gateway.

## Transcript (`/stream2`)

```
C: DESCRIBE rtsp://192.168.5.33:554/stream2 RTSP/1.0
   CSeq: 1
   User-Agent: comma-probe
   Accept: application/sdp

S: RTSP/1.0 401 Unauthorized
   CSeq: 1
   Date: Fri, Oct 09 2026 04:07:39 GMT
   WWW-Authenticate: Basic realm="TP-LINK IP-Camera"
   WWW-Authenticate: Digest realm="TP-LINK IP-Camera", nonce="8baee1d58631d65b0729a247a7da8465"

C: DESCRIBE rtsp://192.168.5.33:554/stream2 RTSP/1.0
   CSeq: 2
   User-Agent: comma-probe
   Accept: application/sdp
   Authorization: Digest username="admin", realm="TP-LINK IP-Camera", nonce="8baee1d58631d65b0729a247a7da8465", uri="rtsp://192.168.5.33:554/stream2", response="<md5>"

S: RTSP/1.0 200 OK
   CSeq: 2
   Date: Fri, Oct 09 2026 04:07:39 GMT
   Content-Base: rtsp://192.168.5.33:554/stream2/
   Content-Type: application/sdp
   Content-Length: 489

v=0
   o=- 14665860 31787219 1 IN IP4 192.168.5.33
   s=Session streamed by "TP-LINK RTSP Server"
   t=0 0
   m=video 0 RTP/AVP 96
   c=IN IP4 0.0.0.0
   b=AS:4096
   a=range:npt=0-
   a=control:track1
   a=rtpmap:96 H264/90000
   a=fmtp:96 packetization-mode=1; profile-level-id=64001E; sprop-parameter-sets=Z2QAHqwsaoKAv+WbgICAoAAAAwAgAAADA9CA,aO4xshs=
   m=audio 0 RTP/AVP 8
   a=rtpmap:8 PCMA/8000
   a=control:track2
   m=application/TP-LINK 0 RTP/AVP smart/1/90000
   a=rtpmap:95 TP-LINK/90000
   a=control:track3
   

C: SETUP rtsp://192.168.5.33:554/stream2/track1 RTSP/1.0
   CSeq: 3
   User-Agent: comma-probe
   Transport: RTP/AVP/TCP;unicast;interleaved=0-1
   Authorization: Digest username="admin", realm="TP-LINK IP-Camera", nonce="8baee1d58631d65b0729a247a7da8465", uri="rtsp://192.168.5.33:554/stream2/track1", response="<md5>"

S: RTSP/1.0 200 OK
   CSeq: 3
   Date: Fri, Oct 09 2026 04:07:39 GMT
   Transport: RTP/AVP/TCP;unicast;interleaved=0-1
   Session: 6E362053;timeout=15

C: PLAY rtsp://192.168.5.33:554/stream2 RTSP/1.0
   CSeq: 4
   User-Agent: comma-probe
   Session: 6E362053
   Range: npt=0.000-
   Authorization: Digest username="admin", realm="TP-LINK IP-Camera", nonce="8baee1d58631d65b0729a247a7da8465", uri="rtsp://192.168.5.33:554/stream2", response="<md5>"

S: RTSP/1.0 200 OK
   CSeq: 4
   Date: Fri, Oct 09 2026 04:07:39 GMT
   Range: npt=0.000-
   Session: 6E362053
   RTP-Info: url=rtsp://192.168.5.33:554/stream2/track1;seq=1;rtptime=0

(note: 0 interleaved RTP frames arrived BEFORE the PLAY reply)

C: TEARDOWN rtsp://192.168.5.33:554/stream2 RTSP/1.0
   CSeq: 5
   User-Agent: comma-probe
   Session: 6E362053
   Authorization: Digest username="admin", realm="TP-LINK IP-Camera", nonce="8baee1d58631d65b0729a247a7da8465", uri="rtsp://192.168.5.33:554/stream2", response="<md5>"

S: RTSP/1.0 200 OK
   CSeq: 5
   Date: Fri, Oct 09 2026 04:07:39 GMT

--- RTP: 32 interleaved packets, 42478 bytes, 0.16s from PLAY to first full IDR; NAL types seen {7: 1, 8: 1, 28: 30}; IDR 42002 bytes; Annex-B out 42087 bytes; sprop in SDP: True
```
