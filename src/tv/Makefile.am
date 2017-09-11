AM_CPPFLAGS = -Wall -Wextra -g $(all_includes) -fstack-check

bin_PROGRAMS = \
	osmotv

osmotv_SOURCES = \
	bas.c \
	fubk.c \
	font.c \
	vcr.c \
	image.c \
	tv_modulate.c \
	channels.c \
	main.c
osmotv_LDADD = \
	$(COMMON_LA) \
	$(top_builddir)/src/common/libimage.a \
	$(top_builddir)/src/common/libcommon.a \
	$(ALSA_LIBS) \
	$(UHD_LIBS) \
	$(SOAPY_LIBS) \
	$(GRAPHICSMAGICK_LIBS) $(IMAGEMAGICK_LIBS) \
	-lm

if HAVE_SDR
AM_CPPFLAGS += -DHAVE_SDR
endif

if HAVE_UHD
AM_CPPFLAGS += -DHAVE_UHD
endif

if HAVE_SOAPY
AM_CPPFLAGS += -DHAVE_SOAPY
endif

if ENABLE_MAGICK
AM_CPPFLAGS += -DWITH_MAGICK
endif
