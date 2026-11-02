#!/usr/bin/env bash
# Builds WiiCompiled for Apple TV on a Mac:
#   extract -> translate -> cross-compile for tvOS -> bundle the user's game data -> sign -> install.
#
# The translation and the C++ build are the same as Launcher/local-build-macos.command; only the
# native configure targets tvOS (arm64) instead of the host. See docs/building-tvos.md.
# Must stay compatible with macOS's bash 3.2.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
launcher_dir=$(cd "$script_dir/.." && pwd)
default_workspace=$(cd "$launcher_dir/.." && pwd)
# shellcheck source=../build-cache.sh
source "$launcher_dir/build-cache.sh"

fail() { printf 'build-tvos.command: error: %s\n' "$*" >&2; exit 1; }
step() { printf 'MKWCBUILD:STEP:%s %s\n' "$1" "$2"; }
note() { printf 'MKWCBUILD: %s\n' "$*"; }
assert_file() { [[ -f "$1" ]] || fail "$2 is missing: $1"; }
assert_dir() { [[ -d "$1" ]] || fail "$2 is missing: $1"; }

usage() {
    cat <<'EOF'
Usage: build-tvos.command --output-dir DIR [options]

Builds the base game (WiiCompiled) as an Apple TV app. Retro Rewind is not supported on tvOS.

Game data
  --game IMAGE --nodtool PATH     Extract and verify a clean supported disc image first
                                  (otherwise Assets/ from an earlier extraction is used)

Target
  --simulator                     Build for the Apple TV Simulator (Apple Silicon Mac) instead of a device
  --cpu NAME                      Oldest Apple TV CPU to support (default: apple-a8 = Apple TV HD;
                                  apple-a10, apple-a12 or apple-a15 for Apple TV 4K-only builds)
  --deployment-target VERSION     Minimum tvOS version (default: 15.0)
  --bundle-id ID                  CFBundleIdentifier (default: local.wiicompiled.tvos); must match
                                  the provisioning profile used for a device build

Signing (device builds)
  --provisioning-profile PATH     tvOS .mobileprovision covering --bundle-id and your Apple TV
                                  (default: searched in Xcode's provisioning profile folders)
  --sign-identity ID              Code signing identity name or SHA-1 (default: the profile's
                                  certificate that is present in your keychain)
  --no-sign                       Leave the app unsigned (a device build then cannot be installed)
  --adhoc-sign                    Ad-hoc sign instead (no Apple account; for re-signing sideload
                                  tools such as atvloadly, which sign with your Apple ID)

Packaging
  --ipa                           Also write WiiCompiled.ipa (Payload/WiiCompiled.app, zipped) beside
                                  the app, the format sideloading tools install
  --no-bundle-data                Leave the game files (DATA) out of the app. The app then reads them
                                  from DATA in its data folder (Library/Caches/WiiCompiled), where they
                                  must be copied once; app updates then stay small and keep that copy

Install
  --install                       Install on --device, or on the booted simulator with --simulator
  --device ID                     Apple TV identifier or name as listed by: xcrun devicectl list devices
  --launch                        Launch after installing

Build
  --output-dir DIR                Where WiiCompiled.app is written (required)
  --workspace DIR                 Repository root (default: this script's repository)
  --force-clean-build             Delete local generated and tvOS native build caches
  --parallel N                    Pin translation and build parallelism
  --cmake PATH --ninja PATH       Override build tools
  --dotnet PATH                   Override dotnet
  --translator-bin PATH           Use a self-contained Translator.Cli executable
  --setup-version VERSION         Version recorded in the runtime log and Info.plist
EOF
}

workspace="$default_workspace"; output_dir=""; game=""; nodtool=""
simulator=0; cpu=apple-a8; deployment_target=15.0; bundle_id=local.wiicompiled.tvos
profile_path=""; sign_identity=""; no_sign=0; adhoc_sign=0; make_ipa=0; install=0; device=""; launch=0
bundle_data=1
force_clean=0; parallel=0; cmake_bin=cmake; ninja_bin=ninja; dotnet_bin=dotnet; translator_bin=""
setup_version=""
while (($#)); do
    case "$1" in
        --workspace) workspace=${2:-}; shift 2 ;;
        --output-dir) output_dir=${2:-}; shift 2 ;;
        --game) game=${2:-}; shift 2 ;;
        --nodtool) nodtool=${2:-}; shift 2 ;;
        --simulator) simulator=1; shift ;;
        --cpu) cpu=${2:-}; shift 2 ;;
        --deployment-target) deployment_target=${2:-}; shift 2 ;;
        --bundle-id) bundle_id=${2:-}; shift 2 ;;
        --provisioning-profile) profile_path=${2:-}; shift 2 ;;
        --sign-identity) sign_identity=${2:-}; shift 2 ;;
        --no-sign) no_sign=1; shift ;;
        --adhoc-sign) adhoc_sign=1; shift ;;
        --ipa) make_ipa=1; shift ;;
        --no-bundle-data) bundle_data=0; shift ;;
        --install) install=1; shift ;;
        --device) device=${2:-}; shift 2 ;;
        --launch) launch=1; shift ;;
        --force-clean-build) force_clean=1; shift ;;
        --parallel) parallel=${2:-}; shift 2 ;;
        --cmake) cmake_bin=${2:-}; shift 2 ;;
        --ninja) ninja_bin=${2:-}; shift 2 ;;
        --dotnet) dotnet_bin=${2:-}; shift 2 ;;
        --translator-bin) translator_bin=${2:-}; shift 2 ;;
        --setup-version) setup_version=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

# ---------------------------------------------------------------- validation
[[ $(uname -s) == Darwin ]] || fail 'building for Apple TV requires a Mac with Xcode'
[[ -n "$output_dir" ]] || fail '--output-dir is required'
[[ "$cpu" =~ ^apple-a(8|9|10|11|12|13|14|15|16|17|18)$ ]] || fail "--cpu must be an Apple A-series CPU such as apple-a8 (got '$cpu')"
[[ "$deployment_target" =~ ^[0-9]+\.[0-9]+$ ]] || fail "--deployment-target must look like 15.0 (got '$deployment_target')"
[[ "$bundle_id" =~ ^[A-Za-z0-9.-]+$ && "$bundle_id" == *.* ]] || fail "--bundle-id must be a reverse-DNS identifier (got '$bundle_id')"
[[ "$parallel" =~ ^[0-9]+$ ]] || fail '--parallel must be a whole number'
if [[ -n "$game" || -n "$nodtool" ]]; then [[ -n "$game" && -n "$nodtool" ]] || fail '--game and --nodtool must be supplied together'; fi
if (( simulator )); then
    [[ $(uname -m) == arm64 ]] || fail '--simulator needs an Apple Silicon Mac (the port is arm64-only)'
    [[ -z "$profile_path" && -z "$sign_identity" ]] || fail 'simulator builds are ad-hoc signed; drop --provisioning-profile/--sign-identity'
    [[ -z "$device" ]] || fail '--device is for device builds; --install --simulator uses the booted simulator'
    sdk=appletvsimulator
else
    sdk=appletvos
fi
(( no_sign && adhoc_sign )) && fail 'choose only one of --no-sign and --adhoc-sign'
if (( adhoc_sign )); then
    [[ -z "$profile_path" && -z "$sign_identity" ]] || fail '--adhoc-sign uses no profile or identity; drop --provisioning-profile/--sign-identity'
fi
if (( install )); then
    (( no_sign )) && fail '--install needs a signed app; drop --no-sign'
    (( adhoc_sign && ! simulator )) && fail '--install on a device needs a real signature; drop --adhoc-sign'
    (( simulator )) || [[ -n "$device" ]] || fail '--install needs --device (see: xcrun devicectl list devices)'
fi
(( launch )) && ! (( install )) && fail '--launch requires --install'

workspace=$(cd "$workspace" && pwd)
if [[ -z "$setup_version" && -f "$workspace/Launcher/Directory.Build.props" ]]; then
    setup_version=$(sed -nE 's/.*<Version>([^<]+)<\/Version>.*/\1/p' "$workspace/Launcher/Directory.Build.props" | head -n 1)
fi
[[ "$setup_version" =~ ^[0-9A-Za-z.+-]*$ ]] || fail '--setup-version contains unsupported characters'

# The tvOS SDKs ship only with full Xcode, not with the Command Line Tools.
sysroot=$(xcrun --sdk "$sdk" --show-sdk-path 2>/dev/null) && [[ -d "$sysroot" ]] || \
    fail "the $sdk SDK was not found; install Xcode with the tvOS platform (Xcode > Settings > Components) and select it with: sudo xcode-select -s /Applications/Xcode.app"
sdk_version=$(xcrun --sdk "$sdk" --show-sdk-version 2>/dev/null || true)
cc_bin=$(xcrun --sdk "$sdk" -f clang 2>/dev/null) || fail 'Xcode clang not found'
cxx_bin=$(xcrun --sdk "$sdk" -f clang++ 2>/dev/null) || fail 'Xcode clang++ not found'
for tool in "$cmake_bin" "$ninja_bin" "$cc_bin" "$cxx_bin" shasum rsync codesign python3; do
    command -v "$tool" >/dev/null || fail "required tool not found: $tool"
done
note "tvOS SDK $sdk_version ($sdk), CPU $cpu, minimum tvOS $deployment_target"

# ---------------------------------------------------------------- game data
assets="$workspace/Assets"; generated="$workspace/generated"
functions="$generated/functions"; metadata="$generated/base_translation_output.json"; manifest_dir="$workspace/build/base"
manifest="$manifest_dir/mkwii_base_manifest.json"; shards="$generated/build_shards"
if (( simulator )); then native_build="$workspace/native-build-tvos-simulator"; else native_build="$workspace/native-build-tvos"; fi

if [[ -n "$game" ]]; then
    # Through bash: the executable bit may not survive copying the repository to the Mac.
    bash "$launcher_dir/macos/extract-disc.command" --workspace "$workspace" --game "$game" --assets-dir "$assets" --nodtool "$nodtool"
fi
assert_file "$assets/main.dol" 'extracted main.dol (pass --game and --nodtool)'
assert_file "$assets/StaticR.rel" 'extracted StaticR.rel (pass --game and --nodtool)'
# The app reads the disc's files from inside its own bundle (an Apple TV app has nowhere else to
# keep them), so the extracted DATA directory is required here, not just main.dol/StaticR.rel.
assert_file "$assets/DATA/sys/boot.bin" 'extracted DATA/sys/boot.bin (pass --game and --nodtool)'
assert_file "$assets/DATA/sys/fst.bin" 'extracted DATA/sys/fst.bin (pass --game and --nodtool)'
assert_dir "$assets/DATA/files" 'extracted DATA/files (pass --game and --nodtool)'
# The translation project is the one pinned to the extracted main.dol, which decides the region.
project=$(mkw_resolve_project "$workspace" "$assets") || fail 'the extracted game cannot be translated'
region=$(mkw_project_pin "$project" region); [[ -n "$region" ]] || fail "could not find the region of $project"

if (( force_clean )); then
    step force-clean 'Discarding translation and tvOS native build caches'
    rm -rf "$generated" "$manifest_dir" "$native_build"
fi

# ---------------------------------------------------------------- translation (host)
if [[ -n "$translator_bin" ]]; then
    assert_file "$translator_bin" 'Translator.Cli executable'
    translator() { "$translator_bin" "$@"; }
else
    command -v "$dotnet_bin" >/dev/null || fail "required tool not found: $dotnet_bin (install the .NET 8 SDK)"
    translator_dll="$workspace/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
    if [[ ! -f "$translator_dll" ]]; then
        step build-translator 'Building the translator'
        "$dotnet_bin" build "$workspace/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release
    fi
    translator() { "$dotnet_bin" "$translator_dll" "$@"; }
fi

entry_point=$(awk '/^translation:/{inside=1} inside && /^[[:space:]]*-[[:space:]]*0[xX][0-9a-fA-F]+[[:space:]]*$/{gsub(/^[[:space:]]*-[[:space:]]*/, ""); print; exit}' "$project")
[[ -n "$entry_point" ]] || fail 'could not find the translation entry point'
host_cpus=$(sysctl -n hw.ncpu); mem_gib=$(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )); (( mem_gib < 1 )) && mem_gib=1
if (( parallel > 0 )); then translator_threads=$parallel; translated_jobs=$parallel; global_jobs=$parallel
else translator_threads=$(( host_cpus < 16 ? host_cpus : 16 )); translated_jobs=$(( host_cpus < mem_gib / 2 ? host_cpus : mem_gib / 2 )); (( translated_jobs < 1 )) && translated_jobs=1; global_jobs=$host_cpus; fi

translation_provenance="$generated/translation-provenance.json"
translation_fingerprint=$(mkw_translation_identity "$workspace" "${translator_bin:-$(dirname "${translator_dll:-}")}" "$script_dir/build-tvos.command")
if [[ "$(mkw_read_identity "$translation_provenance" TranslationFingerprint)" == "$translation_fingerprint" && -f "$metadata" && -f "$manifest" ]]; then
    step reuse-base-translation 'Reusing the completed base translation'
else
    rm -f "$translation_provenance"; mkdir -p "$functions" "$manifest_dir"
    step translate-base 'Translating the user-owned base game'
    translator translate-recursive "$entry_point" --project "$project" --outdir "$functions" --output-metadata "$metadata" --production-source-bundle "$generated/base_translation_sources.bin" --no-function-files --prune-stale --threads "$translator_threads"
    step emit-base-manifest 'Creating the local base translation manifest'
    translator emit-base-manifest --project "$project" --out "$manifest_dir" --functions-dir "$functions" --translation-output-metadata "$metadata" --region "$region"
    mkw_write_identity "$translation_provenance" TranslationFingerprint "$translation_fingerprint"
fi
step generate-data-init 'Generating local game data initialization'
translator generate-data-init --project "$project"
# Base game only: no --resolved-profile/--retro-cpp-dir, so the shard graph has no Retro Rewind.
step emit-build-shards 'Preparing native build shards'
translator emit-build-shards --project "$project" --base-metadata "$metadata" --base-functions-dir "$functions" --native-source-dir "$workspace/runtime/src" --out "$shards"

# ---------------------------------------------------------------- native cross-build (tvOS)
toolchain_provenance="$native_build/toolchain-provenance.json"
toolchain_fingerprint=$(printf '%s\n' "$(mkw_toolchain_identity "$workspace" "$cc_bin" "$cxx_bin" "$cmake_bin" "$ninja_bin")" \
    "$cc_bin" "$cxx_bin" "$sysroot" "$sdk_version" "$deployment_target" "$cpu" "$bundle_id" | mkw_sha256)
if [[ -d "$native_build" && "$(mkw_read_identity "$toolchain_provenance" ToolchainFingerprint)" != "$toolchain_fingerprint" ]]; then
    note 'The toolchain, SDK, target or a pinned dependency changed; rebuilding the tvOS native build from scratch'
    rm -rf "$native_build"
fi
step configure-native 'Configuring the tvOS cross-build'
# Dawn and SDL3 are built from aurora's pinned sources (no tvOS packages exist). The first
# configure downloads them and Dawn's own dependencies, so it needs network access and python3.
"$cmake_bin" -S "$workspace/runtime" -B "$native_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=tvOS \
    -DCMAKE_OSX_SYSROOT="$sysroot" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
    -DCMAKE_C_COMPILER="$cc_bin" \
    -DCMAKE_CXX_COMPILER="$cxx_bin" \
    -DCMAKE_OBJC_COMPILER="$cc_bin" \
    -DCMAKE_OBJCXX_COMPILER="$cxx_bin" \
    -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
    -DMKW_TRANSLATED_COMPILE_JOBS="$translated_jobs" \
    -DMKW_TVOS_CPU="$cpu" \
    -DMKW_TVOS_BUNDLE_IDENTIFIER="$bundle_id" \
    -DAURORA_SDL3_PROVIDER=vendor \
    -DAURORA_DAWN_PROVIDER=vendor \
    -DMKW_SETUP_VERSION="$setup_version"
mkw_write_identity "$toolchain_provenance" ToolchainFingerprint "$toolchain_fingerprint"
step compile 'Compiling WiiCompiled for tvOS'
"$cmake_bin" --build "$native_build" --target WiiCompiled --parallel "$global_jobs"

# ---------------------------------------------------------------- app bundle
built_app="$native_build/WiiCompiled.app"
assert_file "$built_app/WiiCompiled" 'built tvOS executable'
assert_file "$built_app/Info.plist" 'built Info.plist'
for asset in dsp_coef.bin initial_pipeline_cache.db cacert.pem; do
    assert_file "$built_app/$asset" "bundled runtime asset $asset"
done
assert_dir "$built_app/wii_bootstrap" 'bundled Wii first-run bootstrap'
lipo -archs "$built_app/WiiCompiled" 2>/dev/null | grep -qw arm64 || fail 'the built executable is not arm64'

mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
app="$output_dir/WiiCompiled.app"
mkdir -p "$app"
if (( bundle_data )); then
    step bundle 'Assembling WiiCompiled.app with the extracted game data'
    # DATA is excluded from the first pass, which also protects it from --delete; the second pass
    # mirrors it incrementally so a rebuild does not recopy gigabytes of unchanged game files.
    rsync -a --delete --exclude '/DATA' --exclude '/_CodeSignature' --exclude '/embedded.mobileprovision' "$built_app/" "$app/"
    rsync -a --delete "$assets/DATA/" "$app/DATA/"
else
    step bundle 'Assembling WiiCompiled.app without the game data (--no-bundle-data)'
    rsync -a --delete --exclude '/_CodeSignature' --exclude '/embedded.mobileprovision' "$built_app/" "$app/"
    rm -rf "$app/DATA"
    note 'The app reads the game files from Library/Caches/WiiCompiled/DATA in its container; copy them there once (docs/building-tvos.md)'
fi
rm -rf "$app/_CodeSignature" "$app/embedded.mobileprovision"
bundle_kib=$(du -sk "$app" | awk '{print $1}')
note "WiiCompiled.app is $(( bundle_kib / 1024 )) MiB"
if (( bundle_kib > 4 * 1024 * 1024 )); then
    note 'warning: the app is larger than the 4 GiB tvOS app size limit; installation may be refused'
fi

# ---------------------------------------------------------------- signing
plist_value() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null; }

# Decodes a provisioning profile into $1 (an XML plist). $2 = .mobileprovision.
decode_profile() { security cms -D -i "$2" > "$1" 2>/dev/null; }

# Succeeds when the decoded profile $1 is an unexpired tvOS profile for $bundle_id.
profile_matches() {
    local plist=$1 platforms team app_id pattern expiry expiry_epoch
    platforms=$(plist_value "$plist" Platform) || return 1
    grep -q 'tvOS' <<<"$platforms" || return 1
    team=$(plist_value "$plist" TeamIdentifier:0) || return 1
    app_id=$(plist_value "$plist" Entitlements:application-identifier) || return 1
    pattern=${app_id#"$team."}
    if [[ "$pattern" == "*" ]]; then :;
    elif [[ "$pattern" == *'*' ]]; then [[ "$bundle_id" == "${pattern%\*}"* ]] || return 1;
    else [[ "$pattern" == "$bundle_id" ]] || return 1; fi
    expiry=$(plutil -extract ExpirationDate raw -o - "$plist" 2>/dev/null) || return 1
    expiry_epoch=$(date -j -u -f '%Y-%m-%dT%H:%M:%SZ' "$expiry" +%s 2>/dev/null) || return 1
    (( expiry_epoch > $(date +%s) ))
}

sign_work=$(mktemp -d "${TMPDIR:-/tmp}/wiicompiled-tvos-sign.XXXXXX")
cleanup() { rm -rf "$sign_work"; }
trap cleanup EXIT

if (( no_sign )); then
    note 'Leaving WiiCompiled.app unsigned (--no-sign)'
elif (( simulator )); then
    step sign 'Ad-hoc signing for the Apple TV Simulator'
    codesign --force --sign - --timestamp=none "$app"
elif (( adhoc_sign )); then
    # No Apple account involved: a sideloading tool replaces this signature with one made from
    # the user's Apple ID when it installs the app.
    step sign 'Ad-hoc signing WiiCompiled.app for a re-signing sideload tool'
    codesign --force --sign - --timestamp=none "$app"
else
    step sign 'Signing WiiCompiled.app for Apple TV'
    profile_plist="$sign_work/profile.plist"
    if [[ -n "$profile_path" ]]; then
        assert_file "$profile_path" 'provisioning profile'
        decode_profile "$profile_plist" "$profile_path" || fail "could not decode $profile_path"
        profile_matches "$profile_plist" || fail "$profile_path is not an unexpired tvOS profile covering $bundle_id"
    else
        # Exact App IDs win over wildcards; among equals, the newest file wins.
        best=""; best_rank=0
        for dir in "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles" "$HOME/Library/MobileDevice/Provisioning Profiles"; do
            [[ -d "$dir" ]] || continue
            for candidate in "$dir"/*.mobileprovision; do
                [[ -f "$candidate" ]] || continue
                decode_profile "$sign_work/candidate.plist" "$candidate" || continue
                profile_matches "$sign_work/candidate.plist" || continue
                candidate_team=$(plist_value "$sign_work/candidate.plist" TeamIdentifier:0)
                candidate_app_id=$(plist_value "$sign_work/candidate.plist" Entitlements:application-identifier)
                if [[ "$candidate_app_id" == "$candidate_team.$bundle_id" ]]; then rank=2; else rank=1; fi
                if (( rank > best_rank )) || { (( rank == best_rank )) && [[ "$candidate" -nt "$best" ]]; }; then
                    best=$candidate; best_rank=$rank
                fi
            done
        done
        [[ -n "$best" ]] || fail "no unexpired tvOS provisioning profile covers $bundle_id; see docs/building-tvos.md (\"Signing\") or pass --provisioning-profile"
        profile_path=$best
        decode_profile "$profile_plist" "$profile_path"
    fi
    team=$(plist_value "$profile_plist" TeamIdentifier:0)
    note "Provisioning profile: $profile_path (team $team)"

    if [[ -z "$sign_identity" ]]; then
        identities=$(security find-identity -v -p codesigning)
        index=0
        while plutil -extract "DeveloperCertificates.$index" raw -o - "$profile_plist" > "$sign_work/cert.b64" 2>/dev/null; do
            sha1=$(base64 -D -i "$sign_work/cert.b64" | shasum -a 1 | awk '{print toupper($1)}')
            if grep -q "$sha1" <<<"$identities"; then sign_identity=$sha1; break; fi
            index=$(( index + 1 ))
        done
        [[ -n "$sign_identity" ]] || fail "none of the profile's certificates has a private key in your keychain; pass --sign-identity or let Xcode repair signing"
    fi

    entitlements="$sign_work/entitlements.plist"
    /usr/libexec/PlistBuddy -x -c 'Print :Entitlements' "$profile_plist" > "$entitlements" || fail 'could not read the profile entitlements'
    # A wildcard profile grants "TEAM.*"; the signed app must name its concrete App ID.
    /usr/libexec/PlistBuddy -c "Set :application-identifier $team.$bundle_id" "$entitlements"
    cp "$profile_path" "$app/embedded.mobileprovision"
    codesign --force --sign "$sign_identity" --entitlements "$entitlements" --generate-entitlement-der --timestamp=none "$app"
fi
if (( ! no_sign )); then
    codesign --verify --strict "$app" || fail 'code signature verification failed'
fi

# ---------------------------------------------------------------- ipa
if (( make_ipa )); then
    step ipa 'Packaging WiiCompiled.ipa'
    ipa="$output_dir/WiiCompiled.ipa"
    ipa_stage="$output_dir/.ipa-stage"
    rm -rf "$ipa_stage" "$ipa"
    mkdir -p "$ipa_stage/Payload"
    # A hard-linked copy keeps the multi-gigabyte game data from being duplicated on disk.
    cp -Rl "$app" "$ipa_stage/Payload/" 2>/dev/null || ditto "$app" "$ipa_stage/Payload/WiiCompiled.app"
    # --keepParent stores the entries as Payload/WiiCompiled.app/..., the IPA layout; ditto
    # writes Zip64 entries when the archive passes 4 GiB.
    ditto -c -k --norsrc --keepParent "$ipa_stage/Payload" "$ipa"
    rm -rf "$ipa_stage"
    note "WiiCompiled.ipa is $(( $(stat -f %z "$ipa") / 1024 / 1024 )) MiB"
    printf 'MKWCBUILD:IPA=%s\n' "$ipa"
fi

# ---------------------------------------------------------------- install
if (( install )); then
    if (( simulator )); then
        step install 'Installing on the booted Apple TV Simulator'
        xcrun simctl install booted "$app" || fail 'install failed; boot an Apple TV simulator first (Xcode > Window > Devices and Simulators)'
        if (( launch )); then xcrun simctl launch booted "$bundle_id"; fi
    else
        step install "Installing on $device"
        xcrun devicectl device install app --device "$device" "$app"
        if (( launch )); then xcrun devicectl device process launch --device "$device" "$bundle_id"; fi
    fi
fi

note 'The app contains your own game data. Never share or upload it.'
printf 'MKWCBUILD:APP=%s\n' "$app"
