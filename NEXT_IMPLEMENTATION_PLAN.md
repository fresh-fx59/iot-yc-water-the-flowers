# Next Implementation Plan

## Boot Catch-up Safety Follow-up

- Add a native regression test that verifies boot catch-up waters only the overdue trays when multiple trays are overdue.
- Add a native regression test that verifies a single overdue tray on boot does not expand into watering all trays.
- Refactor boot catch-up selection to avoid scanning overdue trays twice in the startup path.
- Review the Telegram notification retry queue for duplicate-message behavior when Telegram/proxy accepts a message but the ESP32 does not receive the success response.

## Notes

- These items are follow-up work after the `v1.19.6` boot catch-up safety fix.
- The goal is to improve verification and reduce misleading Telegram behavior without changing the safety fix intent.
