# F2 · Android: "Call ended" rows must not stay pinned below newer messages

Work in the Cue repo, `clients/apps/android`, on a new branch off main (use a separate git worktree; another agent works in the main checkout). Read `clients/apps/android/AGENTS.md` first.

## Problem

Seen on the phone, 2026-10-08:
- The user made four voice calls in a row without typing anything in between.
- Then the agent posted a proactive message with a `device_setup_offer` card.
- The new message and card appeared **above** the four "Call ended" rows, which stayed pinned to the bottom of the chat.

### Cause

- "Call ended" rows exist only on the phone. They live in SharedPreferences `voice_call_log` (`voice/ui/CallLog.kt:21-46`).
- `CallLog.withRecords` (`voice/ui/CallLog.kt:62-85`) merges them into the timeline at render time (`chat/live/LiveChatScreen.kt:308,323`).
- A record is placed before a row only when that row opens a **user** turn (`role == USER && cardKind == null`).
- Every record not placed yet goes at the very end (`CallLog.kt:82-83`).
- Agent messages that arrive on their own (proactive messages, setup-offer cards, task notices) never flush the pending records.

Two related issues:
- `LiveTimeline.kt:175` uses `createdAt ?: 0L`, so a row without `created_at` can never flush a record.
- The comparison mixes the phone's wall clock (`endedAtMillis`) with the server's `created_at`.

## Do

1. In `CallLog.withRecords`, flush pending records before **any row that starts a new turn** whose `createdAt` is later than the call's end, whatever its role. Use the turn boundary (e.g. `turnId` changes), so a call never splits one agent reply's own rows (text + card + actions).
2. Rows with no `createdAt` don't flush, but they don't push records below newer rows either. Decide sensibly and say how.
3. Tests in `CallLogTest`:
   - records [200, 210] and rows `USER@100, AGENT@300, AGENT(card, same turn)@301` → both calls sit between USER@100 and AGENT@300, and the card stays with its message;
   - existing case: a user message after a call;
   - a call with no later rows stays at the end (before LIVE_STATUS);
   - an agent reply's rows are never split.
4. Run all unit tests and lint.
5. Optional phone check:
   - The test phone `23112153D000003` is shared, so ask the user before installing and check the foreground first.
   - Make a short call, hang up, then make the agent post something without typing (or just compare against the existing call rows in Home).

## Don't

- Don't change how call records are stored, or the call screen.

## Report

Open a draft PR as usual. Don't put verification results in the PR text (AGENTS.md); tell the user in chat.
