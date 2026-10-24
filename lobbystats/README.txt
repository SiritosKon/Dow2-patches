LobbyStats.dll v0.6.17 - Relic rating in the Dawn of War II Retribution / Elite lobby + optional host filter  (prototype, 21.10.2026)

What it does
  In the game lobby each player's name gets the Relic rating appended:  PlayerName  [2087]
  Hover a player slot to see a tooltip: Relic rating, win rate (wins / losses), games, Steam hours.
  Data: a small stats web service (host set in the ini / at build time) that reads the public Relic API.
  The first time a name appears it is shown without numbers; they appear on the next lobby update
  (someone joins, readies, changes race) and instantly in later lobbies (cache file next to the DLL).

Install (Elite)
  1. Copy LobbyStats.dll to  <Steam>\steamapps\common\Dawn of War II - Retribution\Elite_DLLs\
  2. Open Elite.launchconfig (same game folder) in Notepad and append LobbyStats.dll to InjectedFiles:
       InjectedFiles=CullspherePatch.dll, DoW2_ArmyPainterFix.dll, MiscFixes.dll, ShellPatch.dll, LobbyStats.dll
  3. Start Elite as usual.
  Check: Elite_DLLs\LobbyStats.log should contain "hooked Invoke at 0045DB6C" and later "lookup N names".
  Only DOW2.exe 3.19 (current Steam build) is supported; on any other build the DLL does nothing and says so in the log.

Host filter - two menus in Game Setup (v0.5)
  When you host, two extra drop-down lists appear under "Starting resources" (other players don't see them):
    Rating:            off / 1000+ / 2000+ / 2500+ / 1400- / 1000-
    Hours / win rate:  off / hours 1000+ / hours 1000- / win rate 45%+ / 45%- / 50%+
    ("+" = at least, "-" = at most.) Any other numbers - with the !filter chat command; the menu then shows them,
    e.g. "!filter 1700" -> the list shows "Rating 1700+".
  Pick a value and the filter is on: the bot writes the rules in lobby chat for everyone, re-checks players already
  in the lobby, and anyone outside the limits gets "[Bot] Name kicked by bot: min rating is 1600, yours is 1520"
  and is kicked a few seconds later. Set both lists to "off" to turn the filter off. The choice is remembered.
  v0.6: players are checked as soon as their stats arrive (not only when the rules change); the kick notice also goes
  to the General chat so the kicked player sees why (write anything in General once so the DLL learns that channel).
  If the lists cover something or don't show up: [UI] in LobbyStats.ini (FilterLists=0 turns them off).

Host filter - or type it in the lobby chat (you must be the host). The sign can go before or after the number:
  !filter 1500+  (or 1500, +1500)  rating at least 1500      !filter 1500-  (or -1500)  rating at most 1500
  !filter h1000+ / h1000-          Steam hours               !filter wr45+ / wr45-       win rate
  A new value replaces the old one for that parameter. The menu in Game Setup updates right away.
  !filter                         show the current rules
  !filter 1500-2400               rating range        !filter 1800   min rating     !filter -2000  max rating
  !filter wr55 / wr45-60          win rate %          !filter g100   min games      !filter h750   min Steam hours   !filter h-1000  max hours
  !filter nonew / new             kick / allow players without Relic stats
  !filter test                    test mode: the bot only writes in chat, nobody is kicked (default)
  !filter live                    kick for real       !filter off    turn off       !filter reset  clear rules
  Several at once: !filter 1500-2400 wr50 h500 live
  The bot answers in the lobby chat for everyone, e.g. "[Bot] Lobby filter ON: rating 1500-2400, win rate 50%+, hours 500+ (kicks on)",
  re-checks players already in the lobby, and remembers the rules for your next lobbies (LobbyStats.ini).

Options (file Elite_DLLs\LobbyStats.ini, see comments inside)
  [LobbyStats] ShowRatingInName=0  - keep names untouched, show the rating only in the hover tooltip
  [Guard] (v0.2) - host-only lobby filter, OFF by default:
     Enabled=1, DryRun=1, MinRating=1800  -> bot writes in lobby chat
        "[Bot test] Name would be kicked: min rating is 1800, yours is 1520"   (nobody is kicked)
     DryRun=0 -> "[Bot] Name kicked by bot: min rating is 1800, yours is 1520", kick after KickDelaySeconds
     Rules: MinRating, MaxRating, MinWinRate, MaxWinRate (%), MinGames, MinHours, KickUnknown.
     Players are checked by their SteamID64, so renaming doesn't help. Private Steam profiles skip the hours rule.
     If the stats server is unreachable, nobody is kicked.

Remove: delete LobbyStats.dll and remove it from InjectedFiles.
