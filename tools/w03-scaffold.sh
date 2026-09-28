#!/bin/sh
# w03-scaffold.sh — write the skeleton of a W03 scene for footage you took.
#
#   tools/w03-scaffold.sh [--split test|dev] [--rule doorstep|front-door]
#                         [--every <s>] [--camera <id>]
#                         <footage-dir> <scene-name> <source>
#
# <source> is either a directory of frames (*.jpg, *.jpeg, *.png, taken in
# file-name order at a steady rate) or a video file, from which ffmpeg takes
# one frame every --every seconds into <footage-dir>/frames/.
#
# The result is <footage-dir>/<scene-name>.scene with one `sample` line per
# frame and every label set to `?`. It is a skeleton, not a labelling: the
# labels are what a PERSON sees in each frame, and nothing here can know
# that. Replace each `?` with yes, no or unknown and, for a sequence in which
# a package appears, add the `truth` line. tools/w03-run.sh refuses a scene
# that still holds a `?`, so an unlabelled skeleton cannot be measured by
# accident.
#
# Frames stay where they are. Only their paths go into the scene: relative
# to <footage-dir> when they lie inside it, absolute otherwise.
set -eu

PROG=$(basename "$0")
die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 2; }

SPLIT=test; RULE=doorstep; EVERY=5; CAMERA=cam-front
while [ $# -gt 0 ]; do
    case "$1" in
        --split)  [ $# -ge 2 ] || die "--split needs test or dev"; SPLIT=$2; shift 2 ;;
        --rule)   [ $# -ge 2 ] || die "--rule needs an id"; RULE=$2; shift 2 ;;
        --every)  [ $# -ge 2 ] || die "--every needs seconds"; EVERY=$2; shift 2 ;;
        --camera) [ $# -ge 2 ] || die "--camera needs an id"; CAMERA=$2; shift 2 ;;
        --) shift; break ;;
        -*) die "unknown option $1" ;;
        *) break ;;
    esac
done
[ $# -eq 3 ] || die "usage: $PROG [options] <footage-dir> <scene-name> <source>"
DIR=$1; NAME=$2; SRC=$3

case "$SPLIT" in test|dev) ;; *) die "--split must be test or dev" ;; esac
case "$EVERY" in ''|*[!0-9]*|0) die "--every must be a whole number of seconds > 0" ;; esac
case "$NAME" in ''|*[!A-Za-z0-9._-]*) die "scene name may use only letters, digits, '.', '_' and '-'" ;; esac
case "$CAMERA" in ''|*[!A-Za-z0-9._-]*) die "camera id may use only letters, digits, '.', '_' and '-'" ;; esac

# The rule lines are the product's defaults (src/gw_rules.c), so a scene
# measures the confirmation logic that would actually ship.
case "$RULE" in
    doorstep)
        rule_line="rule doorstep appeared yes 3 10000 15000"
        truth_hint="truth doorstep package_appeared <t_ms of the first frame showing it>"
        count=3; window_ms=10000; gap_ms=15000 ;;
    front-door)
        rule_line="rule front-door sustained yes 2 2000 15000 300000"
        truth_hint="truth front-door door_open_sustained <t_ms the door was open long enough>"
        count=2; window_ms=2000; gap_ms=15000 ;;
    *) die "--rule must be doorstep or front-door (the rules benchmarks/w03/questions.txt asks about)" ;;
esac

# A rule that needs `count` agreeing samples inside `window_ms` cannot fire
# when samples are further apart than that allows. Footage sampled that
# sparsely would measure the sampling, not the model — say so now rather than
# after an hour on the board.
every_ms=$((EVERY * 1000))
[ $(((count - 1) * every_ms)) -le "$window_ms" ] \
    || die "one frame every ${EVERY} s is too sparse for rule $RULE: it needs $count samples within $((window_ms / 1000)) s"
[ "$every_ms" -le "$gap_ms" ] \
    || die "one frame every ${EVERY} s exceeds rule $RULE's gap of $((gap_ms / 1000)) s"

[ -d "$DIR" ] || die "no footage directory at $DIR"
DIR=$(cd "$DIR" && pwd)
OUT="$DIR/$NAME.scene"
[ ! -e "$OUT" ] || die "$OUT exists; a labelled scene is not overwritten"

LIST=$(mktemp)
trap 'rm -f "$LIST"' EXIT

if [ -d "$SRC" ]; then
    SRC=$(cd "$SRC" && pwd)
    LC_ALL=C find "$SRC" -maxdepth 1 -type f \
        \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' \) | LC_ALL=C sort > "$LIST"
elif [ -f "$SRC" ]; then
    command -v ffmpeg >/dev/null || die "ffmpeg is needed to take frames from a video (apt install ffmpeg)"
    mkdir -p "$DIR/frames"
    set -- "$DIR/frames/$NAME"-*
    [ ! -e "$1" ] || die "frames for $NAME already exist under $DIR/frames"
    # -nostdin: ffmpeg otherwise reads the terminal for commands and can
    # stall or swallow input when run from a script.
    ffmpeg -nostdin -loglevel error -i "$SRC" -vf "fps=1/$EVERY" \
        "$DIR/frames/$NAME-%04d.jpg" || die "ffmpeg could not read $SRC"
    LC_ALL=C find "$DIR/frames" -maxdepth 1 -type f -name "$NAME-*.jpg" | LC_ALL=C sort > "$LIST"
else
    die "no frames directory or video at $SRC"
fi

[ -s "$LIST" ] || die "no frames found in $SRC"
# The scene format is split on whitespace; a path containing any would be
# read as several tokens.
if grep -q '[[:space:]]' "$LIST"; then
    die "frame paths must not contain whitespace: $(grep -m1 '[[:space:]]' "$LIST")"
fi

{
    printf '%s\n' \
        "# Skeleton from tools/w03-scaffold.sh — LABEL IT BEFORE MEASURING." \
        "# Replace every '$RULE=?' with what you see in that frame: yes, no, or" \
        "# unknown if a person could not tell either. If a package (or an open" \
        "# door) appears in this sequence, uncomment the truth line and give the" \
        "# t_ms at which it first became visible. Negative sequences have none." \
        "# One frame every ${EVERY} s is assumed; adjust t_ms if that is not so." \
        "version 1" \
        "scene $NAME" \
        "split $SPLIT" \
        "camera $CAMERA" \
        "match_window 45000" \
        "$rule_line" \
        "# $truth_hint"
    awk -v dir="$DIR/" -v every="$every_ms" -v rule="$RULE" '{
        path = $0
        if (index(path, dir) == 1) path = substr(path, length(dir) + 1)
        printf "sample %d %s %s=?\n", (NR - 1) * every, path, rule
    }' "$LIST"
} > "$OUT"

n=$(wc -l < "$LIST" | tr -d ' ')
printf '%s: %s frames -> %s (every label is ?, label them next)\n' "$PROG" "$n" "$OUT" >&2
