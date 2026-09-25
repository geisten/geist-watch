#!/bin/sh
# w03-run.sh — put real footage through a small VLM and grade what comes out.
#
#   tools/w03-run.sh --scenes <dir> --out <dir> [--questions <file>]
#                    [--package-rule <id>]
#
# A scene manifest under --scenes is the same format the replay harness reads
# (benchmarks/scenes/), with one difference that matters: every `sample` line
# names a real frame, and its labels are a HUMAN's ground truth for that
# frame. This script asks the model the same question for every labelled
# (frame, rule), writes a copy of the scene with the model's answers in place
# of the human's, replays that copy through the same temporal core, and
# scores the result against the scene's `truth` events.
#
# So one run yields both levels the plan asks about:
#
#   frame level   does the model see what a person sees? (frame_agreement,
#                 invalid_outputs)
#   event level   do its answers, fed through the confirmation logic, produce
#                 the right events? (precision, recall on held-out scenes)
#
# and the costs: per-observation latency and peak RSS.
#
# The report it writes is graded by report_check against the frozen v0.1
# criteria. Criteria this run cannot measure — a 24-hour soak, an offline
# check — are simply absent from the report, and are therefore graded
# NOT MEASURED rather than passed.
#
# Footage never leaves the machine: frames are read where they lie and
# nothing about them but a path is written anywhere. The plan's locality
# promise covers test material too.
#
# Required environment (the workflow knows these; guessing them would put
# plausible fiction into a report's provenance):
#   GW_REPLAY          replay binary
#   GW_MODEL_ID        e.g. smolvlm-500m
#   GW_MODEL_QUANT     quantisation of every component, e.g. "text=Q8_0 mmproj=Q8_0"
#   GW_ENGINE_COMMIT   inference engine revision
#   GW_COMPILER        compiler the engine was built with
#   GW_COMPILER_FLAGS  flags it was built with
# plus GW_VLM_* as tools/vlm-observe.sh documents. Optional overrides, for
# tests: GW_MODEL_SHA256, GW_RESOLUTION.
set -eu

PROG=$(basename "$0")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 2; }

SCENES=; OUT=; QUESTIONS="$ROOT/benchmarks/w03/questions.txt"; PKG_RULE=doorstep
while [ $# -gt 0 ]; do
    case "$1" in
        --scenes)       [ $# -ge 2 ] || die "--scenes needs a directory"; SCENES=$2; shift 2 ;;
        --out)          [ $# -ge 2 ] || die "--out needs a directory"; OUT=$2; shift 2 ;;
        --questions)    [ $# -ge 2 ] || die "--questions needs a file"; QUESTIONS=$2; shift 2 ;;
        --package-rule) [ $# -ge 2 ] || die "--package-rule needs an id"; PKG_RULE=$2; shift 2 ;;
        *) die "unknown argument $1" ;;
    esac
done
[ -n "$SCENES" ] && [ -d "$SCENES" ] || die "--scenes must name a directory of *.scene files"
[ -n "$OUT" ] || die "--out is required"
[ -f "$QUESTIONS" ] || die "no questions file at $QUESTIONS"

for v in GW_REPLAY GW_MODEL_ID GW_MODEL_QUANT GW_ENGINE_COMMIT GW_COMPILER GW_COMPILER_FLAGS; do
    eval "val=\${$v:-}"
    [ -n "$val" ] || die "$v must be set: it goes into the report's provenance"
done
[ -x "$GW_REPLAY" ] || die "GW_REPLAY is not executable: $GW_REPLAY"

set -- "$SCENES"/*.scene
[ -e "$1" ] || die "no *.scene files under $SCENES"

mkdir -p "$OUT/scenes"
OBS="$OUT/observations.tsv"
printf 'scene\tt_ms\trule\ttruth\tanswer\tms\trss_kb\tvalid\tframe\n' > "$OBS"

prompt_id=$(awk '$1 == "prompt_id" { print $2; exit }' "$QUESTIONS")
[ -n "$prompt_id" ] || die "$QUESTIONS declares no prompt_id"

question_for() {
    awk -v id="$1" '$1 == id { $1 = ""; sub(/^ +/, ""); print; exit }' "$QUESTIONS"
}

first_frame=

# Word-splitting the manifest lines is intended; glob-expanding them against
# whatever files sit in the working directory is not.
set -f
for scene in "$@"; do
    name=$(basename "$scene" .scene)
    dir=$(cd "$(dirname "$scene")" && pwd)
    out_scene="$OUT/scenes/$name.scene"
    : > "$out_scene"
    printf '  %s\n' "$name" >&2

    while IFS= read -r line || [ -n "$line" ]; do
        set -- $line
        if [ "${1:-}" != "sample" ]; then
            printf '%s\n' "$line" >> "$out_scene"
            continue
        fi
        t_ms=$2; frame=$3; shift 3
        # A sample without a frame is a synthetic label, and replacing it
        # with a model answer is impossible. Mixing the two would score the
        # model on evidence it never saw.
        [ "$frame" != "-" ] || die "$name: sample at $t_ms ms has no frame; W03 needs real footage, not the synthetic scenes"
        case "$frame" in /*) path=$frame ;; *) path="$dir/$frame" ;; esac
        [ -f "$path" ] || die "$name: frame $frame not found"
        [ -n "$first_frame" ] || first_frame=$path

        new="sample $t_ms $frame"
        for label in "$@"; do
            rule=${label%%=*}
            truth=${label#*=}
            q=$(question_for "$rule")
            [ -n "$q" ] || die "$name: no question for rule '$rule' in $QUESTIONS"
            # stdin from /dev/null: this runs inside a loop reading the scene
            # file, and a model process that touched stdin would swallow the
            # remaining samples without a word.
            set -- $(sh "$ROOT/tools/vlm-observe.sh" "$path" "$q" < /dev/null)
            answer=$1; ms=$2; rss=$3; valid=$4
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "$name" "$t_ms" "$rule" "$truth" "$answer" "$ms" "$rss" "$valid" "$frame" >> "$OBS"
            new="$new $rule=$answer"
        done
        printf '%s\n' "$new" >> "$out_scene"
    done < "$scene"
done

set +f
"$GW_REPLAY" --json "$OUT"/scenes/*.scene > "$OUT/replay.jsonl"

# --- aggregate ---------------------------------------------------------

n=$(awk 'NR > 1' "$OBS" | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || die "no observations were made"
invalid=$(awk -F'\t' 'NR > 1 && $8 == 0' "$OBS" | wc -l | tr -d ' ')
agree=$(awk -F'\t' 'NR > 1 && $4 == $5' "$OBS" | wc -l | tr -d ' ')

# Nearest-rank percentiles: the value at position ceil(p*n). No
# interpolation, so every reported latency is one that actually happened.
pct() {
    awk -F'\t' 'NR > 1 { print $6 }' "$OBS" | sort -n \
        | awk -v p="$1" '{ v[NR] = $1 } END { i = int(p * NR); if (i < p * NR) i++; if (i < 1) i = 1; print v[i] }'
}

# Held-out scenes only: dev scenes are what thresholds and prompts get tuned
# on, and scoring them too would let the tuning count as evidence.
sum_field() {
    grep "\"rule\":\"$PKG_RULE\"" "$OUT/replay.jsonl" | grep '"split":"test"' \
        | sed "s/.*\"$1\":\([0-9]*\).*/\1/" | awk '{ s += $1 } END { print s + 0 }'
}
tp=$(sum_field tp); fp=$(sum_field fp); fn=$(sum_field fn)

model_sha=${GW_MODEL_SHA256:-}
if [ -z "$model_sha" ]; then
    [ -f "${GW_VLM_MODEL:-}" ] || die "GW_VLM_MODEL must name the model file so it can be hashed"
    model_sha=$( (sha256sum "$GW_VLM_MODEL" 2>/dev/null || shasum -a 256 "$GW_VLM_MODEL") | cut -d' ' -f1)
fi

resolution=${GW_RESOLUTION:-}
if [ -z "$resolution" ]; then
    desc=$(file -b "$first_frame" 2>/dev/null || true)
    resolution=$(printf '%s' "$desc" | sed -n 's/.*[^0-9]\([0-9][0-9]*\) *x *\([0-9][0-9]*\).*/\1x\2/p')
    [ -n "$resolution" ] || die "cannot read the frame resolution from '$desc'; set GW_RESOLUTION"
fi

os=$( (. /etc/os-release 2>/dev/null && printf '%s' "$PRETTY_NAME") || uname -s)
hardware=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || uname -m)

REPORT="$OUT/run.report"
{
    printf '# Produced by tools/w03-run.sh. Frames were read in place and never copied.\n'
    printf 'version 1\n'
    printf 'model_id            %s\n' "$GW_MODEL_ID"
    printf 'model_sha256        %s\n' "$model_sha"
    printf 'model_quantization  %s\n' "$GW_MODEL_QUANT"
    printf 'engine_commit       %s\n' "$GW_ENGINE_COMMIT"
    printf 'compiler            %s\n' "$GW_COMPILER"
    printf 'compiler_flags      %s\n' "$GW_COMPILER_FLAGS"
    printf 'os                  %s %s\n' "$os" "$(uname -m)"
    printf 'hardware            %s\n' "$hardware"
    printf 'source              %s\n' "$(basename "$(cd "$SCENES" && pwd)")"
    printf 'roi                 full-frame\n'
    printf 'resolution          %s\n' "$resolution"
    printf 'sampling            per scene manifest, %s observations\n' "$n"
    printf 'prompt_id           %s\n' "$prompt_id"
    printf 'max_tokens          3\n'
    printf 'start               cold\n'
    printf '\n'
    printf 'metric observations       %s\n' "$n"
    printf 'metric invalid_outputs    %s\n' "$invalid"
    printf 'metric frame_agreement    %s\n' "$(awk -v a="$agree" -v n="$n" 'BEGIN { printf "%.4f", a / n }')"
    # Timing disabled means timing unmeasured. Writing the zeros would grade
    # as the fastest model ever tested.
    if [ "${GW_TIME:-}" != "none" ]; then
        printf 'metric peak_rss_bytes     %s\n' "$(awk -F'\t' 'NR > 1 && $7 > m { m = $7 } END { print m * 1024 }' "$OBS")"
        printf 'metric observation_p50_ms %s\n' "$(pct 0.50)"
        printf 'metric observation_p95_ms %s\n' "$(pct 0.95)"
    fi
    # A ratio with an empty denominator is not zero and not one; it is absent.
    if [ $((tp + fp)) -gt 0 ]; then
        printf 'metric package_precision  %s\n' "$(awk -v a="$tp" -v b="$fp" 'BEGIN { printf "%.4f", a / (a + b) }')"
    fi
    if [ $((tp + fn)) -gt 0 ]; then
        printf 'metric package_recall     %s\n' "$(awk -v a="$tp" -v b="$fn" 'BEGIN { printf "%.4f", a / (a + b) }')"
    fi
} > "$REPORT"

printf '\n%s observations, %s invalid, %s agree with the labels; held-out %s: TP %s FP %s FN %s\n' \
    "$n" "$invalid" "$agree" "$PKG_RULE" "$tp" "$fp" "$fn" >&2
printf 'report: %s\n' "$REPORT" >&2
