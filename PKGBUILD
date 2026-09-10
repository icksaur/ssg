pkgname=ssg
pkgver=0.1.0
pkgrel=1
pkgdesc='Fast, modern terminal text editor'
arch=('x86_64')
license=('MIT')
depends=('gcc-libs' 'glibc' 'lua54')
makedepends=('cmake' 'ninja')
source=()
sha256sums=()

_builddir="${BUILDDIR:-$startdir}/build-package"

build() {
    cmake -S "$startdir" -B "$_builddir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DSSG_CCACHE=OFF \
        -DSSG_WERROR=OFF
    cmake --build "$_builddir"
}

check() {
    ctest --test-dir "$_builddir" --output-on-failure
}

package() {
    DESTDIR="$pkgdir" cmake --install "$_builddir"
}
