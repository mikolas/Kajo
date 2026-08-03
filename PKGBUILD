# Maintainer: Mikolas Hämäläinen <mikolas@mikolas.net>
pkgname=kajo-git
_pkgname=kajo
pkgver=0.4.0.r0.g01611da
pkgrel=1
pkgdesc="High-performance Wayland desktop shell for Niri compositor"
arch=('x86_64' 'x86_64_v3')
url="https://github.com/mikolas/desktop"
license=('MIT')
depends=(
    'gtk4'
    'gtk4-layer-shell'
    'json-glib'
    'libpulse'
    'glib2'
)
makedepends=(
    'git'
    'meson'
    'ninja'
    'pkgconf'
    'blueprint-compiler'
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
