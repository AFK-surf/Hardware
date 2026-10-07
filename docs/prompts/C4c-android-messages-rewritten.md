# C4c · Android: refresh messages the server rewrote

Work in the Cue repo, `clients/apps/android`, on a new branch off main.

## Why

Users give the Comma agent device secrets (e.g. a Xiaomi lamp token) in the Home chat. The agent saves them with `device.secret_save` (C4, #2709). Comma has a single Home conversation, so the pasted value would stay in the chat history forever.

Backend #2736 (C4b) fixes that on the server. After a successful save, it replaces the value with `[saved as <name>]` in the stored messages and sends:

```json
{"type": "conversation_changed", "change": "messages_rewritten", "message_ids": ["..."]}
```

Check the exact shape in #2736. Today the app keeps showing the old text until the history is reloaded, so the token stays on screen and in the local cache.

## Do

1. Handle `messages_rewritten`:
   - fetch those message ids again from the server;
   - replace them in the chat on screen;
   - replace them in any local store or cache (including whatever the chat restores from on the next launch).
   
   Don't reload the whole history.
2. If a listed message isn't loaded locally, ignore it: the next history load returns the rewritten version.
3. Unit tests: the event → refetch → replace, on screen and in the cache; unknown ids ignored; other `conversation_changed` kinds unaffected.
4. Verify against staging once #2736 is deployed. The test phone `23112153D000003` is shared, so ask the user before installing and check what's in the foreground first.
   - Have the agent save `bedside-lamp` again with the same value.
   - The message that held the token changes to `[saved as bedside-lamp]` without reopening the chat.

## Don't

- Don't touch the Mac or web client (later).
- Never log message text or the secret.

## Report

Draft PR as usual. Say in the description what you verified on the phone.
