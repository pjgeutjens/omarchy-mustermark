#!/bin/sh
# Run through a function so a truncated download cannot execute half an installer.
main() (
    set -eu
    version=0.2.0
    prefix=${HOME:?HOME is required}/.local
    action=install
    install_deps=no
    archive=
    checksum=
    usage() {
        echo 'Usage: install.sh [--version VERSION] [--prefix PATH] [--install-deps]'
        echo '       install.sh --uninstall [--prefix PATH]'
        echo '       install.sh --archive FILE --sha256 HASH [--prefix PATH]'
    }
    fail() { echo "mustermark: $*" >&2; exit 1; }
    while [ "$#" -gt 0 ]; do
        case $1 in
            --version|--prefix|--archive|--sha256)
                [ "$#" -ge 2 ] || fail "Missing value for $1"
                case $1 in
                    --version) version=${2#v} ;;
                    --prefix) prefix=$2 ;;
                    --archive) archive=$2 ;;
                    --sha256) checksum=$2 ;;
                esac
                shift 2 ;;
            --install-deps) install_deps=yes; shift ;;
            --uninstall) action=uninstall; shift ;;
            --help|-h) usage; exit 0 ;;
            *) usage >&2; fail "Unknown option: $1" ;;
        esac
    done
    case $prefix in /*) ;; *) fail 'Prefix must be an absolute path' ;; esac
    # Desktop Exec and Icon fields need additional escaping for these characters.
    case $prefix in *[!a-zA-Z0-9_./-]*) fail 'Prefix may contain only letters, digits, /, _, ., and -' ;; esac
    case $version in ''|*[!0-9.]*|.*|*..*) fail 'Version must be a numeric release such as 0.2.0' ;; esac
    [ "$(id -u)" -ne 0 ] || fail 'Run as your normal user, not root'
    root=$prefix/lib/mustermark
    link=$prefix/bin/mustermark
    desktop=$prefix/share/applications/mustermark.desktop
    icon=$prefix/share/icons/hicolor/scalable/apps/mustermark.svg
    license=$prefix/share/licenses/mustermark/LICENSE
    mkdir -p "$prefix/lib"
    lock=$prefix/lib/.mustermark-install-lock
    mkdir "$lock" 2>/dev/null || fail "Another installation is running, or a stale lock remains: $lock"
    temp=
    trap '[ -z "$temp" ] || rm -rf -- "$temp"; rmdir "$lock"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    owned_link() { [ -L "$1" ] && [ "$(readlink "$1")" = "$2" ]; }
    refresh() {
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database "$prefix/share/applications" >/dev/null 2>&1 || :
        fi
        if command -v gtk-update-icon-cache >/dev/null 2>&1; then
            gtk-update-icon-cache -f -t "$prefix/share/icons/hicolor" >/dev/null 2>&1 || :
        fi
    }
    if [ "$action" = uninstall ]; then
        [ ! -L "$root" ] && [ -f "$root/.installer-owned" ] || fail 'No installation owned by this installer at this prefix'
        for pair in "bin/mustermark" "share/applications/mustermark.desktop" "share/icons/hicolor/scalable/apps/mustermark.svg" "share/licenses/mustermark/LICENSE"; do
            if owned_link "$prefix/$pair" "$root/current/$pair"; then rm -- "$prefix/$pair"; fi
        done
        rm -rf -- "$root"
        refresh
        echo 'Mustermark uninstalled. Documents, assets, settings, and recovery files were preserved.'
        exit 0
    fi
    [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] || fail 'This early installer supports Linux x86_64 only'
    command -v pacman >/dev/null 2>&1 || fail 'This early installer supports Arch Linux and Omarchy only'
    for tool in curl tar sha256sum mktemp; do command -v "$tool" >/dev/null 2>&1 || fail "Missing required tool: $tool"; done
    if [ -e "$root" ] || [ -L "$root" ]; then
        [ ! -L "$root" ] && [ -f "$root/.installer-owned" ] || fail "Refusing to replace an unmanaged directory: $root"
    fi
    for pair in "bin/mustermark" "share/applications/mustermark.desktop" "share/icons/hicolor/scalable/apps/mustermark.svg" "share/licenses/mustermark/LICENSE"; do
        if [ -e "$prefix/$pair" ] || [ -L "$prefix/$pair" ]; then
            owned_link "$prefix/$pair" "$root/current/$pair" || fail "Existing unmanaged file: $prefix/$pair. Choose another --prefix or move that installation first."
        fi
    done
    missing=
    for package in base-devel cmake ninja qt6-base qt6-declarative qt6-svg qt6-webengine cmark-gfm; do
        if ! pacman -Q "$package" >/dev/null 2>&1; then missing="$missing $package"; fi
    done
    if [ -n "$missing" ]; then
        echo "Missing system packages:$missing"
        if [ "$install_deps" != yes ]; then
            printf 'Install these with sudo pacman? [y/N] ' > /dev/tty || fail 'Rerun with --install-deps or install the listed packages yourself'
            read -r answer < /dev/tty || fail 'No confirmation received'
            case $answer in y|Y|yes) ;; *) fail 'Dependency installation declined' ;; esac
        fi
        # Package names above are a fixed list, not user input.
        sudo pacman -S --needed $missing < /dev/tty
    fi
    temp=$(mktemp -d)
    if [ -n "$archive" ]; then
        [ -n "$checksum" ] || fail '--archive requires --sha256'
        cp -- "$archive" "$temp/source.tar.gz"
    else
        url=https://github.com/pjgeutjens/omarchy-mustermark/releases/download/v$version
        curl --fail --location --proto '=https' --tlsv1.2 --retry 3 "$url/mustermark-$version.tar.gz" -o "$temp/source.tar.gz"
        curl --fail --location --proto '=https' --tlsv1.2 --retry 3 "$url/SHA256SUMS" -o "$temp/SHA256SUMS"
        checksum=$(awk -v file="mustermark-$version.tar.gz" '$2 == file { print $1 }' "$temp/SHA256SUMS")
    fi
    [ "${#checksum}" = 64 ] || fail 'Expected one SHA-256 checksum'
    case $checksum in *[!a-fA-F0-9]*) fail 'Invalid SHA-256 checksum' ;; esac
    printf '%s  %s\n' "$checksum" "$temp/source.tar.gz" | sha256sum --check --status || fail 'Source checksum mismatch'
    # Reject unexpected paths and non-regular archive members before extraction.
    tar -tzf "$temp/source.tar.gz" > "$temp/members"
    awk -v root="mustermark-$version/" 'index($0, root) != 1 || $0 ~ /(^|\/)\.\.(\/|$)/ {bad=1} END {exit bad}' "$temp/members" || fail 'Unsafe archive paths'
    tar -tvzf "$temp/source.tar.gz" | awk 'substr($0,1,1) != "-" && substr($0,1,1) != "d" {bad=1} END {exit bad}' || fail 'Archive contains links or special files'
    tar -xzf "$temp/source.tar.gz" --no-same-owner --no-same-permissions -C "$temp"
    cmake -S "$temp/mustermark-$version" -B "$temp/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build "$temp/build" --parallel 2
    DESTDIR="$temp/stage" cmake --install "$temp/build"
    "$temp/stage/usr/bin/mustermark" --version
    # Build and stage everything before switching the active installation.
    mkdir -p "$root" "$prefix/bin" "$(dirname "$desktop")" "$(dirname "$icon")" "$(dirname "$license")"
    touch "$root/.installer-owned"
    release=$(mktemp -d "$root/release-$version-XXXXXXXX")
    cp -a "$temp/stage/usr/." "$release/"
    sed "s|^Exec=mustermark|Exec=$link|; s|^Icon=mustermark$|Icon=$icon|" \
        "$release/share/applications/mustermark.desktop" > "$temp/desktop"
    cp "$temp/desktop" "$release/share/applications/mustermark.desktop"
    ln -s "$release" "$root/current.new"
    mv -Tf "$root/current.new" "$root/current"
    for pair in "bin/mustermark" "share/applications/mustermark.desktop" "share/icons/hicolor/scalable/apps/mustermark.svg" "share/licenses/mustermark/LICENSE"; do
        if [ ! -L "$prefix/$pair" ]; then ln -s "$root/current/$pair" "$prefix/$pair"; fi
    done
    refresh
    echo "Installed Mustermark $version at $link"
    echo "Open Mustermark from your application launcher, or run: $link"
    case :$PATH: in *:"$prefix/bin":*) ;; *) echo "Add $prefix/bin to PATH to use the mustermark command from a terminal." ;; esac
    echo "Uninstall: sh install.sh --uninstall --prefix $prefix"
)
main "$@"
