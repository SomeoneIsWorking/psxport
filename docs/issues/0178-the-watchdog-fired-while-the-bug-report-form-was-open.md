# 0178: the watchdog fired while the bug-report form was open

Status: fixed (2026-10-23). Owner: `runtime/psx/platform/watchdog.*`.

## Observed
Opening the in-app bug report (the B key) and leaving the form up aborted the process with the watchdog's
`STUCK: no frame presented` backtrace after ~3 s, so a report could not be written.

## Cause
`BugReportSession::report` calls `Watchdog::suspend()` before `holdForm` and `resume()` after, but `suspend`
was a one-shot `alarm(0)`: it kept the process armed. The form still presents (the debug-server idle loop
repaints it), and a present calls `Watchdog::progress()` / `Watchdog::mainPresentComplete()`, which re-armed
the steady 3 s alarm. With no further main present while the player typed, the alarm fired on a deliberately
idle process.

## Proper fix
`Watchdog::suspend()` sets a `suspended` state and cancels the alarm; `progress()` and `mainPresentComplete()`
do nothing while it is set; `resume()` clears it and re-arms. A present during an intentional idle no longer
re-arms the timeout. The debug-server pause and the REPL stdin wait, which used the same pair, are unaffected.
