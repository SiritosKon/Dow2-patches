# Dow2-patches

Small DLL patches for Dawn of War II: Retribution (DOW2.exe 3.19.1.10320, the current Steam build).
Plain C, built with MinGW. Every patch checks the bytes it is about to change and does nothing if the
exe is a different build.

    make                          # all DLLs into build/
    make STATS_HOST=your.host     # same, with the stats host baked into LobbyStats

Needs gcc-mingw-w64-i686. CI builds every push and does a dry-run publish of the DLLs as a launcher add-on.

Versions: the version of the set lives only in VERSION and needs an entry at the top of CHANGELOG.md. After a merge
to main CI tags vX.Y.Z; for X.Y.0 it also publishes a GitHub Release with the DLLs and the CHANGELOG entries since the
previous X.Y.0. Tags are set by CI, not by hand.

obsfix/
    Observer lag fix. When you spectate a live game the client asks "which slot am I in" dozens of times
    per frame, and every time it rebuilds the whole match info from the network session. ObsFix caches
    the answer for 500 ms. Write-up with addresses: docs/OBSERVER_LAG.md

lobbystats/
    Relic rating next to player names in the lobby, plus a tooltip with win rate, games and Steam hours.
    It only reads the lobby list: no kicking, no chat commands, no extra menus. Data comes from a small
    web service that reads the public Relic API (host: STATS_HOST at build time, or Server= in
    LobbyStats.ini). It runs in its own thread and doesn't touch the game's netcode.

reinforcefix/
    Auto-reinforce is lost when a squad goes through an Eldar webway gate. The auto-reinforce list is
    kept by the HUD and every tick it drops squads that have no models on the map, which is the case
    while a squad sits inside the gate. ReinforceFix skips such squads instead of dropping them.

vanilla-loader/
    winmm.dll loader. Forwards everything to the real winmm.dll and loads every *.dll from
    <game>\dow2-patches\, in vanilla and in Elite. Each patch refuses to load twice, so having the same
    DLL in Elite_DLLs as well is fine.

frameprobe/
    The diagnostics DLL I used to find the observer lag: frame times, sim ticks, a stack sampler on the
    game thread, a probe on the post-game Continue button.

cgnumbers/
    Control-group numbers above squads (experimental, skeleton). When you assign squads to a control
    group (Ctrl+1..0) the group number is drawn on that squad's banner, handy in 2v2/3v3. Draws through
    the game's Direct3D9 (DXVK-safe). Stage 1 proves the render path; reading squads/groups is stage 2.

Installing with Elite: put the DLL into Elite_DLLs\ and add it to InjectedFiles= in Elite.launchconfig.
Without Elite: winmm.dll next to DOW2.exe, patches into dow2-patches\.

MIT.
