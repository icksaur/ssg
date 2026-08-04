# Spec: fix the flaky `theCapabilitiesReportReflectsWhatTheTerminalAnswered`

## Problem

`test_terminal_parity`'s `theCapabilitiesReportReflectsWhatTheTerminalAnswered`
is intermittently flaky (reproduced at ~30% failure with 10 concurrent copies
on a 20-core host -- i.e. with NO CPU contention, ruling out scheduling
starvation). When it fails, the failing assertion is line 681
(`output.find("<ESC>[?62;4;52c")`, the escaped DA1 reply in the report), while
the capability assertions above it (line 676, `clipboard_write yes`) PASS.

Root cause -- a read-then-assert ordering race in the TEST harness, not the app.
The test's read loop breaks as soon as it sees the marker `"clipboard_write"`
(test_terminal_parity.cpp:668):

    if (output.find("clipboard_write") != std::string::npos) break;

But `reportCapabilities()` prints the report in order: the capability lines
(`color_depth`, `synchronized_output`, `keyboard_protocol`, `clipboard_write`)
FIRST, and only THEN the `replies (N):` section that contains the escaped DA1
reply the test asserts on at lines 681-682, followed last by the `SSG_TERM_`
override block. So the loop can break the instant `clipboard_write` lands,
before the later `replies:` section has been read() into `output`. After the
break the test reaps the child and asserts -- and whether the replies section is
present depends purely on whether it happened to arrive in the same read()
chunk as `clipboard_write`. That is the race: the app is correct and always
prints the section; the test simply asserts on output it has not finished
reading.

That the capability line (`clipboard_write yes`, printed earlier) passes while
the later `replies:` line fails is the signature of this ordering bug, and is
why it is independent of load.

## Goal

The test consumes the app's COMPLETE `--capabilities` report before asserting on
any part of it, so no assertion can race output that has not been read. Fix the
flake at its root (the harness read loop), not by widening a timeout.

## Design

Drain the pty until the child closes it (EOF) rather than breaking on a
mid-report marker. With a non-blocking master fd, ::read returns 0 at EOF and
-1 with errno on error. EAGAIN/EWOULDBLOCK ("no data right now") and EINTR (a
signal interrupted the read) are transient: keep polling. On Linux a pty master
read after the slave closes reports -1/EIO rather than 0, so the loop treats a 0
return OR any other (non-transient) error as end-of-output. The loop
distinguishes them: EOF/non-transient-error ends the outer loop; a transient
EAGAIN/EINTR just continues polling. The overall wall-clock deadline stays as
the safety net for a child that never exits.

Concretely, in `theCapabilitiesReportReflectsWhatTheTerminalAnswered`:
- Remove the `if (output.find("clipboard_write") ...) break;` early exit.
- Track EOF: when ::read returns 0, set a flag and stop the outer loop.
- Keep answering the probe when \x1b[c is first seen (unchanged), and keep the
  deadline as the upper bound.

This makes the full report -- including the trailing `replies:` and `SSG_TERM_`
sections -- present in `output` before any assertion runs. It is a pure test
change; `reportCapabilities()` and `TerminalCapabilities` are untouched.

### Audit the sibling real-binary tests

`theFirstFrameIsWrittenBeforeAnyReplyIsRead` and `realBinaryOutputMatchesRender*`
are deliberately time/marker-bounded for a different reason (observing an
ordering or a specific frame) and do not assert on content printed after their
break marker; they are left as-is. Only the capabilities test asserts on a
section printed after the marker it breaks on.

## Oracles / tests

- The fix is validated by the flake disappearing: re-run the reproduction (10
  concurrent copies, several rounds) and observe 0 failures where the current
  code fails ~30% of runs. This is a harness determinism fix, so the "oracle" is
  the now-deterministic existing assertions (lines 674-686) passing reliably.
- No new production behavior to cover; adding a bespoke unit test for a pty
  read loop would test the test. The existing assertions, now fed the complete
  report, are the coverage.

## Plan

1. This spec supersedes the misdiagnosed probe-window spec; quick re-review of
   the corrected diagnosis; fold.
2. Rewrite the read loop in
   `theCapabilitiesReportReflectsWhatTheTerminalAnswered` to drain to EOF.
3. Verify: 10x concurrent reproduction, multiple rounds, 0 failures; then the
   full gate.
4. Code review; fold; merge.
