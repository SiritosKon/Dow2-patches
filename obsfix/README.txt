ObsFix.dll v1 - fixes observer lag in Dawn of War II: Retribution 3.19 (Elite). Experimental.

Cause: while spectating a live game, the game asks "which slot is the local player in?" dozens of times per frame.
Each answer rebuilds a full copy of the match info from the network session (all slots, names, lists) and throws it
away. Players and replays skip this path, observers don't - that is why only live spectating lags and it gets worse
with more players. ObsFix remembers the answer for 0.5 s (the slot never changes during a match).

Install: put ObsFix.dll into <game>\Elite_DLLs\ and add ", ObsFix.dll" to InjectedFiles in Elite.launchconfig
(or tick it in Elite Launcher add-ons). Log: Elite_DLLs\ObsFix.log (once a minute: how many lookups were saved).
Turn off: ObsFix.ini [ObsFix] Enabled=0.
