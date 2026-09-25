# Benchmark harness

```sh
make bench                                   # every scene, against the v0.1 floors
build/.../replay --json benchmarks/scenes/*.scene
```

Replays recorded scenes through the temporal core and reports what it
found, so a published number can be re-derived instead of trusted.

## Status of the numbers here

**The ten scenes in `scenes/` are synthetic and hand-written** — three
positive, seven negative. They exist so the harness is exercisable before
any footage does, and so the core's precision-favouring decisions are
pinned as observable facts rather than prose. They are not evidence about
detection quality, and no result from them belongs in a release claim.

Real scenes replace them once the data collection in W02 runs. The
release criteria ask for at least 50 positive placement sequences across
several scenes plus 24 hours of negative footage, with development and
test material separated by day and camera position — not by neighbouring
frames of one recording.

## What a scene is

A plain text manifest, readable by the person who authored the ground
truth. `src/gw_bench.h` has the grammar; the short version:

```
version      1
scene        doorstep-01
split        dev | test
camera       cam-front
match_window <after_ms> [<before_ms>]
tick_ms      <ms>
rule         <id> appeared|sustained yes|no <count> <window_ms> <gap_ms> [<sustain_ms>]
truth        <rule_id> <event_type> <at_ms> [<after_ms>]
restart      <t_ms>
sample       <t_ms> <frame|-> <rule_id>=<yes|no|unknown> ...
```

A scene with no `truth` lines is a **negative** scene: the correct
outcome is that nothing is emitted. `restart` re-initialises the watch
mid-scene — a process restart is the one sequence in the release criteria
that cannot be expressed as a label, because nothing the camera sees says
the process died.

Each `sample` carries a label per rule. Those labels stand in for what a
VLM will answer once one is chosen, which is what makes the harness run
today with no engine and no model. The `frame` column is the path that
model will be handed, carried through untouched until then — a scene
authored now stays valid when the model arrives.

The parser is strict. A misspelled rule id in a `sample` line would
silently drop that rule's labels and report a recall of zero that looks
like a detection failure, so every unrecognised token is an error with a
line number instead.

## How a run is scored

**Matching is one-to-one.** Each detection takes the earliest
still-unmatched ground-truth event whose window contains it. One
detection cannot answer two events, and a second detection inside one
window is a false alarm rather than a second success.

The matcher is greedy in time order rather than optimal-bipartite. That
is a deliberate trade: greedy is explainable from the output, the two
agree whenever events are further apart than the match window — which is
every scene the plan describes — and where they differ greedy scores no
higher, so it cannot flatter a run.

**Match windows are fixed before the run.** They live in the manifest
next to the ground truth, which is the point of writing them down. Tuning
a window against results is how a benchmark stops measuring anything.

**A detection before its event does not count.** Confirmation
legitimately lags a placement; anticipating one is not a detection.

**Unknown phases stay in the denominator.** Availability is the fraction
of scene time in which the rule held a confirmed value, weighted by
duration rather than sample count, and it is reported next to precision
and recall. A run that spent half the scene blind and got the visible
half right is not a perfect run, and the report says so.

**Point estimates travel with an interval.** Precision and recall carry
95 % Wilson score intervals. On fifty sequences the difference between
92 % and 98 % is noise; a bare percentage hides that, and the normal
approximation would put a perfect 12/12 at `[1.0, 1.0]`, claiming a
certainty twelve samples cannot support.

## The replay ticks on a schedule, not on frames

The harness walks the scene on a fixed `tick_ms` cadence and delivers
samples as their timestamps come due, rather than ticking once per
sample.

This is not an implementation detail. Ticking only when a frame arrives
means the core is never asked about time in which no frame arrived, so
evidence expiry can never fire and a camera outage becomes invisible —
the first frame after the gap lands before the tick that should have
noticed it. The product's scheduler runs whether or not capture
delivered, and a replay that does otherwise measures a system nobody
ships. `tick_ms` is refused if it is coarser than any rule's `max_gap`,
since a gap could then pass between two ticks unseen.

## Floors

`make bench` applies `--min-precision 0.95 --min-recall 0.90`, the v0.1
targets from the plan's release criteria. Those are **targets, not
measurements**: the plan fixes a hardware profile after the first
feasibility run and freezes thresholds before the final test. A floor
applies only to a rule that has ground truth in that scene — a rule with
no truth events is not evidence either way.

## What the scenes cover, and what they cannot

The release criteria list the sequences a scene set must contain. They
split into two kinds, and the difference decides what a synthetic label
is worth.

**The label pattern IS the scenario.** These are answerable today,
because what makes them a test is the shape of the observation stream,
not the model's visual judgment:

| Sequence | Scene | Pinned behaviour |
| :-- | :-- | :-- |
| parcel present at start | `doorstep-test` | initial state, not a delivery |
| placement, pickup, second placement | `doorstep-dev` | one event each, re-armed only by confirmed absence |
| person carrying a parcel past | `doorstep-passerby` | two positive frames confirm nothing |
| occlusion | `doorstep-test` | no delivery claimed on return |
| camera failure | `doorstep-camera-shift` | evidence expires, availability collapses |
| camera knocked out of alignment | `doorstep-camera-shift` | unanswerable region stays unknown |
| process restart | `doorstep-restart` | no armed state survives it |
| door: brief opening | `door-brief-opening` | duration, not transition |
| door: 299/300 s boundary | `door-dev` | fires at the threshold, not before |
| door: closes in between | `door-interrupted` | 500 s of open door, no unbroken 300 |
| door: missing frames | `door-missing-frames` | elapsed time is not confirmed time |
| door: process restart | `doorstep-restart` (same mechanism) | no duration carried across |

Each negative scene was checked by mutation: remove the thing it guards
— the interruption, the restart, the frame gap — and it emits the event
it otherwise withholds. A negative scene that passes because nothing
could ever have fired is not a test.

**The label IS the model's judgment.** These cannot be answered by a
synthetic scene, and pretending otherwise would be asserting the
conclusion:

| Sequence | Status |
| :-- | :-- |
| bag instead of a parcel | open — whether the model says yes is the question |
| shadow, backlight, dusk | partially: `doorstep-flicker` pins that an unstable answer produces no event. Whether hard light actually destabilises the answer, or produces a confident wrong one, only footage settles |

Labelling a bag `yes` here would report a false positive that measures a
model failure invented by the author of the scene; labelling it `no`
would test nothing at all. Both belong to W03 and real material.

## Clock jumps

The core refuses time that moves backwards (`GW_E_TIME`), and a manifest
requires samples in time order, so a backwards jump is not expressible as
a scene. It is covered in `tests/test_state.c` instead.

## What is not here yet

Inference latency, peak RSS, throttling and power come from running a
real model on real hardware (W03/W04), and the plan is explicit that
tokens per second alone is not a benchmark report. This harness scores
event quality; the performance harness is a separate milestone.
