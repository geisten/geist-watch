#!/bin/sh
# W03 scene skeletons: frames in, an unlabelled scene out — and the harness
# must refuse that scene until a person has labelled it.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCAF="$ROOT/tools/w03-scaffold.sh"
RUN="$ROOT/tools/w03-run.sh"
: "${GW_REPLAY:?run via make check: GW_REPLAY names the replay binary}"

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fail=0
bad() { printf '  FAIL: %s\n' "$*" >&2; fail=1; }

F="$W/footage"
mkdir -p "$F/frames"
# Out of name order on purpose, plus a file that is not a frame.
for f in d-0002.jpg d-0000.jpg d-0001.PNG; do printf 'frame' > "$F/frames/$f"; done
printf 'notes' > "$F/frames/notes.txt"

# --- a directory of frames ---------------------------------------------

sh "$SCAF" "$F" delivery-1 "$F/frames" 2>/dev/null || bad "a valid frames directory was refused"
S="$F/delivery-1.scene"
[ "$(grep -c '^sample ' "$S")" = 3 ] || bad "expected one sample per image, the .txt skipped"
grep -q '^sample 0 frames/d-0000.jpg doorstep=?$' "$S" || bad "first sample should be t=0, relative path, label ?"
grep -q '^sample 10000 frames/d-0002.jpg doorstep=?$' "$S" || bad "frames must follow file-name order at 5 s steps"
grep -q '^rule doorstep appeared yes 3 10000 15000$' "$S" || bad "the rule must be the product default"
grep -q '^split test$' "$S" || bad "test is the default split"

# Never overwrite: this file may already hold an hour of labelling.
out=$(sh "$SCAF" "$F" delivery-1 "$F/frames" 2>&1 || true)
printf '%s' "$out" | grep -q 'is not overwritten' || bad "an existing scene must not be overwritten, got: $out"

# Frames outside the footage directory keep an absolute path.
mkdir -p "$W/elsewhere"; printf 'frame' > "$W/elsewhere/x.jpg"
sh "$SCAF" --split dev "$F" outside "$W/elsewhere" 2>/dev/null || bad "frames outside the footage dir were refused"
grep -q "^sample 0 $W/elsewhere/x.jpg doorstep=?$" "$F/outside.scene" || bad "a frame outside the footage dir needs its absolute path"
grep -q '^split dev$' "$F/outside.scene" || bad "--split dev was not written"

# --- the harness refuses the skeleton ----------------------------------

# A stand-in model that must never be asked: the refusal has to come first.
cat > "$W/stub.sh" <<EOF
#!/bin/sh
touch "$W/model-was-called"; echo no
EOF
chmod +x "$W/stub.sh"
run() {
    GW_VLM_CMD="$W/stub.sh" GW_TIME=none GW_MODEL_ID=stub GW_MODEL_QUANT=none \
    GW_ENGINE_COMMIT=none GW_COMPILER=none GW_COMPILER_FLAGS=none GW_MODEL_SHA256=stub GW_RESOLUTION=1x1 \
        sh "$RUN" --scenes "$1" --out "$W/out" 2>&1
}
rm "$F/outside.scene"
out=$(run "$F" || true)
printf '%s' "$out" | grep -q 'label not yet given at .*delivery-1.scene:[0-9]*: doorstep=?' \
    || bad "an unlabelled skeleton must be refused with its position, got: $out"
[ ! -e "$W/model-was-called" ] || bad "the model ran before the labels were checked"

# Labelled, the same scene goes through.
sed -i.bak 's/doorstep=?/doorstep=no/' "$S" && rm -f "$S.bak"
run "$F" > /dev/null || bad "the labelled scene was refused: $(run "$F" || true)"

# A typo in a label is caught the same way, not scored as a disagreement.
awk '!done && sub(/doorstep=no$/, "doorstep=noo") { done = 1 } 1' "$S" > "$S.new" && mv "$S.new" "$S"
out=$(run "$F" || true)
printf '%s' "$out" | grep -q 'doorstep=noo' || bad "a misspelled label must be refused, got: $out"

# --- sampling that the rule cannot confirm ------------------------------

# front-door needs two samples within 2 s; one frame every 5 s can never
# confirm it, and a run on such footage would measure nothing.
out=$(sh "$SCAF" --rule front-door "$F" door-1 "$F/frames" 2>&1 || true)
printf '%s' "$out" | grep -q 'too sparse for rule front-door' || bad "5 s sampling must be refused for front-door, got: $out"
out=$(sh "$SCAF" --every 6 "$F" sparse "$F/frames" 2>&1 || true)
printf '%s' "$out" | grep -q 'too sparse for rule doorstep' || bad "6 s sampling must be refused for doorstep, got: $out"
sh "$SCAF" --rule front-door --every 2 "$F" door-1 "$F/frames" 2>/dev/null || bad "2 s sampling should suit front-door"
grep -q '^sample 4000 frames/d-0002.jpg front-door=?$' "$F/door-1.scene" || bad "--every 2 should step 2000 ms"

# --- a video ------------------------------------------------------------

# A stand-in ffmpeg: writes three frames to the output pattern it is given,
# and fails if it was not told to leave stdin alone.
mkdir -p "$W/bin"
cat > "$W/bin/ffmpeg" <<'EOF'
#!/bin/sh
case " $* " in *" -nostdin "*) ;; *) echo "stdin not closed" >&2; exit 1 ;; esac
for last; do :; done
for i in 1 2 3; do printf 'frame' > "$(printf "$last" "$i")"; done
EOF
chmod +x "$W/bin/ffmpeg"
PATH="$W/bin:$PATH" sh "$SCAF" "$F" clip "$W/elsewhere/x.jpg" 2>/dev/null || bad "a video source was refused"
grep -q '^sample 10000 frames/clip-0003.jpg doorstep=?$' "$F/clip.scene" || bad "video frames should land in frames/ at 5 s steps"
rm "$F/clip.scene"
out=$(PATH="$W/bin:$PATH" sh "$SCAF" "$F" clip "$W/elsewhere/x.jpg" 2>&1 || true)
printf '%s' "$out" | grep -q 'already exist' || bad "existing frames of a scene must not be overwritten, got: $out"

# --- names that would break the whitespace-split format -----------------

mkdir -p "$W/with space"; printf 'frame' > "$W/with space/a.jpg"
out=$(sh "$SCAF" "$F" spaced "$W/with space" 2>&1 || true)
printf '%s' "$out" | grep -q 'must not contain whitespace' || bad "a path with a space must be refused, got: $out"
out=$(sh "$SCAF" "$F" 'bad name' "$F/frames" 2>&1 || true)
printf '%s' "$out" | grep -q 'scene name may use only' || bad "a scene name with a space must be refused, got: $out"

exit $fail
