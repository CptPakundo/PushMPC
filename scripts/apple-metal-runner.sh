#!/bin/sh
# apple-metal-runner.sh: turns a rented bare-metal Mac (Scaleway Mac mini M4, AWS mac-m4.metal / mac-m4pro.metal) into
# the self-hosted runner `apple-metal` for .github/workflows/apple-metal.yml, and wipes it again afterwards.
#
#   apple-metal-runner.sh setup <runner registration token>     (token: repo Settings > Actions > Runners > New, 1 h)
#   apple-metal-runner.sh run                                     the runner in the foreground (or `svc` below)
#   apple-metal-runner.sh svc                                     the runner as a launchd service (survives logout)
#   apple-metal-runner.sh teardown <runner removal token>         deregisters, deletes the runner, checkouts, logs, caches
#
# What setup does, all under ~/pushmpc-metal (nothing system-wide except what Homebrew and Xcode need):
#   - Xcode command line tools (needs a GUI-less `xcode-select --install` or Xcode from the provider's image; macOS 26
#     SDK required for the low-latency effects: check `xcrun --sdk macosx --show-sdk-version` >= 26)
#   - Homebrew (if missing), cmake, python3, gh
#   - the GitHub Actions runner (pinned version + SHA-256 below), registered to the PRIVATE repository with the labels
#     self-hosted, macOS, ARM64, apple-metal
#   - a note of the macOS version: upgrade to macOS 26 (and later 27 for the size queries) through
#     System Settings > Software Update before running the workflow
# Costs money while the machine is rented (Apple's licence: 24 h minimum). Ask the owner before renting.
set -eu

REPO_URL="https://github.com/CptPakundo/PushMPC"
RUNNER_VERSION="2.329.0"
RUNNER_SHA256="TODO-PIN-AT-SETUP"     # sha256 of actions-runner-osx-arm64-$RUNNER_VERSION.tar.gz from the release page
BASE="$HOME/pushmpc-metal"
RUNNER="$BASE/runner"

need_arm() { [ "$(uname -m)" = arm64 ] || { echo "not Apple silicon"; exit 1; }; }

setup() {
    need_arm
    token=${1:?registration token}
    sw_vers
    major=$(sw_vers -productVersion | cut -d. -f1)
    [ "$major" -ge 26 ] || echo "WARNING: macOS $major: the low-latency effects need macOS 26 (upgrade first, then rerun setup)"
    xcode-select -p > /dev/null 2>&1 || { echo "installing the command line tools (accept the dialog, then rerun)"; xcode-select --install; exit 1; }
    if ! command -v brew > /dev/null 2>&1; then
        NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        eval "$(/opt/homebrew/bin/brew shellenv)"
    fi
    brew install cmake python3 gh > /dev/null
    mkdir -p "$RUNNER"
    cd "$RUNNER"
    tgz="actions-runner-osx-arm64-$RUNNER_VERSION.tar.gz"
    [ -f "$tgz" ] || curl -fsSL -o "$tgz" "https://github.com/actions/runner/releases/download/v$RUNNER_VERSION/$tgz"
    got=$(shasum -a 256 "$tgz" | cut -d' ' -f1)
    case $RUNNER_SHA256 in
        TODO*) echo "runner tarball sha256: $got (compare with the release page before continuing)"; printf "continue? [y/N] "; read -r a; [ "$a" = y ] || exit 1 ;;
        *) [ "$got" = "$RUNNER_SHA256" ] || { echo "runner tarball SHA-256 mismatch: $got"; exit 1; } ;;
    esac
    tar xzf "$tgz"
    ./config.sh --unattended --url "$REPO_URL" --token "$token" --name "apple-metal-$(hostname -s)" --labels apple-metal --work "$BASE/work" --replace
    echo "registered. Start it with: $0 run   (or: $0 svc)"
    echo "then dispatch the workflow 'apple-metal' on the branch to test"
}

teardown() {
    token=${1:?removal token}
    cd "$RUNNER" 2>/dev/null || { echo "no runner in $RUNNER"; exit 0; }
    ./svc.sh stop > /dev/null 2>&1 || true
    ./svc.sh uninstall > /dev/null 2>&1 || true
    ./config.sh remove --token "$token" || true
    cd "$HOME"
    rm -rf "$BASE" "$HOME/Library/Logs/PushMPC" "$HOME/.config/pushmpc" "$HOME/.config/mpv"
    brew uninstall --force pushmpc > /dev/null 2>&1 || true
    rm -f "$(brew --prefix 2>/dev/null)/etc/mpv/scripts/pushmpc-vsscript.lua"
    echo "runner removed and PushMPC files deleted. Release the machine at the provider (the 24 h minimum still applies)."
}

case ${1:-} in
    setup) shift; setup "$@" ;;
    run) cd "$RUNNER" && ./run.sh ;;
    svc) cd "$RUNNER" && ./svc.sh install && ./svc.sh start ;;
    teardown) shift; teardown "$@" ;;
    *) sed -n 2,20p "$0"; exit 2 ;;
esac
