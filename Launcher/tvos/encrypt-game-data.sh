#!/usr/bin/env bash
# Packs an extracted Mario Kart Wii DATA folder (files/ + sys/) into AES-256 encrypted parts that the
# GitHub tvOS workflow (.github/workflows/tvos.yml) downloads from the release tagged `game-data`.
# The key never leaves your machine except as the MKW_DATA_KEY repository secret.
#
# Usage: encrypt-game-data.sh DATA_DIR OUT_DIR KEY_FILE
#   DATA_DIR  the extracted disc's DATA folder (must be named DATA and contain sys/ and files/)
#   OUT_DIR   where mkw-data.tar.enc.part-* are written (keep it outside synced folders)
#   KEY_FILE  created with a fresh random key if it does not exist yet; keep it - fetch-ipa.sh
#             needs the same key to decrypt finished builds
# Runs in Git Bash on Windows, and on macOS/Linux. Needs tar, openssl and split.
set -euo pipefail

fail() { printf 'encrypt-game-data.sh: error: %s\n' "$*" >&2; exit 1; }
(( $# == 3 )) || fail 'usage: encrypt-game-data.sh DATA_DIR OUT_DIR KEY_FILE'
data_dir=${1%/}; out_dir=$2; key_file=$3

[[ "$(basename "$data_dir")" == DATA ]] || fail "the folder must be named DATA (got $(basename "$data_dir"))"
for required in sys/boot.bin sys/fst.bin sys/main.dol files/rel/StaticR.rel; do
    [[ -f "$data_dir/$required" ]] || fail "$data_dir/$required is missing; is this an extracted Wii disc?"
done
for tool in tar openssl split; do command -v "$tool" >/dev/null || fail "required tool not found: $tool"; done

mkdir -p "$out_dir" "$(dirname "$key_file")"
if [[ ! -s "$key_file" ]]; then
    # Written with printf so the file holds the key and a plain LF on every platform.
    (umask 077; printf '%s\n' "$(openssl rand -hex 32 | tr -d '\r\n')" > "$key_file")
    printf 'Created a new key: %s\n' "$key_file"
fi
# Line endings are stripped and the key is handed over through the environment, exactly as the
# workflow receives it from the MKW_DATA_KEY secret (openssl's file: source would keep a CR).
MKW_DATA_KEY=$(head -n 1 "$key_file" | tr -d '\r\n')
export MKW_DATA_KEY
[[ "$MKW_DATA_KEY" =~ ^[0-9a-f]{64}$ ]] || fail "$key_file does not hold a 64-character hex key"

rm -f "$out_dir"/mkw-data.tar.enc.part-*
printf 'Packing and encrypting %s ...\n' "$data_dir"
# Parts stay under GitHub's 2 GiB limit for one release asset.
tar -C "$(dirname "$data_dir")" -cf - DATA \
    | openssl enc -aes-256-cbc -pbkdf2 -iter 200000 -md sha256 -salt -pass env:MKW_DATA_KEY \
    | split -b 1900m - "$out_dir/mkw-data.tar.enc.part-"
ls -l "$out_dir"/mkw-data.tar.enc.part-*
printf 'Done. Upload the parts above to the release `game-data`; set MKW_DATA_KEY from %s.\n' "$key_file"
