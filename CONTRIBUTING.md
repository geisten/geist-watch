# Contributing to geist-watch

## From clone to green tests

```sh
make check              # model-free C tests plus the header contract
make MODE=asan check    # the same, under ASan/UBSan
make format-check
```

No engine, no model, no camera, and no network. That is a property worth
protecting: the temporal core is where the product's correctness claims
live, and it stays testable on any machine precisely because it knows
nothing about inference.

The supported compilers are GCC 14+ and Clang 19+ (C23). GCC 13 and
Clang 18 build the same tree but only answer to the older spelling of the
standard, so on a distribution compiler use `make CSTD=c2x check`. CI
gates the real floor.

## What the core may not do

`libgeist_watch.a` must not allocate, read the clock, touch the
filesystem, or link anything. Every entry point takes its buffers and its
notion of "now" from the caller.

This is not minimalism for its own sake. The release criteria ask for a
299-versus-300-second boundary test on the door rule; that is only
answerable in a test suite because the clock is an argument. The day the
core calls `clock_gettime` is the day those tests have to sleep for five
minutes, and the day they start getting skipped.

## Rules for rules

- `unknown` is a value, never an error. A covered lens, a stale frame and
  a malformed model reply are all `GW_UNKNOWN`, and a rule must treat it
  as "evidence stopped", never as "nothing changed".
- Evidence expires. A confirmed state older than `max_gap_ns` decays to
  `GW_UNKNOWN` on the next tick, because an unchanging scene and a dead
  camera look identical from the inside.
- One frame is one observation. Re-evaluating a held frame returns
  `GW_E_DUPLICATE` rather than counting twice.
- Prefer refusing to guessing. Where the two conflict, the release
  criteria put precision above recall, and a missed event inside an
  observation gap is a documented sampling limit rather than a bug.

## Changing the event schema

`GW_EVENT_SCHEMA_VERSION` goes up on any change to the field set, and the
change is recorded in `CHANGELOG.md`. Consumers are expected to refuse a
version they do not know rather than guess at it.

## Style

`make format` (clang-format, config in `.clang-format`). Warnings are
errors: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wstrict-prototypes -Wimplicit-fallthrough -Wvla -Werror`.

Comments explain why, not what. The plan this project is built from makes
a lot of deliberate, non-obvious choices; where the code implements one,
say which and why, so the next reader does not "simplify" it away.
