#!/bin/sh
# W03 harness: footage in, graded report out — with a stand-in model.
#
# The real model only runs on the board. What CAN be tested anywhere is
# everything around it, and that is where a measurement quietly goes wrong:
# a sample swallowed by a subprocess reading stdin, an invalid reply counted
# as an answer, a disabled timer reporting zeros that grade as fast, a
# synthetic scene scored as if a model had looked at it. Each case below is
# one of those.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUN="$ROOT/tools/w03-run.sh"
: "${GW_REPLAY:?run via make check: GW_REPLAY names the replay binary}"
: "${GW_REPORT_CHECK:?run via make check: GW_REPORT_CHECK names report_check}"

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fail=0
bad() { printf '  FAIL: %s\n' "$*" >&2; fail=1; }

# A stand-in model: the answer is written in the frame's file name, and a
# "junk" frame gets a chatty reply that is not one of the three words. It
# also reads stdin on purpose — the harness must not let that eat samples.
cat > "$W/stub.sh" <<'EOF'
#!/bin/sh
cat > /dev/null
case "$(basename "$1")" in
    yes_*)  echo "Yes" ;;
    no_*)   echo "no" ;;
    junk_*) echo "Well, it could be a package." ;;
esac
EOF
chmod +x "$W/stub.sh"

F="$W/footage"
mkdir -p "$F/frames"
for f in no_a no_b no_c yes_d yes_e yes_f junk_g yes_h yes_l yes_m no_i no_j no_k; do
    printf 'frame' > "$F/frames/$f.jpg"
done

# Held-out scene: three empty frames, then three with a package. One truth
# event. The model's answers match the labels exactly.
cat > "$F/test-1.scene" <<'EOF'
version 1
scene test-1
split test
camera cam
match_window 45000
rule doorstep appeared yes 3 10000 15000
truth doorstep package_appeared 15000
sample 0     frames/no_a.jpg  doorstep=no
sample 5000  frames/no_b.jpg  doorstep=no
sample 10000 frames/no_c.jpg  doorstep=no
sample 15000 frames/yes_d.jpg doorstep=yes
sample 20000 frames/yes_e.jpg doorstep=yes
sample 25000 frames/yes_f.jpg doorstep=yes
EOF

# Dev scene with one junk frame whose human label is `yes`: the model's
# invalid reply must become `unknown`, count as invalid, and disagree.
#
# It also fires with no truth event to answer, so the dev scene holds a false
# positive. Precision on held-out scenes stays 1.0 only if dev scenes are
# really excluded — without this, a harness that scored every scene would
# pass the same test.
cat > "$F/dev-1.scene" <<'EOF'
version 1
scene dev-1
split dev
camera cam
match_window 45000
rule doorstep appeared yes 3 10000 15000
sample 0     frames/no_i.jpg   doorstep=no
sample 5000  frames/no_j.jpg   doorstep=no
sample 10000 frames/no_k.jpg   doorstep=no
sample 15000 frames/junk_g.jpg doorstep=yes
sample 20000 frames/yes_h.jpg  doorstep=yes
sample 25000 frames/yes_l.jpg  doorstep=yes
sample 30000 frames/yes_m.jpg  doorstep=yes
EOF

export GW_VLM_CMD="$W/stub.sh" GW_TIME=none
export GW_MODEL_ID=stub GW_MODEL_QUANT="text=none mmproj=none" GW_ENGINE_COMMIT=stub
export GW_COMPILER=stub GW_COMPILER_FLAGS=none GW_MODEL_SHA256=stub GW_RESOLUTION=1x1

sh "$RUN" --scenes "$F" --out "$W/out" 2>/dev/null || bad "the harness failed on valid footage"
R="$W/out/run.report"
metric() { awk -v k="$1" '$1 == "metric" && $2 == k { print $3 }' "$R"; }

# Every labelled sample was observed. A subprocess eating stdin would have
# taken the rest of the scene with it, and the count would be short.
[ "$(metric observations)" = 13 ] || bad "expected 13 observations, got '$(metric observations)'"

[ "$(metric invalid_outputs)" = 1 ] || bad "the chatty reply should count as one invalid output"
[ "$(metric frame_agreement)" = "0.9231" ] || bad "12 of 13 should agree, got '$(metric frame_agreement)'"
grep -q 'doorstep=unknown' "$W/out/scenes/dev-1.scene" \
    || bad "the invalid reply must enter the replay as unknown"
grep '"scene":"dev-1"' "$W/out/replay.jsonl" | grep -q '"fp":1' \
    || bad "the dev scene should hold one false positive, or the exclusion test proves nothing"

# Held-out only: the dev scene has no truth events and must not dilute these.
[ "$(metric package_precision)" = "1.0000" ] || bad "precision should be 1.0 on the held-out scene"
[ "$(metric package_recall)" = "1.0000" ] || bad "recall should be 1.0 on the held-out scene"

# Timing was disabled, so timing is unmeasured — not zero, and above all not
# fast. The grader must say so.
[ -z "$(metric observation_p95_ms)" ] || bad "latency must be absent when timing is disabled"
[ -z "$(metric peak_rss_bytes)" ] || bad "RSS must be absent when timing is disabled"
out=$("$GW_REPORT_CHECK" --criteria "$ROOT/benchmarks/criteria/v0.1.criteria" "$R" || true)
printf '%s' "$out" | grep -q 'NOT MEASURED observation_p95_ms' \
    || bad "report_check should grade latency NOT MEASURED"
printf '%s' "$out" | grep -q 'PASS         package_precision' \
    || bad "report_check should grade the measured precision"

grep -q '^prompt_id           w03-fullframe-v1$' "$R" || bad "the prompt_id must come from the questions file"
grep -q '^start               cold$' "$R" || bad "per-frame processes are cold starts, and the report must say so"

# The synthetic scenes have no frames. Scoring a model on them would be
# scoring evidence it never saw.
out=$(sh "$RUN" --scenes "$ROOT/benchmarks/scenes" --out "$W/synthetic" 2>&1 || true)
printf '%s' "$out" | grep -q 'needs real footage' || bad "synthetic scenes must be refused, got: $out"

# Provenance is required from the caller, not invented.
out=$(env -u GW_ENGINE_COMMIT sh "$RUN" --scenes "$F" --out "$W/noprov" 2>&1 || true)
printf '%s' "$out" | grep -q 'GW_ENGINE_COMMIT must be set' || bad "missing provenance must be refused, got: $out"

# A rule the questions file does not cover is an error, not a silent skip.
sed 's/doorstep/mailbox/g' "$F/test-1.scene" > "$W/mailbox.scene"
mkdir -p "$W/mb" && cp "$W/mailbox.scene" "$W/mb/" && cp -r "$F/frames" "$W/mb/"
out=$(sh "$RUN" --scenes "$W/mb" --out "$W/mbout" 2>&1 || true)
printf '%s' "$out" | grep -q "no question for rule 'mailbox'" || bad "an unknown rule must be refused, got: $out"

# --- the request file behind the push trigger ---------------------------

REQ="$ROOT/tools/w03-request.sh"
req() { sh "$REQ" "$@" 2>&1; }

# The template on main must itself be a valid request.
out=$(req "$ROOT/benchmarks/w03/request") || bad "the committed request template does not parse: $out"
printf '%s' "$out" | grep -qx 'MODE=list' || bad "the template should request list"

printf 'mode pin\nfiles a.gguf mmproj-a.gguf\nhf_revision abc123\n' > "$W/r1"
out=$(req "$W/r1") || bad "a valid pin request was refused: $out"
printf '%s' "$out" | grep -qx 'FILES=a.gguf mmproj-a.gguf' || bad "files must pass through whole, spaces included"
printf '%s' "$out" | grep -qx 'MODEL=smolvlm-500m' || bad "omitted keys must take the form's defaults"
printf '%s' "$out" | grep -qx 'ISSUE=9' || bad "the default issue is the measurement log"

printf 'mode list\nmodle x\n' > "$W/r2"
req "$W/r2" | grep -q "unknown key 'modle'" || bad "a misspelled key must be refused, not ignored"
printf '# nothing\n' > "$W/r3"
req "$W/r3" | grep -q 'no mode given' || bad "a request without a mode must be refused"
printf 'mode everything\n' > "$W/r4"
req "$W/r4" | grep -q 'mode must be list, pin or measure' || bad "an unknown mode must be refused"
printf 'mode list\nissue nine\n' > "$W/r5"
req "$W/r5" | grep -q 'issue must be a number' || bad "a non-numeric issue must be refused"

# The one that guards the channel: GITHUB_ENV is read line by line, so a
# value with a newline in it would set a variable of the caller's choosing.
out=$(IN_MODE=list IN_FOOTAGE="$(printf '/x\nLD_PRELOAD=/tmp/evil.so')" sh "$REQ" --from-env 2>&1 || true)
printf '%s' "$out" | grep -q 'line break' || bad "a value with a newline must be refused, got: $out"
printf '%s' "$out" | grep -q 'LD_PRELOAD' && bad "the injected variable reached the output"

out=$(IN_MODE=measure IN_FOOTAGE=/home/pi/footage sh "$REQ" --from-env) || bad "a valid dispatch was refused"
printf '%s' "$out" | grep -qx 'FOOTAGE=/home/pi/footage' || bad "dispatch values must pass through"

exit $fail
