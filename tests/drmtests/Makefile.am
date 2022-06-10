AM_CFLAGS = \	
	$(WARN_CFLAGS)\
	-Wno-pointer-arith \
	-fvisibility=hidden \
	-pthread \
	-I$(top_srcdir)/include/drm \
	-I$(top_srcdir)/tests \
	-I$(top_srcdir)

if HAVE_INSTALL_TESTS
bin_PROGRAMS = \
	plane_flip \
	plane_image
else
noinst_PROGRAMS = \
	plane_flip \
	plane_image
endif

DRMTEST_SOURCES := kms.c buffers.c format.c pattern.c bmp.c image.c
plane_flip_SOURCES = plane_flip.c $(DRMTEST_SOURCES)
plane_image_SOURCES = plane_image.c $(DRMTEST_SOURCES)

plane_flip_LDADD = \
	$(top_builddir)/libdrm.la
plane_image_LDADD = \
	$(top_builddir)/libdrm.la 
