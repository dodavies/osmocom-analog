AM_CPPFLAGS = -Wall -Wextra -Wmissing-prototypes -g $(all_includes)

bin_PROGRAMS = \
	imts \
	imts-dialer

imts_SOURCES = \
	imts.c \
	dsp.c \
	image.c \
	main.c
imts_LDADD = \
	$(COMMON_LA) \
	$(top_builddir)/src/liboptions/liboptions.a \
	$(top_builddir)/src/libmobile/libmobile.a \
	$(top_builddir)/src/libtones/libtones.a \
	$(top_builddir)/src/libdisplay/libdisplay.a \
	$(top_builddir)/src/libjitter/libjitter.a \
	$(top_builddir)/src/libsquelch/libsquelch.a \
	$(top_builddir)/src/libsamplerate/libsamplerate.a \
	$(top_builddir)/src/libemphasis/libemphasis.a \
	$(top_builddir)/src/libfm/libfm.a \
	$(top_builddir)/src/libfilter/libfilter.a \
	$(top_builddir)/src/libwave/libwave.a \
	$(top_builddir)/src/libsample/libsample.a \
	$(top_builddir)/src/libaaimage/libaaimage.a \
	$(top_builddir)/src/liblogging/liblogging.a \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOCC_LIBS) \
	-lm

imts_dialer_SOURCES = \
	dialer.c

imts_dialer_LDADD = \
	$(COMMON_LA) \
	$(top_builddir)/src/liboptions/liboptions.a \
	$(top_builddir)/src/libwave/libwave.a \
	$(top_builddir)/src/libsample/libsample.a \
	$(top_builddir)/src/liblogging/liblogging.a \
	$(LIBOSMOCORE_LIBS) \
	-lm

if HAVE_ALSA
imts_LDADD += \
	$(top_builddir)/src/libsound/libsound.a \
	$(ALSA_LIBS)

imts_dialer_LDADD += \
	$(top_builddir)/src/libsound/libsound.a \
	$(ALSA_LIBS)
endif

if HAVE_SDR
imts_LDADD += \
	$(top_builddir)/src/libsdr/libsdr.a \
	$(top_builddir)/src/libam/libam.a \
	$(top_builddir)/src/libfft/libfft.a \
	$(UHD_LIBS) \
	$(SOAPY_LIBS)
endif

if HAVE_ALSA
AM_CPPFLAGS += -DHAVE_ALSA
endif

