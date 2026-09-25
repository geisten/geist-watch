# W03 — putting real footage through a small VLM

The first measurement the plan asks for: short package and negative sequences
through a small vision-language model on the Pi, reporting peak RSS,
observation latency and failure cases — graded against criteria frozen before
any of it was measured.

Everything here runs from the **W03 (Pi 5, manual)** workflow. Results land in
the measurement-log issue.

## 1. Record the footage

A few short sequences of the entrance area, taken with the camera where it
will actually stand. For the first feasibility run a handful is enough:

| sequence | what happens |
| --- | --- |
| empty | nobody, nothing, a minute or two |
| delivery | empty, then a package is set down and left |
| pickup | a package lies there, then is taken away |
| passer-by | someone walks through carrying a package, and leaves with it |
| bag | a bag or a jacket is put down instead of a package |

Frames at a steady rate — one every few seconds is plenty; the plan's
semantic sampling is every 5–10 s. JPEG or PNG.

**The footage stays on the Pi.** It is never committed, uploaded or posted:
the workflow reads frames in place and records only their paths. This
repository is public, and a doorstep is somebody's home.

## 2. Label it

One directory per run, with the frames and one `.scene` file per sequence —
the same format as `benchmarks/scenes/`, except that every `sample` line names
a real frame and its label is what **you** see in that frame:

```
footage/
├── delivery-1.scene
├── empty-1.scene
└── frames/
    ├── delivery-1-0000.jpg
    └── …
```

```
version 1
scene delivery-1
split test
camera cam-front
match_window 45000
rule doorstep appeared yes 3 10000 15000
truth doorstep package_appeared 40000
sample 0     frames/delivery-1-0000.jpg doorstep=no
sample 5000  frames/delivery-1-0005.jpg doorstep=no
…
sample 40000 frames/delivery-1-0040.jpg doorstep=yes
```

- `split test` for held-out sequences, `split dev` for the ones prompts and
  thresholds may be tuned on. Precision and recall are reported on **test
  only** — scoring the tuning set would let the tuning count as evidence.
- `truth` marks when a package first became visible. Negative sequences have
  no `truth` line: any event there is a false alarm.
- The rule id must have a question in `questions.txt` (`doorstep` and
  `front-door` do).

A sample without a frame (`-`) is refused: that is a synthetic label, and a
model cannot be scored on evidence it never saw.

## 3. Pin the model, once

Run the workflow with **mode `list`** — it posts the files of
`ggml-org/SmolVLM-500M-Instruct-GGUF` and the commit `main` resolves to.
Choose the text model and its `mmproj` file, then run **mode `pin`** with those
two names in `files`. It downloads them on the Pi, measures them, and posts a
manifest. That manifest goes into `tools/models/smolvlm-500m.model` through a
pull request — reviewed, not copied blindly.

## 4. Measure

Run **mode `measure`** with `footage` set to the absolute path of the directory
on the Pi. The workflow installs the pinned model (verified, no network),
builds the pinned llama.cpp from `reference`, runs every labelled frame, and
posts the report with its grade.

## What the numbers are — and are not

- **Cold starts.** Every observation is a fresh process, so latency includes
  model load. It is an upper bound; a resident model is a separate, warm
  measurement.
- **Full frame.** The model sees the whole image; cropping to the rule's
  region is W06. The questions in `questions.txt` are written for that and
  carry their own `prompt_id`.
- **Criteria this run cannot measure** — a 24-hour negative soak, operation
  with networking disabled — appear as NOT MEASURED. That is the honest grade
  of a first feasibility run, not a failure of it.
