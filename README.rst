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


Testing
---------

plane_flip:
  plane_flip -M <module> -P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>] -v
  - test vsynced page flipping
  plane_flip -M <module> -P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>] -u
  - draw flipping
  
plane_image:
  plane_image -M <module> -P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>] -i <bmp file>
