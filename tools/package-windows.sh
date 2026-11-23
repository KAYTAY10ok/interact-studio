#!/usr/bin/env bash
# Package a native x64 Windows build from MSYS2 UCRT64.
set -euo pipefail
umask 022
build=${1:-}
tag=${2:-}
output=${3:-dist}
if [[ ${MSYSTEM:-} != UCRT64 || ! $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ || ! -f "$build/InteractStudio.exe" ]]; then
    echo 'Usage in MSYS2 UCRT64: tools/package-windows.sh BUILD_DIRECTORY vMAJOR.MINOR.PATCH [output-directory]' >&2
    exit 2
fi

app_version=${tag#v}
basename="InteractStudio-$tag-windows-x86_64"
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
plugins=$(cygpath -u "$(qmake-qt5 -query QT_INSTALL_PLUGINS | tr -d '\r')")
platform="$plugins/platforms/qwindows.dll"
test -f "$platform"
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
payload="$stage/$basename"
mkdir -p "$payload"/{platforms,docs,licenses}
install -m 755 "$build/InteractStudio.exe" "$payload/"
install -m 755 "$platform" "$payload/platforms/"
install -m 644 "$repo"/{README.md,LICENSE} "$payload/"
install -m 644 "$repo"/docs/{installation.md,customization.md,design.md,interact-studio.png} "$payload/docs/"
install -m 644 "$repo/docs/releases/$tag.md" "$payload/docs/RELEASE.md"

# Include the UCRT64 DLLs imported by the executable and qwindows, then
# follow their imports too. System DLLs come from Windows itself.
files=("$payload/InteractStudio.exe" "$payload/platforms/qwindows.dll")
declare -A packages=()
packages["$(pacman -Qqo "$platform")"]=1
for ((index=0; index<${#files[@]}; index++)); do
    imports=$(objdump -p "${files[index]}")
    while read -r dll; do
        [[ -n $dll ]] || continue
        if [[ -f "$MINGW_PREFIX/bin/$dll" && ! -f "$payload/$dll" ]]; then
            install -m 755 "$MINGW_PREFIX/bin/$dll" "$payload/"
            files+=("$payload/$dll")
            packages["$(pacman -Qqo "$MINGW_PREFIX/bin/$dll")"]=1
        fi
    done < <(awk '/DLL Name:/ {print $3}' <<<"$imports")
done
packages["$(pacman -Qqo "$(command -v makensis.exe)")"]=1
for package in "${!packages[@]}"; do
    pacman -Q "$package"
    # Package recipes sometimes share one licenses directory among split packages.
    while read -r license; do
        if [[ -f $license && $license == */share/licenses/* ]]; then
            install -D -m 644 "$license" "$payload/licenses/${license#*/share/licenses/}"
        fi
    done < <(pacman -Qlq "$package")
done | LC_ALL=C sort > "$payload/licenses/MSYS2-packages.txt"
cat > "$payload/licenses/README.txt" <<'NOTICES'
Interact Studio is distributed under GPL-3.0-or-later. Its source is available
with the corresponding release tag at https://github.com/KAYTAY10ok/interact-studio.

Runtime libraries are distributed as separate DLLs. Exact MSYS2 UCRT64 package
names and versions are listed in MSYS2-packages.txt. Package metadata and
source build recipes are available at https://packages.msys2.org/ and
https://github.com/msys2/MINGW-packages. The Qt 5 base module's sources
are also available at https://download.qt.io/archive/qt/5.15/.
NOTICES

# Check the portable layout with no Qt development binaries or plugins on PATH.
(
    cd -- "$payload"
    unset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH QTDIR
    export QT_QPA_PLATFORM=windows
    export PATH=/c/Windows/System32
    /usr/bin/timeout 25 ./InteractStudio.exe --version
) > "$stage/version.txt"
tr -d '\r' < "$stage/version.txt" | grep -Fx "Interact Studio $app_version"

# Remove packaged files only. The uninstaller preserves files created by users.
uninstall="$stage/uninstall-files.nsh"
(
    cd -- "$payload"
    while IFS= read -r path; do
        printf 'Delete "$INSTDIR\\%s"\n' "${path:2}" | sed 's|/|\\|g'
    done < <(find . -type f | LC_ALL=C sort)
    while IFS= read -r path; do
        printf 'RMDir "$INSTDIR\\%s"\n' "${path:2}" | sed 's|/|\\|g'
    done < <(find . -depth -mindepth 1 -type d)
) > "$uninstall"

(
    cd -- "$stage"
    zip -qr "$basename.zip" "$basename"
)
MSYS2_ARG_CONV_EXCL='*' makensis.exe \
    "/DVERSION=$app_version" \
    "/DPAYLOAD=$(cygpath -w "$payload")" \
    "/DUNINSTALL_FILES=$(cygpath -w "$uninstall")" \
    "/DOUTPUT_FILE=$(cygpath -w "$stage/$basename-setup.exe")" \
    "$(cygpath -w "$repo/tools/windows/installer.nsi")"
install -m 644 "$stage/$basename.zip" "$stage/$basename-setup.exe" "$output/"
