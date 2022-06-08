libdrm - clone form https://gitlab.freedesktop.org/mesa/drm
----------------------------------

Compiling
---------
Checkout 2.4.97-rohan, this branch based on libdrm-2.4.97 tag

$> git checkout -b 2.4.97-rohan origin/2.4.97-rohan

Configure for the ARM architecture:

$> ./autogen.sh 
 --target=arm-linux-gnueabihf
 --host=arm-linux-gnueabihf
 --build=x86_64-pc-linux-gnu
 --prefix=/usr
 --exec-prefix=/usr
 --sysconfdir=/etc
 --localstatedir=/var
 --program-prefix=
 --disable-gtk-doc
 --disable-gtk-doc-html
 --disable-doc
 --disable-docs
 --disable-documentation
 --with-xmlto=no
 --with-fop=no
 --disable-dependency-tracking
 --enable-ipv6
 --disable-nls
 --disable-cairo-tests
 --disable-manpages
 --disable-intel
 --disable-radeon
 --disable-amdgpu
 --disable-nouveau
 --disable-vmwgfx
 --disable-omap-experimental-api
 --disable-etnaviv-experimental-api
 --disable-exynos-experimental-api
 --disable-freedreno
 --disable-tegra-experimental-api
 --disable-vc4
 --disable-udev
 --disable-valgrind
 --enable-install-test-programs
 --disable-static --enable-shared

Then build and install:

$> make

$> make DESTDIR=<path> install

