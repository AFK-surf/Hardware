# E2b · Android: pick the setup-offer card state by revision (fix #2740 review)

Work in the Cue repo, `clients/apps/android`, on the existing branch `codex/android-nearby-device` (PR #2740). Rebase on main first.

## Context

#2740 shows the backend's `device_setup_offer` card in the Home chat. That card says "I found a Comma device nearby (Comma-5D28)…" and has **Connect** / **Not now** buttons. Status changes arrive as hidden `device_setup_offer.updated` app events. The backend is #2731, already merged; see `systems/apps/comma_web/lib/comma_web/device_setup_offers.ex`.

Review on #2740 (Request changes, head `b3f27b269`), one blocking issue:
- The app picks the current card state by **message `seq`** (latest event wins).
- The merged server contract orders states by the **offer's `revision`**.
- A delayed older event can therefore move the card from `done` back to `started`/`open`, and show Connect again.
- A newer state on the initial card can also be overwritten by an older update.

The reviewer reproduced it.

## Do

1. Read the server contract in #2731's code: where `revision` is set and bumped, and that it's in both the card block and the `offer` of `device_setup_offer.updated`.
2. In `SetupOfferProjection` (and wherever the card state is resolved):
   - keep, per `offer_id`, the state with the **highest `revision`**;
   - when revisions are equal, keep the existing state, and don't let a lower one replace it;
   - if `revision` is missing (old data), fall back to the current `seq` rule.
3. Tests:
   - a delayed old event (lower revision) after `done` → stays `done`, no Connect;
   - the initial card carries a higher revision than a later-arriving update → the card's state wins;
   - normal progression `open` → `started` → `done`;
   - a missing revision falls back to seq.
4. Run the unit tests and lint. No phone test is needed unless you change the UI; if you do, the test phone `23112153D000003` is shared, so ask the user first.
5. Push to the same branch.
   - Reply on the review thread with what changed (no verification results in the PR text).
   - Re-request review.

## Don't

- Don't change the server.
- Don't change the scan, report or Add-device flow.
