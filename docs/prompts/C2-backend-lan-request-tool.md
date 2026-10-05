# C2 · Backend: `udp_request` + `device.lan_request` tool + home-gateway skill

Repo: AFK-surf/Cue, `systems/`. Read first:
- `~/Desktop/AFK/Hardware/docs/connector-plan.md` (in Chinese)
- `~/Desktop/AFK/Hardware/docs/connector-protocol.md` and `docs/reports/A1-report.md`, once they exist

## Goal

The Router agent can make a connected gateway device (first the T-Circle; any Connector should work the same way) perform a LAN HTTP or UDP request and return the result. Today the device-side `http_request` exists in the Go Connector, but no agent tool exposes it; only device-placed MCP uses it. The cloud's own `web.http_request` deliberately refuses private addresses.

## Context

- Tool registry: `apps/salix_agent/lib/salix_agent/tools.ex` (`registry/0` :210-249; order pinned for prompt cache, so append; entry shape :192-204).
- Closest example, an RPC to a connector device:
  - definition: `apps/salix_agent/lib/salix_agent/tools/dependency_installations.ex`;
  - path: `EnvDispatch.request/4` → `apps/salix_web/lib/salix_web/env_dispatch.ex:359` → ConnectorRecovery → socket.
- Also required for a new tool:
  - IFC classification: `ifc/destination.ex:460-520`. An unknown name defaults to public egress; decide the right class for LAN requests.
  - Plugin membership: `apps/salix_web/lib/salix/control/plugins.ex` (`environment-control` :127-136).
  - Read-only list: `tools.ex:149-190`, only if it applies.
- Protocol method timeouts: `apps/salix_env/lib/salix_env/protocol.ex:31-60`.
- The device-side `http_request` contract: `systems/connector/salix-connect/main.go:4283-4362`.
- Skills: `resources/salix-system-files/skills/<id>/SKILL.md`. For miniskills (`activation: per-message`, ≤2 KiB) see `docs/architecture/DOMAIN_CONCEPTS.md:547-560`.

## Do

1. **Protocol:** add the `udp_request` method (params `{host, port, payload_b64, timeout_ms, max_responses?}`, result `{responses:[{from,payload_b64}], timed_out}`) with a timeout entry. Optionally add `lan_discover` for A5. Add it to the Go Connector as well, so the Mac Connector supports it and the two stay at parity.
2. **Tool `device.lan_request`:** args are `device_id` (+ `environment_id` if needed) and either `http {method,url,headers,body,timeout_seconds,max_bytes}` or `udp {...}`.
   - Validate on the server that the destination is private or link-local. The tool exists for the LAN; refuse public IPs and point to `web.http_request` instead.
   - Make credentials usable through the existing `credential_env` placeholder mechanism (see `tools/http_request.ex:33-38`), so device keys such as a Hue app key or a miIO token never appear in the transcript.
   - Mark it a write-safety tool.
3. **Skill `home-gateway`:** tells the agent:
   - how to find a gateway device (`device.list`, its capabilities in metadata);
   - to prefer it for LAN devices;
   - to read the state back after every change;
   - to stop and say so when the gateway is offline.
4. Tests for the tool (validation, dispatch, error mapping) and the protocol entry. Update `docs/tools-integrations.md`.
5. Verify locally with A1's `mini_connector.py`, then on staging with the T-Circle (A3).

## Running in parallel with A1

A1 (the protocol spike) may still be running. If `docs/connector-protocol.md` doesn't exist yet:
- Work from the code itself. Steps 1–4 don't depend on A1.
- Use your own git worktree of Cue on a branch (`codex/lan-request-tool`), not the main checkout.
- Don't start a second local dev stack while A1's is up: ports and the shared `@cue-dev` app data collide. Run unit tests only. Do step 5 after A1 reports, or coordinate with the user.
- Once A1's report lands, re-check your protocol entry against it.

## Decided after A1 (2026-10-05)

- **Encrypted LAN protocols are encoded in the backend with Elixir `:crypto`.** There is no always-available cloud Python (A1 report, "Cloud compute for miIO"). Add a codec option to `device.lan_request` (or a sibling tool); start with `miio` (MD5 + AES-128-CBC, UDP 54321: hello handshake, then encrypted JSON-RPC). The device only sends and receives bytes. Keep the codec boundary generic, because Gree (AES-ECB) and Tuya local come next. Which commands to send and what to read back stays in skills.
- **Static device secrets need a store.** `credential_env` resolves OAuth credentials only. There is nowhere to keep a Hue app key or a miIO device token. Add a per-group secret store that tools can reference by name, so tokens never enter the transcript. A4 (Mi light end to end) depends on it.
- A1 findings that affect the tool: the server does not check device access for requests from device-placed MCP, so the device-side gate is the only boundary (`docs/reports/A1-report.md`). Decide whether `device.lan_request` should also check the device's reported access state on the server. Recommended, as defence in depth.

## Notes

- Open a PR. Don't merge without review.
