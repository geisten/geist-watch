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

### Notes

- The core does not allocate, does not read the clock, and does not touch
  the filesystem. Time is an argument, which is what lets the door rule's
  299/300-second boundary be tested without waiting five minutes.
- Capture, the model adapter, the CLI, packaging and the Pi benchmark
  harness belong to later work packages and are deliberately absent
  rather than stubbed.
