# DoW2 Retribution / Elite: client-side fixes and tools (source)

All of these are small 32-bit DLLs for DOW2.exe 3.19.1.10320 (current Steam build), written in plain C
and built with MinGW. They are loaded through Elite's DLL injector (`Elite_DLLs` + `InjectedFiles=` in
`Elite.launchconfig`). Each one checks the exact bytes it patches and does nothing on another game build.

Build everything: `make` (needs `gcc-mingw-w64-i686`). GitHub Actions builds every push; tagged releases (`v*`) attach the DLLs and their SHA-256.

| Folder | What | Build |
|---|---|---|
| `obsfix/` | **Observer lag fix.** Caches the observer's "which slot am I" lookup, which rebuilt the whole match info dozens of times per frame. See [docs/OBSERVER_LAG.md](docs/OBSERVER_LAG.md). | `i686-w64-mingw32-gcc -O2 -shared -o ObsFix.dll obsfix.c -static-libgcc` |
| `lobbystats/` | **Lobby stats.** Relic rating next to names and a stats tooltip. The full build also has an optional host filter and kick. Lite build = tooltip + rating only. | full: `i686-w64-mingw32-gcc -O2 -shared -o LobbyStats.dll lobbystats.c -lwinhttp -lpsapi -static-libgcc -Wl,--kill-at` — lite: add `-DLITE` |
| `vanilla-loader/` | `winmm.dll` loader for vanilla (no Elite): forwards all 193 winmm exports to the system DLL and loads every `*.dll` from `<game>\dow2-patches\` (ObsFix, LobbyStats, ...). Does nothing when `-modname` is on the command line (Elite loads its own DLLs). | `i686-w64-mingw32-gcc -O2 -shared -o winmm.dll winmm_proxy.c winmm.def -static-libgcc` |
| `frameprobe/` | **Diagnostics** used to find the observer lag: frame times, sim ticks, a stack sampler of the game-loop thread, a Continue-button probe. | `i686-w64-mingw32-gcc -O2 -shared -o FrameProbe.dll frameprobe.c -lpsapi -lwinmm -static-libgcc` |

The launcher (Rust/Tauri, MIT) lives in its own repository, `elite-launcher`.

LobbyStats reads ratings from a small web service that reads the public Relic API. Its host is set at
build time (`make STATS_HOST=your.host`) or in `LobbyStats.ini` (`[LobbyStats] Server=your.host`). It runs in a background thread over plain HTTP and never touches the game's netcode or stat reporting.

License: MIT. Author: pivolan.
