# Observer lag in Dawn of War II: Retribution: root cause and fix

## Symptoms

- Spectating a live multiplayer game (observer slot) runs at about half the frame rate of the same match played back as a replay on the same PC.
- The bigger the match (more players and stations), the worse it gets. The frame rate also drops as the match goes on and armies grow.
- Players in the same match are not affected, only observers.

## Measurement

The numbers come from FrameProbe, a small diagnostic DLL. It hooks `shark::update` once per frame and samples the game-loop thread's stack every 20 ms. The test match was 3v3 on Khatrin Purification Plant (Elite 3.1). The same PC first spectated the match live, then watched the replay of it.

| | Live observer | Replay of the same match |
|---|---|---|
| FPS, early game | 62 | 130 |
| FPS, late game | 31 | 72 |
| Game thread CPU | ~100% of one core | ~100% of one core |
| Time in `ntdll` (heap alloc/free, locks) | 22% | 3% |
| Hot call chain `0x005F6E47 → 0x00AE68BE` (into Scaleform/GameInfo code) | ~38% of the frame | not in the top |

Both are CPU-bound on a single thread. In live spectating, more than a third of every frame goes into one call chain, and most of that time is spent allocating and freeing memory.

## Root cause

All addresses are for `DOW2.exe` 3.19.1.10320, the current Steam build.

### `FUN_00AE6850`: "which slot is the local player in?"

```
if (*(*(0x01335678) + 4) & 0x558)          // the session is online (live MP game)
{
    match = session->vtbl[0x24]()           // RLink match object
    GameInfo gi;                            // ctor 0x0040172D
    match->vtbl[4](&gi)                     // copies the WHOLE match info:
                                            //   every slot (stride 200 bytes), names, strings, vectors
    me = local user id
    for each slot in gi: if slot.id == me -> index
    ~GameInfo()                             // dtor 0x00401794: frees all of it
    return index
}
return 0                                    // offline / replay: returns immediately
```

Every call builds a complete copy of the match info from the network session and then throws it away. The only goal is to find the local player's slot index.

### Who calls it, and how often

The main caller is `FUN_00AE5E00`. It also has 8 other call sites: `0xAE5F84`, `0xAE7250`, `0xAEC947`, `0xB229EB`, `0xB22A19`, `0xB24738`, `0xB651A1`, `0xB6BE87`.

```
if (*(0x013356A4 + 0x174) == 0)             // no local player entity  ->  we are an OBSERVER
    observer = FUN_00AE6850() > 3;          // rebuild GameInfo to find our slot
relation = FUN_00815890(...)                 // ally / enemy, swapped for observers
```

It answers "am I an observer, and is this unit an ally or an enemy?". The game asks this from rendering and UI code, many times per frame (team colours, minimap, selection and so on).

### Why only observers, why only live, why it scales

- Players skip the expensive branch. They have their own player entity (`+0x174 != 0`).
- Replays skip it too. The session is not online (`& 0x558 == 0`), so the function returns 0 immediately.
- Live observers hit it dozens of times per frame. Each call copies every slot, so the cost grows with the number of players and stations, and with how often the per-unit query runs (more units means more calls).

## Fix: ObsFix.dll

The local player's slot index cannot change during a match, so the answer can be cached.

- ObsFix.dll hooks the prologue of `FUN_00AE6850` (`55 8B EC 83 E4 F8`, the same 6-byte pattern used elsewhere) with a jump to a small wrapper.
- The wrapper returns the cached value if it is less than 500 ms old. Otherwise it calls the original function through a trampoline and stores the result.
- The game gets exactly the same value as before. Only the redundant rebuilds are skipped.
- Nothing in the simulation changes, so there is no sync impact. Players and replays still take their fast paths and are unaffected.
- `ObsFix.log` writes a line once a minute: total lookups, real lookups and the percentage saved.
- `ObsFix.ini`: `Enabled=0` turns it off. `CacheMs` sets the cache lifetime.
- If `DOW2.exe` is not 3.19 (the prologue doesn't match), it does nothing and says so in the log.

## Result

Tested by spectating live games with ObsFix enabled. Spectating no longer drops to half of replay speed. The exact before/after numbers for a given match can be read from FrameProbe and ObsFix logs.

## Files

- `ObsFix.dll`, `ObsFix.ini`, `README.txt`
- `obsfix.c`: the full source, about 60 lines. Build: `i686-w64-mingw32-gcc -O2 -shared -o ObsFix.dll obsfix.c -static-libgcc`
- Install: put the DLL into `Elite_DLLs\` and add `, ObsFix.dll` to `InjectedFiles=` in `Elite.launchconfig`. In the Elite Launcher it is listed under add-ons.

## Possible follow-ups

- Do the same caching inside `FUN_00AE5E00` itself, or patch the caller to compute the observer flag once per frame.
- The same "rebuild GameInfo on every call" pattern may exist in other places (the `0x0040172D` ctor has many callers). They are worth checking with the same profiler.
