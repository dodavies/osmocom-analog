AM_CPPFLAGS = -Wall -Wextra -Wmissing-prototypes -g $(all_includes) \
	$(SOAPY_CFLAGS)

noinst_LIBRARIES = libsdr.a

libsdr_a_SOURCES = \
	sdr_config.c \
	sdr.c

AM_CPPFLAGS += -DHAVE_SDR

if HAVE_UHD
AM_CPPFLAGS += -DHAVE_UHD

libsdr_a_SOURCES += \
	uhd.c
endif

if HAVE_SOAPY
AM_CPPFLAGS += -DHAVE_SOAPY

libsdr_a_SOURCES += \
	soapy.c
endif

