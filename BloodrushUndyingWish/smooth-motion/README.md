# Smooth Motion

Makes the player, enemies and camera move smoothly at your monitor's refresh rate
(144 Hz, 240 Hz, ...) instead of the game's fixed 60 updates per second.

## Install

1. Install [Python 3](https://www.python.org)
2. Close the game.
3. Run `install.py` (double-click it, or run `python install.py`).
4. Enter the game folder when asked.
   To find it in Steam, right-click the game > **Manage** > **Browse local files**, then copy the path.
5. Press Enter to confirm.

The installer:

- downloads the official Godot 4.4.1 engine from GitHub and swaps it in,
- sets `override.cfg`: ultrawide support (fills wide screens instead of black bars), unlocked fps (no vsync, no frame cap) and physics interpolation,
- updates the script class list inside `BloodRush.pck` so the game's scripts load on Godot 4.4.

The downloaded engine is kept in `cache/` so a reinstall doesn't download it again.

## After a game update

Steam updates undo the mod. Run `install.py` again.

## Uninstall

1. Delete `override.cfg` from the game folder.
2. In Steam: right-click the game > **Properties** > **Installed Files** > **Verify integrity of game files**.
