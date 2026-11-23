#!/usr/bin/env bash
# Package a Debian 12 native build for x86-64 Linux.
set -euo pipefail
umask 022
export LC_ALL=C
cd -- "$(dirname -- "$0")/.."
tag=${1:-}
output=${2:-dist}
if [[ ! $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo 'Usage: tools/package-linux.sh vMAJOR.MINOR.PATCH [output-directory]' >&2
    exit 2
fi
version=${tag#v}
archive="interact-studio-$tag-linux-x86_64"
deb="interact-studio_${version}_amd64.deb"

source /etc/os-release
if [[ ${ID:-} != debian || ${VERSION_CODENAME:-} != bookworm || $(dpkg --print-architecture) != amd64 ]]; then
    echo 'Build release packages on Debian 12 (Bookworm), amd64.' >&2
    exit 1
fi

binary=build/interact-studio
if [[ ! -f LICENSE ]]; then
    echo 'The release must include the project license.' >&2
    exit 1
fi

if [[ ! -x $binary ]] ||
   ! readelf -h "$binary" | grep -q 'Machine:.*X86-64' ||
   readelf -d "$binary" | grep -E '\((RPATH|RUNPATH)\)'; then
    echo 'Build interact-studio against the Debian 12 system Qt and QTermWidget before packaging.' >&2
    exit 1
fi
if [[ $(QT_QPA_PLATFORM=offscreen "$binary" --version) != "Interact Studio $version" ]]; then
    echo "Executable version differs from $tag" >&2
    exit 1
fi

mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
if [[ -z ${SOURCE_DATE_EPOCH:-} ]]; then
    SOURCE_DATE_EPOCH=$(git log -1 --format=%ct 2>/dev/null || date +%s)
fi
export SOURCE_DATE_EPOCH

copy_docs() {
    local dest=$1
    install -d "$dest"
    install -m 644 README.md "$dest/"
    install -d "$dest/docs"
    install -m 644 docs/*.md docs/interact-studio.png "$dest/docs/"
    if [[ -f "docs/releases/$tag.md" ]]; then
        install -d "$dest/docs/releases"
        install -m 644 "docs/releases/$tag.md" "$dest/docs/releases/"
    fi
    install -m 644 LICENSE "$dest/"
    if [[ -f CHANGELOG.md ]]; then install -m 644 CHANGELOG.md "$dest/"; fi
}

portable="$stage/$archive"
copy_docs "$portable"
install -m 755 "$binary" "$portable/interact-studio"
strip --strip-unneeded "$portable/interact-studio"
cat > "$portable/INSTALL.txt" <<'INSTRUCTIONS'
Interact Studio — Linux x86-64

Debian 12 desktop dependencies:
    sudo apt install libqt5widgets5 libqt5network5 libqtermwidget5-1 qtermwidget5-data git ripgrep

Launch from the extracted folder:
    ./interact-studio

Open a project with:
    ./interact-studio /path/to/project

The Debian package is an alternative that installs the dependencies and the
desktop shortcut automatically. See the enclosed README.md for IDE usage.
INSTRUCTIONS
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 \
    -C "$stage" -cf - "$archive" | gzip -n > "$stage/$archive.tar.gz"

package="$stage/deb"
install -d "$package/DEBIAN" "$package/usr/bin" "$package/usr/share/applications"
install -m 755 "$portable/interact-studio" "$package/usr/bin/interact-studio"
install -m 644 tools/linux/interact-studio.desktop "$package/usr/share/applications/"
copy_docs "$package/usr/share/doc/interact-studio"
install -m 644 tools/linux/copyright "$package/usr/share/doc/interact-studio/copyright"
cat > "$package/DEBIAN/control" <<CONTROL
Package: interact-studio
Version: $version
Section: devel
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.36), libstdc++6 (>= 12), libgcc-s1, libqt5widgets5 (>= 5.15), libqt5gui5 (>= 5.15), libqt5core5a (>= 5.15), libqt5network5 (>= 5.15), libqtermwidget5-1 (>= 1.2.0), qtermwidget5-data, git, ripgrep
Installed-Size: $(du -sk "$package/usr" | cut -f1)
Maintainer: Interact Studio <KAYTAY10ok@users.noreply.github.com>
Homepage: https://github.com/KAYTAY10ok/interact-studio
Description: Native editor and development workspace
 Project search, Git, customizable keyboard shortcuts and a terminal
 in a native Qt Widgets application.
CONTROL
dpkg-deb --root-owner-group --build "$package" "$stage/$deb"

install -m 644 "$stage/$archive.tar.gz" "$stage/$deb" "$output/"
printf 'Packages: %s\n' "$output/$archive.tar.gz" "$output/$deb"
