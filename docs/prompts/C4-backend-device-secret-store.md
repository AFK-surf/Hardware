# C4 · Backend phase 2b: a device secret store for LAN codecs

For the Comma backend team (AFK-surf/Cue, `systems/`). Follows C3 phase 2a (#2701 UDP, #2702 miIO codec + `device.miio_request`, #2703 miio-lights skill). In 2a the miIO token is a plain tool argument: it sits in the transcript, and the user must paste it again in every new conversation.

## Why now

Today the user's Mi lamp token lives in a file on their Mac (`~/token_extractor/.lamp_token`). The agent "remembers" it only because it runs Python through the Mac Connector and reads that file. With the Mac off, which is the whole point of the T-Circle gateway, the agent has no token unless the user pastes it into chat again, where it then stays in the transcript. Before this reaches anyone outside the team, device keys need a proper home.

## Scope

Store per-workspace (or per-group; pick what matches existing credential ownership) **device secrets** for local-network devices:
- Mi/miIO tokens (32 hex chars), one per device;
- later: Gree keys, Tuya local keys, Hue app keys. Keep the schema generic: `kind` + `name` + secret value + a little non-secret metadata (device host or IP, model, created_at, last_used_at).

**Before adding an entity, follow `AGENTS.md` → Domain concepts.** First check whether an existing credential store fits: OAuth credentials, MCP credentials, connector secrets. Prefer extending one over a new entity. If a new one is needed, update `docs/architecture/DOMAIN_CONCEPTS.md` in the same change, with the reasoning.

## Do

1. **Storage:** encrypted at rest, scoped to the workspace or group, never returned to the model or to clients after it's written (write-only, like other credentials). Deleting a workspace or group deletes its secrets.
2. **Agent tools:**
   - `device.secret_save {kind, name, value, host?, model?}`. The user gives a token once in chat; the agent saves it. Its result must not echo the value. Mark it write-safety.
   - `device.secret_list` → names, kinds, hosts, last_used. **No values.**
   - `device.secret_delete {name}`.
3. **Use secrets by reference:** `device.miio_request` takes `token_ref` (the secret name) instead of, or alongside, `token`. The value is resolved at dispatch time inside Salix and never enters the transcript. Keep the plain `token` argument only for internal testing behind the existing "INTERNAL TESTING ONLY" note, or remove it if the team prefers.
4. **Skills:** update `miio-lights` (and `home-gateway`):
   - when a Mi device needs a token, look for a saved secret first;
   - if there isn't one, ask the user once, save it, and use the reference from then on;
   - never repeat the token back.
5. **User visibility:** at least a backend API to list and delete secrets: `GET` / `DELETE /v1/comma/workspaces/:ws/device-secrets`. The client UI comes later. The Android app will add a section under Settings › Devices.
6. **Security review** per the repo's rules. Answer: who can read or write; how the agent can't leak values (tool results, logs, IFC classification of the resolved request); what happens on workspace export or deletion.
7. **Tests:** save → list (no value) → `miio_request` with `token_ref` works through a fake gateway; the transcript and tool results never contain the value; delete → a later request fails with a clear "saved token not found" error.

## Out of scope

- Fetching tokens automatically from a Xiaomi account (a "connect Xiaomi account" flow). Possibly later.
- Client UI.

## End-to-end check (with the user, after deploy)

1. The Mac's Comma Staging is quit, so its Connector is off.
2. In a new conversation the user says "关掉床头灯" and gives the token once; the agent saves it and turns the lamp off through the T-Circle.
3. In another new conversation, "打开床头灯" works **without** giving the token again, and the transcript shows no token anywhere.
