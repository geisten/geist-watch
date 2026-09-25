# geist-watch

Tiny local vision agent.

Tell a camera what to watch for. Receive an event when the observed state changes.
No cloud inference. No video upload.

**Status: the model-free core exists; nothing looks at a camera yet.**

The first milestone is a Raspberry Pi and USB camera detecting a newly arrived
package in a configured doorway region. A second scenario detects a door that
has remained visibly open for five minutes.

What is implemented today is the part that needs no hardware: the temporal
core that turns a stream of `yes`/`no`/`unknown` observations into confirmed
states and events, and the versioned JSONL those events are written as.

```sh
make check              # model-free C tests plus the header contract
make MODE=asan check    # the same, under ASan/UBSan
make bench              # replay the scenes, report TP/FP/FN and availability
```

No engine, no model, no camera, no network. The core does not allocate and
never reads the clock — time is an argument on every call, which is what lets
the five-minute door rule be tested at its 299/300-second boundary instead of
waiting. `include/geist_watch.h` is the whole API.

`make bench` scores recorded scenes against ground truth — one-to-one window
matching, TP/FP/FN, Wilson intervals, and an availability figure that keeps
blind time in the denominator (`benchmarks/README.md`). The scenes shipped
today are synthetic placeholders so the harness runs before any footage
exists; they are not evidence about detection quality.

Capture, the model adapter, the CLI, packaging and the Raspberry Pi
performance harness belong to later work packages and are not present yet.
Small-model support and Pi performance must be demonstrated before the release
model is chosen; the engine is not pinned until then.

The implementation uses C23 and GNU Make, following geist-memory and
geist-diktat.

See [PLAN.md](PLAN.md) for the implementation plan, architecture, milestones,
acceptance criteria and proposed repository structure.

Repository: [geisten/geist-watch](https://github.com/geisten/geist-watch).

Proposed license for implementation: Apache-2.0, matching the sibling projects.
