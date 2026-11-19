# Maintainer: Mikolas Hämäläinen <mikolas@mikolas.net>
pkgname=kajo-git
_pkgname=kajo
pkgver=0.1.0.r117.g456be9b
pkgrel=1
pkgdesc="Kajo: native GTK4 Wayland shell and app suite (IDE, files, player, settings, greeter) for the Niri compositor"
arch=('x86_64' 'x86_64_v3')
url="https://github.com/mikolas/Kajo"
license=('MIT')
depends=(
    'gtk4'
    'gtk4-layer-shell'
    'gtksourceview5'
    'json-glib'
    'glib2'
    'libpulse'
    'libxkbcommon'
    'wayland'
    'pam'
    'poppler-glib'
    'libarchive'
    'mpv'
    # kajo-ide tree-sitter engine. The first six are in the official/CachyOS
    # repositories, the rest are AUR packages (install with an AUR helper).
    # Each grammar is optional at build time; a missing one only disables
    # tree-sitter highlighting for that language.
    'tree-sitter'
    'tree-sitter-c'
    'tree-sitter-bash'
    'tree-sitter-javascript'
    'tree-sitter-lua'
    'tree-sitter-rust'
    'tree-sitter-cpp'
    'tree-sitter-css'
    'tree-sitter-go'
    'tree-sitter-html'
    'tree-sitter-java'
    'tree-sitter-json'
    'tree-sitter-toml'
    'tree-sitter-typescript'
    'tree-sitter-yaml'
    'tree-sitter-m68k'
    'libglvnd'
    # kajo-comp: its own core, plus the shared libraries of the wlroots fork it
    # links statically (subprojects/wlroots; docs/spec_wlroots_fork.md K6).
    'talloc'
    'pcre2'
    'libpng'
    'libdrm'
    'libinput'
    'seatd'
    'libdisplay-info'
    'libliftoff'
    'lcms2'
    'pixman'
    'mesa'
    'vulkan-icd-loader'
    'libxcb'
    'xcb-util-wm'
    'xcb-util-renderutil'
    'xcb-util-errors'
    'xorg-xwayland'
)
makedepends=(
    'git'
    'meson'
    'ninja'
    'pkgconf'
    'blueprint-compiler'
    'wayland-protocols'
    # wlroots fork build
    'glslang'
    'vulkan-headers'
    'hwdata'
)
optdepends=(
    'ripgrep: kajo-ide search, live grep, hover and go-to-definition'
    'git: kajo-ide file tree status'
    'm68k-lsp-server: kajo-ide Motorola 68000 language server (vasmm68k_mot from vasm is needed for its build task)'
    'gvfs: kajo-files network/trash locations'
    'gvfs-smb: kajo-files SMB shares'
    'udisks2: kajo-files drive mounting'
    'greetd: login manager for kajo-greeter'
    'niri: the Wayland compositor Kajo targets'
)
provides=("${_pkgname}")
conflicts=("${_pkgname}")
source=("${_pkgname}::git+${url}.git#branch=master"
        # Kajo's wlroots fork. The commit actually built is the submodule pin
        # recorded in Kajo (subprojects/wlroots), not a ref named here.
        "wlroots::git+https://github.com/mikolas/wlroots.git")
sha256sums=('SKIP'
            'SKIP')

pkgver() {
    cd "${_pkgname}"
    # <base>.r<total commits>.g<hash>, e.g. 0.1.0.r117.g456be9b. <base> is the
    # latest release tag (`v<version>`; pre-release tags such as `v0.2.0-rc1`
    # are skipped, they would sort above the final release) or, before the
    # first one, the meson project version; other tags (e.g.
    # `pre-startup-fixes`) are ignored. The total commit count keeps the version
    # monotonic when the first tag lands.
    local base
    base="$(git describe --tags --abbrev=0 --match 'v[0-9]*' --exclude '*-*' 2>/dev/null | sed 's/^v//')"
    if [ -z "${base}" ]; then
        base="$(sed -n "s/^ *version: *'\([0-9][^']*\)'.*/\1/p" meson.build | head -1)"
    fi
    printf '%s.r%s.g%s' "${base:-0}" "$(git rev-list --count HEAD)" "$(git rev-parse --short=7 HEAD)"
}

prepare() {
    cd "${_pkgname}"
    git submodule init
    git config submodule.subprojects/wlroots.url "${srcdir}/wlroots"
    git -c protocol.file.allow=always submodule update
}

build() {
    # CachyOS makepkg automatically passes CachyOS CFLAGS (-march=x86-64-v3 -O3 -flto) via arch-meson
    arch-meson "${_pkgname}" build \
        -Doptimization=3 \
        -Db_lto=true \
        -Db_pie=true
    meson compile -C build
}

package() {
    # --skip-subprojects: wlroots is linked statically into kajo-comp; without
    # the flag its headers, static library and pkg-config files would be
    # installed and collide with the system wlroots package.
    meson install -C build --destdir "${pkgdir}" --skip-subprojects
    if [ -f "${_pkgname}/LICENSE" ]; then
        install -Dm644 "${_pkgname}/LICENSE" "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
    fi
    # Notices of statically linked and vendored third-party code
    install -Dm644 "${_pkgname}/THIRD_PARTY_NOTICES.md" "${pkgdir}/usr/share/licenses/${pkgname}/THIRD_PARTY_NOTICES.md"
    install -Dm644 "${_pkgname}/subprojects/wlroots/LICENSE" "${pkgdir}/usr/share/licenses/${pkgname}/wlroots-LICENSE"
}
