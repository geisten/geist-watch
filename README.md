# geist-watch

Tiny local vision agent.

Tell a camera what to watch for. Receive an event when the observed state changes.
No cloud inference. No video upload.

**Status: project planning, no implementation yet.**

The first milestone is a Raspberry Pi and USB camera detecting a newly arrived
package in a configured doorway region. A second scenario detects a door that
has remained visibly open for five minutes.

The planned implementation uses C23, GNU Make and a pinned geistlib revision,
following geist-memory and geist-diktat. Small-model support and Raspberry Pi
performance must be demonstrated before selecting the release model.

See [PLAN.md](PLAN.md) for the implementation plan, architecture, milestones,
acceptance criteria and proposed repository structure.

Repository: [geisten/geist-watch](https://github.com/geisten/geist-watch).

Proposed license for implementation: Apache-2.0, matching the sibling projects.
