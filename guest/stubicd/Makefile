include ../mingw-shell.mk

CC := i686-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
	-Wno-cast-function-type -std=c99 -D_WIN32_WINNT=0x0501 -DWINVER=0x0501
LDFLAGS_DLL := -Wl,--subsystem,windows:5.01
LDFLAGS_EXE := -Wl,--subsystem,console:5.01
BUILD := build
ISO := xpdriver-stubicd.iso

.PHONY: all clean iso

all: $(BUILD)/stubicd.dll $(BUILD)/glprobe.exe $(BUILD)/icdinfo.exe \
	$(BUILD)/icdhijack.exe $(BUILD)/setmode.cmd $(BUILD)/README.txt

$(BUILD):
	mkdir -p $@

$(BUILD)/stubicd.dll: stubicd.c stubicd.def | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ stubicd.c stubicd.def \
		$(LDFLAGS_DLL) -Wl,--kill-at -static-libgcc

$(BUILD)/glprobe.exe: glprobe.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ glprobe.c $(LDFLAGS_EXE) \
		-static-libgcc -lopengl32 -lgdi32 -luser32

$(BUILD)/icdinfo.exe: icdinfo.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ icdinfo.c $(LDFLAGS_EXE) \
		-static-libgcc -lgdi32 -luser32

$(BUILD)/icdhijack.exe: icdhijack.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ icdhijack.c $(LDFLAGS_EXE) -static-libgcc

$(BUILD)/setmode.cmd: setmode.cmd | $(BUILD)
	cp $< $@

$(BUILD)/README.txt: README.txt | $(BUILD)
	cp $< $@

iso: all
	genisoimage -quiet -J -R -V XPDRIVER_STUB -o $(ISO) $(BUILD)
	@echo $(ISO)

clean:
	rm -rf $(BUILD)
