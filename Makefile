# All DLLs are 32-bit (DOW2.exe is 32-bit). Needs MinGW-w64: apt install gcc-mingw-w64-i686
CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wno-unused-function -Wno-misleading-indentation -Wno-stringop-truncation
OUT = build
# stats service host for LobbyStats (can also be set later in LobbyStats.ini: [LobbyStats] Server=host)
STATS_HOST ?=
STATS = $(if $(STATS_HOST),-DSTATS_HOST='L"$(STATS_HOST)"',)

all: $(OUT)/ObsFix.dll $(OUT)/LobbyStats.dll $(OUT)/winmm.dll $(OUT)/FrameProbe.dll $(OUT)/ReinforceFix.dll
all: $(OUT)/CGNumbers.dll

$(OUT):
	mkdir -p $(OUT)
$(OUT)/ObsFix.dll: obsfix/obsfix.c | $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ $< -static-libgcc
$(OUT)/LobbyStats.dll: lobbystats/lobbystats.c | $(OUT)
	$(CC) $(CFLAGS) $(STATS) -shared -o $@ $< -lwinhttp -lpsapi -static-libgcc -Wl,--kill-at
$(OUT)/winmm.dll: vanilla-loader/winmm_proxy.c vanilla-loader/winmm.def | $(OUT)
	$(CC) -O2 -shared -o $@ $^ -static-libgcc
$(OUT)/FrameProbe.dll: frameprobe/frameprobe.c | $(OUT)
	$(CC) $(CFLAGS) -Wno-unused-variable -shared -o $@ $< -lpsapi -lwinmm -static-libgcc
$(OUT)/ReinforceFix.dll: reinforcefix/reinforcefix.c | $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ $< -static-libgcc

clean:
	rm -rf $(OUT)
.PHONY: all clean

$(OUT)/CGNumbers.dll: cgnumbers/cgnumbers.c | $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ $< -ld3d9 -static-libgcc
