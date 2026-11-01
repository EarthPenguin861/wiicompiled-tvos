#!/usr/bin/env bash
# Downloads an encrypted tvOS build published by .github/workflows/tvos.yml and decrypts it into
# WiiCompiled.ipa, ready for a sideloading tool such as atvloadly.
#
# Usage: fetch-ipa.sh KEY_FILE OUT_DIR [TAG]
#   KEY_FILE  the key encrypt-game-data.sh created
#   OUT_DIR   where WiiCompiled.ipa is written
#   TAG       a release such as tvos-3 (default: the newest tvos-* release)
# Run it inside this repository's clone (the GitHub repository is taken from its origin remote).
# Runs in Git Bash on Windows, and on macOS/Linux. Needs gh (logged in), openssl and cat.
set -euo pipefail

fail() { printf 'fetch-ipa.sh: error: %s\n' "$*" >&2; exit 1; }
(( $# == 2 || $# == 3 )) || fail 'usage: fetch-ipa.sh KEY_FILE OUT_DIR [TAG]'
key_file=$1; out_dir=$2; tag=${3:-}

gh_bin=$(command -v gh || true)
if [[ -z "$gh_bin" && -x "/c/Program Files/GitHub CLI/gh.exe" ]]; then gh_bin="/c/Program Files/GitHub CLI/gh.exe"; fi
[[ -n "$gh_bin" ]] || fail 'the GitHub CLI (gh) is not installed'
command -v openssl >/dev/null || fail 'openssl not found'
[[ -s "$key_file" ]] || fail "key file not found: $key_file"
MKW_DATA_KEY=$(head -n 1 "$key_file" | tr -d '\r\n')
export MKW_DATA_KEY
[[ "$MKW_DATA_KEY" =~ ^[0-9a-f]{64}$ ]] || fail "$key_file does not hold a 64-character hex key"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" remote get-url origin 2>/dev/null | sed -E 's#^.*github\.com[:/]##; s#\.git$##')
[[ -n "$repo" ]] || fail 'could not tell the GitHub repository from the origin remote'

if [[ -z "$tag" ]]; then
    tag=$("$gh_bin" release list --repo "$repo" --limit 50 --json tagName,createdAt \
        --jq '[.[] | select(.tagName | startswith("tvos-"))] | sort_by(.createdAt) | last | .tagName // empty')
    [[ -n "$tag" ]] || fail "no tvos-* release in $repo yet; run the tvOS build workflow first"
fi

mkdir -p "$out_dir"
work=$(mktemp -d "${TMPDIR:-/tmp}/wiicompiled-ipa.XXXXXX")
trap 'rm -rf "$work"' EXIT
printf 'Downloading %s from %s ...\n' "$tag" "$repo"
"$gh_bin" release download "$tag" --repo "$repo" --dir "$work" --pattern 'WiiCompiled.ipa.enc.part-*'
printf 'Decrypting ...\n'
cat "$work"/WiiCompiled.ipa.enc.part-* \
    | openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -md sha256 -pass env:MKW_DATA_KEY \
    > "$out_dir/WiiCompiled.ipa"
ls -l "$out_dir/WiiCompiled.ipa"
printf 'Ready: %s - install it with atvloadly. It contains your game; do not share it.\n' "$out_dir/WiiCompiled.ipa"
