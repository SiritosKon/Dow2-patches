CGNumbers.dll v1 - control-group numbers above squads in Dawn of War II: Retribution 3.19 (Elite). Experimental.

When you assign squads to a control group (Ctrl+1 .. Ctrl+0), the group number is drawn in white in the
top-right corner of that squad's floating banner. Handy in 2v2/3v3 to see your groups at a glance.
It reuses the game's own squad banner position (no separate tracking), so it follows the squads naturally.

Install: put CGNumbers.dll into  <game>\dow2-patches\  (loaded by the winmm shim), or into <game>\Elite_DLLs\
and add ", CGNumbers.dll" to InjectedFiles in Elite.launchconfig (or tick it in the Elite Launcher add-ons).
Log: CGNumbers.log (next to the dll). Turn off: CGNumbers.ini -> [CGNumbers] Enabled=0.

Needs DoW2 Retribution 3.19 (Elite); 32-bit. Draws through the game's Direct3D9 (DXVK-safe).
