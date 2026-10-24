FrameProbe.dll v9 - diagnostic logger for Dawn of War II: Retribution (Elite). It does not change the game, it only writes logs.

Install: put FrameProbe.dll into <game folder>\Elite_DLLs\ and add ", FrameProbe.dll" to the end of the InjectedFiles line in Elite.launchconfig.

Observer lag: watch a replay for ~1 minute, then spectate a live multiplayer game for 1-2 minutes. Note the times and FPS.
Continue button: play a multiplayer match to the end and stay on the score screen for ~30 seconds (host and one more player if possible).

Then send Elite_DLLs\FrameProbe.log, Elite_DLLs\FrameProbe_trace.log and Logfiles\warnings.txt (or press "Send logs" in the launcher).
Remove: delete ", FrameProbe.dll" from InjectedFiles and delete the file.

Options: Elite_DLLs\FrameProbe.ini, [Probe] Observer=0 / Continue=0.
v7: fixes the crash on the first 10-second report (game tick counter removed); no longer copies the game debug output.
v8: samples the game-loop thread (v7 sampled the window thread by mistake); Continue no longer crashes after a replay.
v9: samples 50 times a second instead of 200; [Probe] Sampler=0 never pauses the game thread (FPS and frame times only).
