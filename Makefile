#---------------------------------------------------------------------------------
# Makefile  —  Alone NDS project
#
# Builds both ARM9 and ARM7 sub-projects then combines them into a .nds ROM
# using ndstool.
#
# Usage
#   make          build everything → Alone.nds
#   make arm9     build/rebuild the ARM9 ELF only
#   make arm7     build/rebuild the ARM7 ELF only
#   make clean    remove all build artefacts and the ROM
#
# Required environment variable
#   DEVKITARM     path to devkitARM installation
#                 e.g. export DEVKITARM=/opt/devkitpro/devkitARM
#
# Directory layout expected by this Makefile
#   arm9/source/  — all ARM9 .cpp/.c files (main.cpp, AudioSystem.cpp, …)
#   arm7/source/  — ARM7 .cpp/.c files (AudioArm7.cpp only)
#   common/include/ — headers shared by both CPUs (if any)
#   data/         — binary assets embedded as .o (optional)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

# ndstool is in devkitPro's tools directory
export PATH := $(DEVKITARM)/bin:$(DEVKITPRO)/tools/bin:$(PATH)

TARGET := Alone

#---------------------------------------------------------------------------------
# Top-level phony targets
#---------------------------------------------------------------------------------
.PHONY: all arm9 arm7 clean

# Default: build everything then pack the ROM
all: $(TARGET).nds
	@echo "  BUILD COMPLETED: $@"

#---------------------------------------------------------------------------------
# ROM assembly
# ndstool packs an ARM9 ELF + ARM7 ELF into a .nds ROM.
# The -7 flag provides the ARM7 ELF; -9 provides the ARM9 ELF.
#---------------------------------------------------------------------------------
$(TARGET).nds: arm9/$(TARGET).elf arm7/arm7.elf
	@echo "  PACK    $@"
	ndstool -c $@ -9 arm9/$(TARGET).elf -7 arm7/arm7.elf

#---------------------------------------------------------------------------------
# ARM9 sub-project
# Delegates to Makefile.arm9 (renamed from the original Makefile).
#---------------------------------------------------------------------------------
arm9/$(TARGET).elf: arm9
arm9:
	@$(MAKE) -f Makefile.arm9

#---------------------------------------------------------------------------------
# ARM7 sub-project
# Delegates to Makefile.arm7.
#---------------------------------------------------------------------------------
arm7/arm7.elf: arm7
arm7:
	@$(MAKE) -f Makefile.arm7

#---------------------------------------------------------------------------------
clean:
	@echo "  CLEAN"
	@$(MAKE) -f Makefile.arm9 clean
	@$(MAKE) -f Makefile.arm7 clean
	@rm -f $(TARGET).nds
