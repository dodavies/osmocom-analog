AM_CPPFLAGS = -Wall -Wextra -g $(all_includes)

bin_PROGRAMS = \
	golay

golay_SOURCES = \
	golay.c \
	dsp.c \
	image.c \
	main.c
golay_LDADD = \
	$(COMMON_LA) \
	../amps/libusatone.a \
	$(top_builddir)/src/liboptions/liboptions.a \
	$(top_builddir)/src/libdebug/libdebug.a \
	$(top_builddir)/src/libmobile/libmobile.a \
	$(top_builddir)/src/libosmocc/libosmocc.a \
	$(top_builddir)/src/libdisplay/libdisplay.a \
	$(top_builddir)/src/libjitter/libjitter.a \
	$(top_builddir)/src/libtimer/libtimer.a \
	$(top_builddir)/src/libselect/libselect.a \
	$(top_builddir)/src/libsamplerate/libsamplerate.a \
	$(top_builddir)/src/libemphasis/libemphasis.a \
	$(top_builddir)/src/libfm/libfm.a \
	$(top_builddir)/src/libfilter/libfilter.a \
	$(top_builddir)/src/libwave/libwave.a \
	$(top_builddir)/src/libsample/libsample.a \
	$(top_builddir)/src/libg711/libg711.a \
	$(top_builddir)/src/libaaimage/libaaimage.a \
	-lm

if HAVE_ALSA
golay_LDADD += \
	$(top_builddir)/src/libsound/libsound.a \
	$(ALSA_LIBS)

endif

if HAVE_SDR
golay_LDADD += \
	$(top_builddir)/src/libsdr/libsdr.a \
	$(top_builddir)/src/libam/libam.a \
	$(top_builddir)/src/libfft/libfft.a \
	$(UHD_LIBS) \
	$(SOAPY_LIBS)
endif

if HAVE_ALSA
AM_CPPFLAGS += -DHAVE_ALSA
endif

