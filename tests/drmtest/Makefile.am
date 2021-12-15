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
	planetest
else
noinst_PROGRAMS = \
	planetest
endif

UTIL_SOURCES := kms.c buffers.c format.c pattern.c
planetest_SOURCES = plane_test.c $(UTIL_SOURCES)

planetest_LDADD = \
	$(top_builddir)/libdrm.la 
