#!/bin/sh
# w03-request.sh — turn a W03 request into the variables the workflow uses.
#
#   tools/w03-request.sh <request-file>     read a request file (push trigger)
#   tools/w03-request.sh --from-env         read IN_* variables (manual dispatch)
#
# Prints KEY=value lines for $GITHUB_ENV:
#
#   MODE FOOTAGE MODEL HF_REPO HF_REVISION FILES LICENSE_OVERRIDE ISSUE
#
# Both triggers go through this one script, so a run started by a request
# file and a run started from the Actions form cannot be read differently.
#
# One rule is enforced here rather than in the workflow's validation step,
# because it guards the channel the values travel through: no value may
# contain a line break. $GITHUB_ENV is parsed line by line, and a value with
# a newline in it would set a second variable of the caller's choosing — on
# a private machine, LD_PRELOAD would do. Everything else (repo names,
# paths, file names) is checked by the validation step with the same rules
# for both triggers.
set -eu

PROG=$(basename "$0")
die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 2; }

mode=; footage=; model=smolvlm-500m
hf_repo=ggml-org/SmolVLM-500M-Instruct-GGUF; hf_revision=main
files=; license=; issue=9

if [ "${1:-}" = "--from-env" ]; then
    mode=${IN_MODE:-}; footage=${IN_FOOTAGE:-}; model=${IN_MODEL:-$model}
    hf_repo=${IN_HF_REPO:-$hf_repo}; hf_revision=${IN_HF_REVISION:-$hf_revision}
    files=${IN_FILES:-}; license=${IN_LICENSE:-}; issue=${IN_ISSUE:-$issue}
else
    [ $# -eq 1 ] || die "usage: $PROG <request-file> | --from-env"
    [ -f "$1" ] || die "no request file at $1"
    n=0
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        case "$line" in ''|'#'*) continue ;; esac
        key=${line%%[ 	]*}
        val=$(printf '%s' "$line" | sed "s/^$key[ 	]*//")
        case "$key" in
            mode)        mode=$val ;;
            footage)     footage=$val ;;
            model)       model=$val ;;
            hf_repo)     hf_repo=$val ;;
            hf_revision) hf_revision=$val ;;
            files)       files=$val ;;
            license)     license=$val ;;
            issue)       issue=$val ;;
            *) die "line $n: unknown key '$key'" ;;
        esac
    done < "$1"
fi

case "$mode" in
    list|pin|measure) ;;
    '') die "no mode given; a request must say list, pin or measure" ;;
    *)  die "mode must be list, pin or measure, got '$mode'" ;;
esac
case "$issue" in ''|*[!0-9]*) die "issue must be a number, got '$issue'" ;; esac

for v in "$mode" "$footage" "$model" "$hf_repo" "$hf_revision" "$files" "$license" "$issue"; do
    case "$v" in
        *'
'*|*"$(printf '\r')"*) die "a value contains a line break; refusing to pass it on" ;;
    esac
done

printf 'MODE=%s\nFOOTAGE=%s\nMODEL=%s\nHF_REPO=%s\nHF_REVISION=%s\nFILES=%s\nLICENSE_OVERRIDE=%s\nISSUE=%s\n' \
    "$mode" "$footage" "$model" "$hf_repo" "$hf_revision" "$files" "$license" "$issue"
