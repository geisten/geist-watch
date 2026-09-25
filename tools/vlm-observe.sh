#!/bin/sh
# vlm-observe.sh — one frame, one question, one tri-state answer.
#
#   tools/vlm-observe.sh <image> <question>
#
# Prints exactly one line:  <answer> <elapsed_ms> <peak_rss_kb> <valid>
#
#   answer       yes | no | unknown
#   elapsed_ms   wall time of the whole inference process
#   peak_rss_kb  its maximum resident set size
#   valid        1 if the model's reply was one of the three words, 0 if it
#                was anything else and has been recorded as `unknown`
#
# The plan's observation contract, applied literally: the model may say yes,
# no or unknown and nothing else. Where the backend supports it the output is
# constrained by a grammar, and the reply is STILL parsed strictly, because
# "schema conformance proves no correct recognition" — and because a grammar
# the backend silently ignored would otherwise go unnoticed. Anything else
# becomes `unknown` with valid=0, so an invalid reply reduces evidence rather
# than inventing it, and the count of such replies is itself a measurement.
#
# Every call is a fresh process, so every number is a COLD number: model load
# included. That is an upper bound on latency, labelled as such in the report
# (start=cold). A resident model via llama-server is the warm measurement,
# and a separate one.
#
# Environment:
#   GW_VLM_CLI      llama-mtmd-cli binary          (required unless GW_VLM_CMD)
#   GW_VLM_MODEL    text model GGUF                 (required unless GW_VLM_CMD)
#   GW_VLM_MMPROJ   multimodal projector GGUF       (required unless GW_VLM_CMD)
#   GW_VLM_THREADS  threads, default: all cores
#   GW_VLM_CMD      a stand-in command, called as `cmd <image> <question>`,
#                   whose stdout is taken as the model's reply. Tests only.
#   GW_TIME         set to `none` to skip timing (ms and RSS report 0). Tests
#                   only: GNU time is not on every platform the suite runs on,
#                   and the Pi job checks for it explicitly.
set -eu

PROG=$(basename "$0")
die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 2; }

# The instruction is part of the measured configuration: changing it changes
# the prompt_id in the report, not silently the numbers.
instruction() { printf '%s Answer with exactly one word: yes, no, or unknown.' "$1"; }
GRAMMAR='root ::= "yes" | "no" | "unknown"'

# --raw: run the model once and print its reply, nothing else. The timed path
# below re-invokes this script in --raw mode under GNU time, so there is ONE
# construction of the model command rather than two copies that can drift.
if [ "${1:-}" = "--raw" ]; then
    shift
    if [ -n "${GW_VLM_CMD:-}" ]; then
        # shellcheck disable=SC2086  # a stand-in may carry its own arguments
        exec $GW_VLM_CMD "$1" "$2"
    fi
    [ -x "${GW_VLM_CLI:-}" ]    || die "GW_VLM_CLI must name the llama-mtmd-cli binary"
    [ -f "${GW_VLM_MODEL:-}" ]  || die "GW_VLM_MODEL must name the text model"
    [ -f "${GW_VLM_MMPROJ:-}" ] || die "GW_VLM_MMPROJ must name the projector"
    exec "$GW_VLM_CLI" -m "$GW_VLM_MODEL" --mmproj "$GW_VLM_MMPROJ" \
        --image "$1" -p "$(instruction "$2")" \
        --grammar "$GRAMMAR" --temp 0 -n 3 \
        -t "${GW_VLM_THREADS:-$(nproc 2>/dev/null || echo 4)}" 2>/dev/null
fi

[ $# -eq 2 ] || die "usage: $PROG <image> <question>"
IMAGE=$1
QUESTION=$2
[ -f "$IMAGE" ] || die "no image at $IMAGE"

OUT=$(mktemp)
TIMING=$(mktemp)
trap 'rm -f "$OUT" "$TIMING"' EXIT

if [ "${GW_TIME:-}" = "none" ]; then
    "$0" --raw "$IMAGE" "$QUESTION" > "$OUT" || true
    ms=0
    rss=0
else
    # GNU time only: BSD time has no -f, and a timing that silently measured
    # nothing would put zeros into a report as if they were fast.
    /usr/bin/time -f '%e %M' -o "$TIMING" true 2>/dev/null \
        || die "GNU time (/usr/bin/time -f) is required to measure latency and RSS"
    /usr/bin/time -f '%e %M' -o "$TIMING" "$0" --raw "$IMAGE" "$QUESTION" > "$OUT" || true
    # The LAST line: when the model process exits non-zero, GNU time writes
    # "Command exited with non-zero status N" above the format line. A crash
    # is an observation too — recorded as unknown, with its real cost.
    ms=$(tail -n 1 "$TIMING" | awk '{printf "%d", $1 * 1000 + 0.5}')
    rss=$(tail -n 1 "$TIMING" | awk '{print $2}')
fi

# Strict: the whole reply, trimmed and lowercased, must be one of three words.
# Whitespace is squeezed to single spaces FIRST and trimmed after: trimming
# line by line and then joining would turn a trailing newline into a trailing
# space, and a correct "yes" would be recorded as an invalid reply.
reply=$(tr '[:upper:]' '[:lower:]' < "$OUT" | tr -s '[:space:]' ' ' | sed 's/^ //;s/ $//')
case "$reply" in
    yes|no|unknown) printf '%s %s %s 1\n' "$reply" "$ms" "$rss" ;;
    *)              printf 'unknown %s %s 0\n' "$ms" "$rss" ;;
esac
