PLUGINOBJECTS = \
	ReadFFmpeg.o FFmpegFile.o WriteFFmpeg.o PixelFormat.o \
	GenericReader.o GenericWriter.o GenericOCIO.o SequenceParsing.o ofxsMultiPlane.o
PLUGINNAME = FFmpeg

TOP_SRCDIR = ..
include $(TOP_SRCDIR)/Makefile.master

RESOURCES = \
fr.inria.openfx.ReadFFmpeg.png \
fr.inria.openfx.ReadFFmpeg.svg \
fr.inria.openfx.WriteFFmpeg.png \
fr.inria.openfx.WriteFFmpeg.svg 

CXXFLAGS += $(FFMPEG_CXXFLAGS)
LINKFLAGS += $(FFMPEG_LINKFLAGS)

USE_OCIO ?= true

ifeq ($(USE_OCIO), true)
	CXXFLAGS += $(OCIO_CXXFLAGS)
	LINKFLAGS += $(OCIO_LINKFLAGS)
endif
