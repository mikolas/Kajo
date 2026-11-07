# Maintainer: Mikolas Hämäläinen <mikolas@mikolas.net>
pkgname=kajo-git
_pkgname=kajo
pkgver=0.4.0.r0.g01611da
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
)
makedepends=(
    'git'
    'meson'
    'ninja'
    'pkgconf'
    'blueprint-compiler'
    'wayland-protocols'
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
source=("git+${url}.git#branch=master")
sha256sums=('SKIP')

pkgver() {
    cd "${srcdir}/${_pkgname}" 2>/dev/null || cd "${srcdir}"
    git describe --long --tags --abbrev=7 2>/dev/null | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g' || echo "0.4.0"
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
    meson install -C build --destdir "${pkgdir}"
    if [ -f "${_pkgname}/LICENSE" ]; then
        install -Dm644 "${_pkgname}/LICENSE" "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
    fi
}
