==============
drmModeFormats
==============

------------------------------------------------------------
read and hold data from DRM/KMS "IN_FORMATS" blobs version 1
------------------------------------------------------------

:Date: December 2020
:Manual section: 3
:Manual group: Direct Rendering Manager

Synopsis
========

``#include <xf86drmMode.h>``

``int drmModePopulateFormats(drmModePropertyBlobPtr ptr, drmModeFormatsPtr *out_formats);``

``drmModeFormatsPtr drmModeAllocFormats(const uint32_t formats_count);``

``void drmModeFreeFormats(drmModeFormatsPtr ptr);``

Description
===========

``drmModeAllocFormats`` allocates and returns a ``drmModeFormats`` structure
ready to be populated via ``drmModePopulateFormats``. ``drmModeFreeFormats``
frees memeory allocated for ``drmModeFormats``. ``drmModeFormats`` contains the
following fields:

::

   typedef struct _drmModeFormats {
      uint32_t count;
      struct {
         uint32_t format;
         uint32_t count_modifiers;
         uint64_t *modifiers;
      } formats[];
   } drmModeFormats, *drmModeFormatsPtr;

The *formats* field is a flexible array member, it will be dynamically sized at
allocation time by ``drmModeAllocFormats``. ``drmModeFormats`` supports C99
standard and above.

Return Value
============

``drmModePopulateFormats`` returns *0* on success, *EINVAL* if either any input
pointer is *NULL*, or if no formats are available in the blob.
``drmModePopulateFormats`` returns *ENOMEM* when ``drmModeAllocFormats``
fails to allocate memory.

Compatibility
=============

``drmModeFormats`` supports the DRM/KMS property *IN_FORMATS* advertised in
kernel blobs version 1. Newer DRM/KMS blobs are expected to be backwards
compatible and not to break userspace applications relying on previous versions
of the blob. Likewise, newer versions of libdrm are expected not to break client
applications linked against an older version. When applications depend on libdrm
and make use of drmModeFormats functionality for populating DRM/KMS *IN_FORMATS*
data version 1, they can assume such functionality to be preserved on upcoming
version.

Reporting Bugs
==============

Bugs in this function should be reported to
https://gitlab.freedesktop.org/mesa/drm/-/issues

See Also
========

**drm**\ (7), **drm-kms**\ (7), **drmModeGetFB**\ (3), **drmModeAddFB**\ (3),
**drmModeAddFB2**\ (3), **drmModeRmFB**\ (3), **drmModeDirtyFB**\ (3),
**drmModeGetCrtc**\ (3), **drmModeSetCrtc** (3), **drmModeGetEncoder** (3),
**drmModeGetConnector**\ (3)
