# C4d · Android: show the server's copy of a sent message, not the local text

Work in the Cue repo, `clients/apps/android`. Make a new branch off main. If #2737 (`codex/android-messages-rewritten`) hasn't merged yet, branch off it instead and say so in the PR.

## Why

Users give the Comma agent device secrets in the Home chat (e.g. a Xiaomi lamp token, 32 hex characters). After `device.secret_save`, the server keeps a rule that replaces that value with `[saved as <name>]` everywhere it reads messages (#2736, #2881). #2737 refreshes messages when the server sends `conversation_changed` / `messages_rewritten`.

Gap found on staging, 2026-10-08:
- 11:08:24: the user pasted a token that was **already saved**, so the rule already existed.
- The phone kept showing the raw token on screen for about 20 s, until 11:08:44.
- The backend says the `POST` response (202) already carries the message as the server stored and read it, i.e. `[saved as bedside-lamp]`.
- The app apparently keeps showing the locally typed text and doesn't take the server's version.

The CommaSync trace also showed this:
- the 11:08:27 snapshot reported `changed=0` (the phone's copy and the server's matched);
- the 11:08:41 snapshot reported `changed=1`.

So check the snapshot path too.

## Do

1. **Find where a sent message is shown.** That means the optimistic local copy, and how it's reconciled with the `POST` response and with later snapshots.
2. **Take the server's version.**
   - When the `POST` response (or the first snapshot or event that contains the message) arrives, replace the local copy's text with the server's text.
   - Update both the screen and the SQLite cache.
   - This applies whenever the two differ, not only for secrets.
3. **Check the snapshot comparison.**
   - Find out why the 11:08:27 snapshot counted `changed=0`. Was the server's copy still raw at that moment, or does the comparison skip pending or just-sent local messages?
   - Report which it is. If it's the server, describe it precisely so it can be passed to the backend.
4. **Tests:**
   - a sent message whose `POST` response text differs → the screen and the cache show the server text;
   - a snapshot with a different text for a just-sent message → it's replaced;
   - nothing changes when the texts are the same.
5. **Verify on staging.**
   - The test phone `23112153D000003` is shared. Ask the user before installing, and check the foreground first. The PIN for unlocking is given by the user.
   - **Never use a real token.** Use a fresh random 32-hex dummy:
     1. Save it once as `test-dummy` through the chat ("Test only: please save a miio_token device secret named test-dummy for host 192.168.5.250 with this value: <hex>").
     2. Paste **the same dummy again** and time how long the raw value stays on screen. The target is under ~2 s.
     3. Finally ask the agent to delete `test-dummy`.
   - Typing via adb: switch Gboard to English first (the pinyin keyboard garbles `adb input text`). Avoid parentheses in the text, because the device shell breaks on them. Check the field via `uiautomator dump` before sending.

## Don't

- Don't log message text or secrets.
- Don't mask anything on the client before sending (product decision: the agent must receive a first-time token).
- Don't touch the Mac or web clients.

## Report

Open a draft PR as usual. Don't put verification results in the PR text; tell the user in chat what you verified and what you found about the 11:08:27 snapshot.
