LobbyStats Lite v0.6.17 - Relic rating next to names and a stats tooltip in the Dawn of War II: Retribution lobby. Nothing else.

Hover a player slot in the lobby: Relic rating, win rate (wins / losses), games, Steam hours.
No host filter, no kicks, no chat commands, no new menus.
The rating is also shown next to each name, e.g. "PlayerName [2061]" (ShowRatingInName=0 in the ini turns that off).
Data: public Relic API via a small stats web service (read only, in a background thread; the game's netcode is not touched).

Elite:
  1. Put LobbyStats.dll and LobbyStats.ini into <game folder>\Elite_DLLs\
  2. In Elite.launchconfig add ", LobbyStats.dll" to the end of the InjectedFiles= line.
Vanilla (no Elite):
  Put winmm.dll and the LobbyStats folder next to DOW2.exe, start the game from Steam as usual.
  (winmm.dll passes everything to the real Windows winmm.dll and loads LobbyStats; with Elite it does nothing.)
Log: LobbyStats.log next to the dll. Remove: delete the files.
