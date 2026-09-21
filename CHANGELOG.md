# Changelog

All notable changes to this project are recorded here. The event wire
format is versioned separately by `GW_EVENT_SCHEMA_VERSION`.

## Unreleased

### Added

- Build and CI foundation (plan work package W01): `Makefile`,
  `mk/config.mk` with `TARGET`/`MODE`/`CSTD`, configuration-derived build
  directories, strict warnings as errors, and a model-free CI matrix over
  Linux x86-64/ARM64 and macOS ARM64 plus a `pi5` cross-compile.
- Model-free temporal core (plan work package W05): `libgeist_watch.a`
  with `gw_init`, `gw_add_rule`, `gw_observe`, `gw_tick` and
  `gw_get_state`. Tri-state observations, confirmation by count *and*
  time window, evidence expiry, and two rule kinds — `GW_RULE_APPEARED`
  (a package arrives) and `GW_RULE_SUSTAINED` (a door stays open).
- Versioned JSONL event rendering (`gw_event_render`) at schema
  version 1, into caller-provided buffers.
- Replay and scoring harness (plan work package W02): a line-oriented
  scene manifest with ground truth, `gw_replay`, and `gw_score` with
  one-to-one window matching, TP/FP/FN, Wilson intervals on precision
  and recall, and availability that keeps unknown time in the
  denominator. `make bench` replays every scene against the v0.1 floors.
- Three synthetic scenes under `benchmarks/scenes/`, so the harness runs
  before any footage exists. They are not evidence about detection
  quality; `benchmarks/README.md` says so and says what replaces them.

### Notes

- The core does not allocate, does not read the clock, and does not touch
  the filesystem. Time is an argument, which is what lets the door rule's
  299/300-second boundary be tested without waiting five minutes.
- Capture, the model adapter, the CLI, packaging and the Pi benchmark
  harness belong to later work packages and are deliberately absent
  rather than stubbed.
