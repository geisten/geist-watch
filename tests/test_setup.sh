#!/bin/sh
# Model setup: pinning, verification, atomic install.
#
# These cases are the reason the tool exists. A setup script that installs an
# unverified file, or leaves a half-written directory where a complete one
# should be, fails in a way nothing downstream can detect: the model loads,
# produces plausible output, and every number measured from it is wrong. So
# each test here is a way for that to happen, and the assertion is that it
# does not.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FETCH="$ROOT/tools/fetch-model.sh"
PIN="$ROOT/tools/pin-model.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export XDG_DATA_HOME="$WORK/data"

fail=0
check() {
    if [ "$1" = "0" ]; then :; else
        printf '  FAIL: %s\n' "$2" >&2
        fail=1
    fi
}
# Runs a command that must fail, and whose message must mention a phrase.
# "It refused" is not enough — it has to refuse for the stated reason.
refuses() {
    _want=$1; shift
    _out=$("$@" 2>&1) && { printf '  FAIL: expected refusal (%s) from: %s\n' "$_want" "$*" >&2; fail=1; return; }
    case "$_out" in
        *"$_want"*) ;;
        *) printf '  FAIL: refused with "%s", expected it to mention "%s"\n' "$_out" "$_want" >&2; fail=1 ;;
    esac
}

# --- a small model directory ------------------------------------------

SRC="$WORK/src"
mkdir -p "$SRC/tower"
printf 'decoder weights\n'      > "$SRC/model.safetensors"
printf 'vision tower weights\n' > "$SRC/tower/vision.safetensors"
printf '{"projector":true}\n'   > "$SRC/projector.json"

MAN="$WORK/demo.model"
"$PIN" --dir "$SRC" --name demo --revision abc123 --license Apache-2.0 \
       --repo Example/Demo --license-url https://example.invalid/LICENSE > "$MAN"

# The manifest must pin every file, including the tower and the projector —
# the plan names those two explicitly because they are the ones a naive
# "download the model" step forgets.
check "$( grep -c '^file ' "$MAN" | grep -qx 3 && echo 0 || echo 1 )" \
      "pin-model should record all three files (got $(grep -c '^file ' "$MAN"))"
check "$( grep -q 'tower/vision.safetensors' "$MAN" && echo 0 || echo 1 )" \
      "the vision tower must be pinned"
check "$( grep -q 'revision     abc123' "$MAN" && echo 0 || echo 1 )" \
      "the revision must be recorded"

# --- install, verify, idempotence -------------------------------------

"$FETCH" --manifest "$MAN" --from "$SRC" >/dev/null
DEST="$XDG_DATA_HOME/geist-watch/models/demo"
check "$( [ -f "$DEST/model.safetensors" ] && echo 0 || echo 1 )" "the decoder should be installed"
check "$( [ -f "$DEST/tower/vision.safetensors" ] && echo 0 || echo 1 )" "the tower should be installed"

"$FETCH" --manifest "$MAN" --verify-only >/dev/null
check "$?" "a fresh install should verify"

out=$("$FETCH" --manifest "$MAN" --from "$SRC")
case "$out" in
    *"already installed"*) ;;
    *) printf '  FAIL: a second run should be a no-op, got: %s\n' "$out" >&2; fail=1 ;;
esac

# --- tampering --------------------------------------------------------

# One byte changed in an installed file. The size still matches, so only the
# digest catches it — which is the whole reason both are recorded.
printf 'decoder weightz\n' > "$DEST/model.safetensors"
refuses "does not match the manifest" "$FETCH" --manifest "$MAN" --verify-only
refuses "exists but does not match"   "$FETCH" --manifest "$MAN" --from "$SRC"

# A tampered SOURCE must not reach the destination at all.
rm -rf "$DEST"
BAD="$WORK/bad"
cp -r "$SRC" "$BAD"
printf 'tampered\n' > "$BAD/tower/vision.safetensors"
refuses "verification failed" "$FETCH" --manifest "$MAN" --from "$BAD"
check "$( [ ! -d "$DEST" ] && echo 0 || echo 1 )" \
      "a failed install must leave nothing behind"
check "$( [ -z "$(find "$(dirname "$DEST")" -maxdepth 1 -name 'demo.staging.*' 2>/dev/null)" ] && echo 0 || echo 1 )" \
      "the staging directory must not survive a failure"

# A missing source file is caught before anything is published.
INCOMPLETE="$WORK/incomplete"
cp -r "$SRC" "$INCOMPLETE"
rm "$INCOMPLETE/projector.json"
refuses "has no projector.json" "$FETCH" --manifest "$MAN" --from "$INCOMPLETE"
check "$( [ ! -d "$DEST" ] && echo 0 || echo 1 )" \
      "an incomplete source must not produce an install"

# --- manifests that must be refused -----------------------------------

# The one that matters most: a manifest pinning nothing would install
# nothing and report success.
printf 'version      1\nname         empty\nrevision     r1\nlicense      X\n' > "$WORK/empty.model"
refuses "pins no files" "$FETCH" --manifest "$WORK/empty.model" --from "$SRC"

printf 'version      1\nname         n\nlicense      X\nfile  %064d  1  a\n' 0 > "$WORK/norev.model"
refuses "needs a revision" "$FETCH" --manifest "$WORK/norev.model" --from "$SRC"

printf 'version      1\nname         n\nrevision     r\nfile  %064d  1  a\n' 0 > "$WORK/nolic.model"
refuses "needs a license" "$FETCH" --manifest "$WORK/nolic.model" --from "$SRC"

printf 'version      2\nname         n\nrevision     r\nlicense      X\n' > "$WORK/ver.model"
refuses "version must be 1" "$FETCH" --manifest "$WORK/ver.model" --from "$SRC"

# A path that escapes the model directory would let a manifest write anywhere
# the user can.
printf 'version      1\nname         n\nrevision     r\nlicense      X\nfile  %064d  1  ../escape\n' 0 > "$WORK/esc.model"
refuses "must stay inside" "$FETCH" --manifest "$WORK/esc.model" --from "$SRC"

printf 'version      1\nname         n\nrevision     r\nlicense      X\nfile  /etc/passwd  1  a\n' > "$WORK/badsha.model"
refuses "64 hex digits" "$FETCH" --manifest "$WORK/badsha.model" --from "$SRC"

# --- no manifest ships pre-pinned -------------------------------------

# Nothing under tools/models/ may carry invented hashes. If a manifest is
# ever added there, this test insists it was produced by measuring files.
for m in "$ROOT"/tools/models/*.model; do
    [ -e "$m" ] || continue
    grep -q '^# Pinned by tools/pin-model.sh' "$m" || {
        printf '  FAIL: %s was not produced by pin-model.sh\n' "$m" >&2
        fail=1
    }
done

exit $fail
