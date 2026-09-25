#!/bin/sh
# fetch-model.sh — install a pinned model, or refuse.
#
# The plan's requirement, in one sentence: setup pins the model revision,
# every file including the vision tower and projector, SHA-256, licence and
# download size; files land in temporary storage, are verified, and only then
# are installed atomically. Runtime starts offline and never attempts a
# download.
#
# So this script does exactly two things it will not compromise on:
#
#   1. It installs nothing it has not verified. Every file in the manifest is
#      checked by SHA-256 AND byte count before anything becomes visible. A
#      manifest with no `file` lines is not "a model with no files" — it is an
#      unpinned manifest, and it is refused.
#
#   2. A half-finished install never exists under the final path. Files are
#      staged in a sibling directory and the whole thing is renamed into place
#      once, so an interrupted run leaves either the old install or nothing —
#      never a directory that looks complete and is not.
#
# Offline is the normal case: `--from <dir>` installs from a local model
# directory with the same verification and no network at all, which is what
# makes a fully net-free setup possible. Re-running against an existing,
# verified install is a no-op.
#
#   tools/fetch-model.sh <name> [--from <dir>] [--verify-only] [--force]
#   tools/fetch-model.sh --manifest <path> [...]
#
# Exit codes: 0 ok, 1 verification or download failure, 2 usage.
set -eu

PROG=$(basename "$0")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MANIFEST=
NAME=
FROM=
VERIFY_ONLY=0
FORCE=0

die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 1; }
usage() {
    cat >&2 <<USAGE
usage: $PROG <name> [--from <dir>] [--verify-only] [--force]
       $PROG --manifest <path> [--from <dir>] [--verify-only] [--force]

  <name>          a manifest under tools/models/<name>.model
  --from <dir>    install from a local directory instead of downloading
  --verify-only   check an existing install and change nothing
  --force         replace an existing install
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --manifest) [ $# -ge 2 ] || usage; MANIFEST=$2; shift 2 ;;
        --from)     [ $# -ge 2 ] || usage; FROM=$2; shift 2 ;;
        --verify-only) VERIFY_ONLY=1; shift ;;
        --force)    FORCE=1; shift ;;
        -h|--help)  usage ;;
        --*)        die "unknown option $1" ;;
        *)          [ -z "$NAME" ] || usage; NAME=$1; shift ;;
    esac
done

if [ -z "$MANIFEST" ]; then
    [ -n "$NAME" ] || usage
    MANIFEST="$ROOT/tools/models/$NAME.model"
fi
[ -f "$MANIFEST" ] || die "no manifest at $MANIFEST"

# One digest tool, chosen once. Two implementations print different columns,
# and guessing per call is how a verifier ends up comparing a filename.
if command -v sha256sum >/dev/null 2>&1; then
    sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    die "neither sha256sum nor shasum is available; cannot verify anything"
fi

size_of() { wc -c < "$1" | tr -d ' '; }

# --- manifest ---------------------------------------------------------

m_version=; m_name=; m_repo=; m_revision=; m_license=; m_license_url=; m_base_url=
files_tmp=$(mktemp)
trap 'rm -f "$files_tmp"' EXIT
n_files=0
total_bytes=0
lineno=0

while IFS= read -r line || [ -n "$line" ]; do
    lineno=$((lineno + 1))
    case "$line" in ''|'#'*) continue ;; esac
    key=${line%%[   ]*}
    val=$(printf '%s' "$line" | sed "s/^$key[ 	]*//")
    case "$key" in
        version)     m_version=$val ;;
        name)        m_name=$val ;;
        repo)        m_repo=$val ;;
        revision)    m_revision=$val ;;
        license)     m_license=$val ;;
        license_url) m_license_url=$val ;;
        base_url)    m_base_url=$val ;;
        file)
            sha=$(printf '%s' "$val" | awk '{print $1}')
            bytes=$(printf '%s' "$val" | awk '{print $2}')
            path=$(printf '%s' "$val" | awk '{ $1=""; $2=""; sub(/^  */, ""); print }')
            case "$sha" in
                *[!0-9a-f]*|'') die "line $lineno: sha256 must be 64 hex digits" ;;
            esac
            [ ${#sha} -eq 64 ] || die "line $lineno: sha256 must be 64 hex digits"
            case "$bytes" in ''|*[!0-9]*) die "line $lineno: size must be a byte count" ;; esac
            [ -n "$path" ] || die "line $lineno: file needs a path"
            # A path that escapes the model directory would let a manifest
            # write anywhere the user can. Refuse rather than sanitise.
            case "$path" in
                /*|*..*) die "line $lineno: path '$path' must stay inside the model directory" ;;
            esac
            printf '%s %s %s\n' "$sha" "$bytes" "$path" >> "$files_tmp"
            n_files=$((n_files + 1))
            total_bytes=$((total_bytes + bytes))
            ;;
        *) die "line $lineno: unknown key '$key'" ;;
    esac
done < "$MANIFEST"

[ "$m_version" = "1" ] || die "manifest version must be 1 (got '${m_version:-none}')"
[ -n "$m_name" ]     || die "manifest needs a name"
[ -n "$m_revision" ] || die "manifest needs a revision: an unpinned model is not reproducible"
[ -n "$m_license" ]  || die "manifest needs a license"

# The whole point of this script. A manifest that pins nothing would install
# nothing and report success, which is worse than failing.
[ "$n_files" -gt 0 ] || die "manifest pins no files — run tools/pin-model.sh on a machine with network access to produce a real one"

DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
DEST="$DATA_HOME/geist-watch/models/$m_name"

printf 'model    %s\n' "$m_name"
printf 'revision %s\n' "$m_revision"
printf 'license  %s%s\n' "$m_license" "${m_license_url:+  ($m_license_url)}"
printf 'files    %s, %s bytes total\n' "$n_files" "$total_bytes"
printf 'dest     %s\n' "$DEST"

# --- verify a directory against the manifest --------------------------

verify_dir() {
    _dir=$1
    _bad=0
    while read -r _sha _bytes _path; do
        _f="$_dir/$_path"
        if [ ! -f "$_f" ]; then
            printf '  MISSING  %s\n' "$_path" >&2
            _bad=$((_bad + 1))
            continue
        fi
        _have_size=$(size_of "$_f")
        if [ "$_have_size" != "$_bytes" ]; then
            printf '  SIZE     %s: %s bytes, expected %s\n' "$_path" "$_have_size" "$_bytes" >&2
            _bad=$((_bad + 1))
            continue
        fi
        _have_sha=$(sha256_of "$_f")
        if [ "$_have_sha" != "$_sha" ]; then
            printf '  SHA256   %s: %s, expected %s\n' "$_path" "$_have_sha" "$_sha" >&2
            _bad=$((_bad + 1))
            continue
        fi
    done < "$files_tmp"
    [ "$_bad" -eq 0 ]
}

if [ "$VERIFY_ONLY" -eq 1 ]; then
    [ -d "$DEST" ] || die "nothing installed at $DEST"
    if verify_dir "$DEST"; then
        printf 'verified: %s files match the manifest\n' "$n_files"
        exit 0
    fi
    die "installed model does not match the manifest"
fi

if [ -d "$DEST" ] && [ "$FORCE" -eq 0 ]; then
    if verify_dir "$DEST" 2>/dev/null; then
        printf 'already installed and verified; nothing to do\n'
        exit 0
    fi
    die "$DEST exists but does not match the manifest; re-run with --force to replace it"
fi

# --- stage, verify, then publish --------------------------------------

STAGE="$DEST.staging.$$"
rm -rf "$STAGE"
mkdir -p "$STAGE"
# The staging directory is removed on any failure, so an aborted run leaves
# the previous install untouched rather than a plausible-looking ruin.
trap 'rm -f "$files_tmp"; rm -rf "$STAGE"' EXIT

while read -r sha bytes path; do
    mkdir -p "$STAGE/$(dirname "$path")"
    if [ -n "$FROM" ]; then
        [ -f "$FROM/$path" ] || die "local source has no $path"
        cp "$FROM/$path" "$STAGE/$path"
    else
        [ -n "$m_base_url" ] || die "manifest has no base_url and --from was not given; nothing to download from"
        url="$m_base_url/$path"
        printf '  fetching %s\n' "$path"
        curl -fsSL --retry 3 -o "$STAGE/$path" "$url" \
            || die "download failed: $url"
    fi
done < "$files_tmp"

printf 'verifying %s files\n' "$n_files"
verify_dir "$STAGE" || die "verification failed; nothing was installed"

mkdir -p "$(dirname "$DEST")"
if [ -d "$DEST" ]; then
    OLD="$DEST.old.$$"
    mv "$DEST" "$OLD"
    mv "$STAGE" "$DEST"
    rm -rf "$OLD"
else
    mv "$STAGE" "$DEST"
fi
trap 'rm -f "$files_tmp"' EXIT

printf 'installed %s\n' "$DEST"
