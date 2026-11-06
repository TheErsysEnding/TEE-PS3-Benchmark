#---------------------------------------------------------------------------------
# SPU program of TEE SPE Bench, after PSL1GHT's samples/spu/sputhread/spu/Makefile.
# Builds ../data/spu.bin, which the PPU build links in (spu_bin.h).
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/spu_rules

TARGET		:=	tee-spu-bench
BUILD		:=	build
SOURCES		:=	source
DATA		:=
INCLUDES	:=	../source

CFLAGS		=	-O3 -Wall -funroll-loops -std=gnu99 $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)

LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS	:= -lsputhread

LIBDIRS	:=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD	:=	$(CC)

export OFILES	:=	$(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf

else

DEPENDS	:= $(OFILES:.o=.d)

# the linker notes the output's full path as the SPU program's name (.note.spu_name): not into the app
../../data/spu.bin: $(OUTPUT).elf
	@$(PREFIX)objcopy -R .note.spu_name $< $@

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
