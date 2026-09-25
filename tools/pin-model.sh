#!/bin/sh
# pin-model.sh — turn a directory of weights into a manifest.
#
# The authoring half of the setup. It records what is actually on disk —
# every file, its SHA-256 and its byte count — so that fetch-model.sh can
# later refuse anything that differs. It deliberately does NOT download:
# obtaining the weights the first time is a human decision about a licence
# and several gigabytes, and folding it into a script would hide both.
#
# So the workflow is: obtain the model once, by whatever means the model card
# documents; point this at the directory; commit the manifest. From then on
# every install anywhere is verified against it.
#
#   tools/pin-model.sh --dir <path> --name <n> --revision <rev> \
#                      --license <id> [--repo <r>] [--license-url <u>] \
#                      [--base-url <u>] > tools/models/<n>.model
#
# Emitting to stdout is on purpose: a manifest is reviewed before it is
# trusted, and a tool that writes it in place invites trusting it unread.
set -eu

PROG=$(basename "$0")
DIR=; NAME=; REVISION=; LICENSE=; REPO=; LICENSE_URL=; BASE_URL=

die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --dir)         [ $# -ge 2 ] || die "--dir needs a value"; DIR=$2; shift 2 ;;
        --name)        [ $# -ge 2 ] || die "--name needs a value"; NAME=$2; shift 2 ;;
        --revision)    [ $# -ge 2 ] || die "--revision needs a value"; REVISION=$2; shift 2 ;;
        --license)     [ $# -ge 2 ] || die "--license needs a value"; LICENSE=$2; shift 2 ;;
        --repo)        [ $# -ge 2 ] || die "--repo needs a value"; REPO=$2; shift 2 ;;
        --license-url) [ $# -ge 2 ] || die "--license-url needs a value"; LICENSE_URL=$2; shift 2 ;;
        --base-url)    [ $# -ge 2 ] || die "--base-url needs a value"; BASE_URL=$2; shift 2 ;;
        *) die "unknown argument $1" ;;
    esac
done

[ -n "$DIR" ] && [ -d "$DIR" ] || die "--dir must name an existing directory"
[ -n "$NAME" ]     || die "--name is required"
# Without these two a manifest is a list of bytes with no provenance, which
# is not a pin — it is a checksum file.
[ -n "$REVISION" ] || die "--revision is required: an unpinned model is not reproducible"
[ -n "$LICENSE" ]  || die "--license is required"

if command -v sha256sum >/dev/null 2>&1; then
    sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    die "neither sha256sum nor shasum is available"
fi

printf '# Pinned by tools/pin-model.sh. Every line below was measured, not\n'
printf '# transcribed: fetch-model.sh refuses any file that differs by one byte.\n'
printf 'version      1\n'
printf 'name         %s\n' "$NAME"
[ -n "$REPO" ]        && printf 'repo         %s\n' "$REPO"
printf 'revision     %s\n' "$REVISION"
printf 'license      %s\n' "$LICENSE"
[ -n "$LICENSE_URL" ] && printf 'license_url  %s\n' "$LICENSE_URL"
[ -n "$BASE_URL" ]    && printf 'base_url     %s\n' "$BASE_URL"
printf '\n'
printf '# sha256                                                           bytes      path\n'

total=0
count=0
# Sorted, so the manifest is stable across machines and a diff between two
# pins of the same revision is empty rather than a reshuffle.
( cd "$DIR" && find . -type f | sed 's|^\./||' | LC_ALL=C sort ) | while IFS= read -r rel; do
    f="$DIR/$rel"
    printf 'file  %s  %s  %s\n' "$(sha256_of "$f")" "$(wc -c < "$f" | tr -d ' ')" "$rel"
done

count=$( ( cd "$DIR" && find . -type f | wc -l ) | tr -d ' ')
total=$( ( cd "$DIR" && find . -type f -exec wc -c {} + ) | tail -1 | awk '{print $1}')
printf '\n# %s files, %s bytes\n' "$count" "$total"
