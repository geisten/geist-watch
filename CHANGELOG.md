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
- Ten synthetic scenes under `benchmarks/scenes/`, so the harness runs
  before any footage exists — three positive and seven negative, covering
  the sequences from the release criteria whose test is the shape of the
  observation stream rather than the model's visual judgment. Each
  negative scene is mutation-checked: remove what it guards and it emits
  the event it otherwise withholds. They are not evidence about detection
  quality; `benchmarks/README.md` says so, and says which sequences only
  real footage can settle.
- Rule layer (`src/gw_rule.h`, first half of plan work package W07):
  `gw_rule_compile` turns a German or English template sentence plus
  explicit parameters into a validated `gw_rule_config`, and
  `gw_region_parse`/`gw_region_pixels` handle normalised regions in
  parts per million with exact integer conversion to pixels and no
  locale dependency. Two templates today — `doorstep-package` and
  `door-open` — whose defaults are the same numbers the benchmark scenes
  use. Unrecognised sentences, negations, two templates in one sentence,
  a duration a template cannot honour, and a sentence duration that
  contradicts an explicit one are all refused with a reason rather than
  interpreted. The compiler validates its own output by handing it to
  `gw_add_rule`, so it cannot produce a configuration the core will not
  run. Not part of `libgeist_watch.a`: the library stays the model-free
  timing core.
- Explicit model setup (`tools/pin-model.sh`, `tools/fetch-model.sh`,
  `make setup`), the first piece of plan work package W03. A manifest pins
  the revision, licence, and every file's SHA-256 and byte count, including
  the vision tower and projector; installs are staged, verified in full, and
  only then renamed into place, so an interrupted run leaves the previous
  install or nothing. `--from` installs from a local directory with the same
  verification and no network, which is what makes a net-free setup possible.
  Nothing downloads implicitly: `setup` is never a dependency of a build, a
  test or a benchmark.
  No model is pinned yet — obtaining weights is a decision about a licence
  and several gigabytes, and a manifest with invented hashes would be worse
  than none, so `make setup` fails closed until `pin-model.sh` has measured
  real files.
- Benchmark report and release gate (`src/gw_report.h`, `make report-check`),
  the second piece of W03. A report carries provenance — model and engine
  hashes, quantisation of every component, compiler flags, OS, hardware, ROI,
  resolution, sampling, prompt, token limit, cold or warm start — and its
  measurements; `benchmarks/criteria/v0.1.criteria` carries the plan's
  section-9 targets, committed before any hardware number exists so they
  cannot be tuned to fit one. Grading refuses outright when provenance is
  incomplete, reports every criterion whether met or not, and counts an
  unmeasured criterion as not demonstrated rather than as a pass.
- `make format-check` now covers `src/*.h` and `benchmarks/*.c`, which it had
  been skipping.
- `restart <t_ms>` in the scene manifest, re-initialising the watch
  mid-scene. A process restart is the one sequence the criteria list that
  cannot be expressed as a label: nothing the camera sees says the
  process died.

### Notes

- The core does not allocate, does not read the clock, and does not touch
  the filesystem. Time is an argument, which is what lets the door rule's
  299/300-second boundary be tested without waiting five minutes.
- Capture, the model adapter, the CLI, packaging and the Pi benchmark
  harness belong to later work packages and are deliberately absent
  rather than stubbed.
