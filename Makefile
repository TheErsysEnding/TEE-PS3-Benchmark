#---------------------------------------------------------------------------------
# TEE PS3 Benchmark (until V9.0 "TEE SPE Bench") - PSL1GHT build, after TEE PS3 Live's Makefile.
# "make" builds the .self, "make pkg" the installable .pkg. Paths must not contain spaces:
# ./build.sh takes care of that. spu/ is the SPU program, built first and linked in as spu.bin.
# AUTOTEST=1: presses the buttons itself (for RPCS3): menu, info page, the series from PPU to 6 SPEs,
# a 20 s stress test with the maximum and one with the mixed load, pictures on the way, then quits.
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

TARGET		:=	tee-ps3-benchmark
BUILD		:=	build
SOURCES		:=	source
INCLUDES	:=	source
DATA		:=	data
SHADERS		:=	shaders

TITLE		:=	TEE PS3 Benchmark
APPID		:=	TEEBENCH1
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000

# the XMB pictures, drawn by tools/make_xmb_images.swift: the icon, and everything in pkgfiles/
# (PIC1.PNG, the background while the app is selected) goes into the package as it is
ICON0		:=	$(CURDIR)/assets/ICON0.PNG
PKGFILES	:=	$(CURDIR)/pkgfiles

CFLAGS		=	-O2 -Wall -std=gnu99 -mcpu=cell $(MACHDEP) $(INCLUDE)
ifdef AUTOTEST
# in RPCS3, 127.0.0.1 is the Mac: stand-ins for webMAN and for the time server listen there on 18080 and
# 12300 (ports 80 and 123 need root)
CFLAGS		+=	-DBENCH_AUTOTEST -DWEBMAN_PORT=18080 -DNTP_TEST_IP=0x7F000001u -DNTP_PORT=12300
endif
CXXFLAGS	=	$(CFLAGS)

LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS	:=	-lrsx -lgcm_sys -lio -lsysutil -lnet -lsysmodule -lrt -llv2 -lm

LIBDIRS	:=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
					$(foreach dir,$(SHADERS),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(filter-out spu.bin,$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))) spu.bin
# vertex programs in assembler (cgcomp cannot compile Cg here: NVIDIA's toolkit is missing)
VASMFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.vasm)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(BINFILES)) $(VASMFILES:.vasm=.vpo.o) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) bin clean run pkg

$(BUILD): bin
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

bin:
	@mkdir -p $(DATA)
	@$(MAKE) --no-print-directory -C spu

clean:
	@echo clean ...
	@$(MAKE) --no-print-directory -C spu clean
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT)*.pkg $(DATA)/spu.bin

run:
	ps3load $(OUTPUT).self

pkg:	$(BUILD) $(OUTPUT).pkg

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

# spu.bin.o also writes spu_bin.h, which bench.c includes; pass.vpo.o writes pass_vpo.h for rsx_load.c
bench.o: spu.bin.o
rsx_load.o: pass.vpo.o ui.vpo.o vload.vpo.o
ui.o: ui.vpo.o ui_atlas.bin.o

%.vpo	:	%.vasm
	@echo $(notdir $<)
	@$(CGCOMP) -v -a $< $@

%.vpo.o	:	%.vpo
	@echo $(notdir $<)
	@$(bin2o)

%.bin.o	:	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
