#!/usr/bin/env bash
# Install development headers for the Debian 13 cloud image without root.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
source /etc/os-release
if [[ ${ID:-} != debian || ${VERSION_CODENAME:-} != trixie ]]; then
    echo 'This installer targets Debian 13. On a system with Qt5Widgets and qtermwidget5 development packages, run make directly.' >&2
    exit 1
fi
sdk=${INTERACT_SDK:-$(dirname "$repo")/.interact-sdk}
mkdir -p "$sdk" build
sdk=$(cd "$sdk" && pwd -P)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/lists/partial" "$work/debs/partial"
for suite in trixie bookworm; do
    echo "deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian $suite main"
done > "$work/sources.list"
cat > "$work/config" <<APT
Dir::Etc::sourcelist "$work/sources.list";
Dir::Etc::sourceparts "-";
Dir::State::lists "$work/lists";
Dir::Cache::archives "$work/debs";
Acquire::Languages "none";
#clear APT::Update::Post-Invoke;
APT
apt-get -c "$work/config" update
cd "$work/debs"
# Qt5 is preinstalled on the cloud image. Match its headers exactly.
qt_version=$(dpkg-query -W -f='${Version}' libqt5widgets5t64)
apt-get -c "$work/config" download "qtbase5-dev=$qt_version" \
    libqtermwidget5-1-dev/bookworm libqtermwidget5-1/bookworm \
    qtermwidget5-data/bookworm libutf8proc2/bookworm libgl-dev/trixie
for package in ./*.deb; do
    dpkg-deb -x "$package" "$sdk"
done
arch=$(c++ -print-multiarch)
lib="$sdk/usr/lib/$arch"
for component in Core Gui Widgets Network Test; do
    # The -dev package provides a relative symlink to the system runtime.
    soname=$(readlink "$lib/libQt5$component.so")
    test -f "/usr/lib/$arch/$soname"
    ln -sfn "/usr/lib/$arch/$soname" "$lib/$soname"
done
cd "$repo"
printf 'INTERACT_SDK := %s\n' "$sdk" > build/sdk.mk
for data in kb-layouts color-schemes; do
    ln -sfnT "$sdk/usr/share/qtermwidget5/$data" "build/$data"
done
