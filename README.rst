libdrm-2.4.97-branch
----------------------------------
rohan
- add plantest application in [tests/drmtest] to test render with SetPlane API.
  

libdrm - userspace library for drm
----------------------------------

This is libdrm, a userspace library for accessing the DRM, direct rendering
manager, on Linux, BSD and other operating systems that support the ioctl
interface.
The library provides wrapper functions for the ioctls to avoid exposing the
kernel interface directly, and for chipsets with drm memory manager, support
for tracking relocations and buffers.
New functionality in the kernel DRM drivers typically requires a new libdrm,
but a new libdrm will always work with an older kernel.

libdrm is a low-level library, typically used by graphics drivers such as
the Mesa drivers, the X drivers, libva and similar projects.


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

Testing
---------

plane_flip:
  plane_flip -M <module> -P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>] -v
  - test vsynced page flipping
  plane_flip -M <module> -P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>] -u
  - draw flipping
  
plane_image:
  plane_image -M <module> -P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>] -i <bmp file>
