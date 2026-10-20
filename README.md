# misc-game-mods
A collection of random mods I have made for random games!

## Installation

### Unity Engine
1. Determine the modloader needed (Bepinex or Melonloader). 
2. Follow the modloaders instructions how to build and install.

### Godot Engine
1. Create an `override.cfg` next to the game binary.
2. Append: 
``` 
[autoload]
SCRIPT_NAME=SCRIPT_PATH
```

### Terraria
1.
2.
3.

### Reshade Plugins
1.
2.
3.


## Notes
Most Godot Engine games can have its FPS unlocked and enable ultrawide support by appending the below snippet to an `override.cfg` in the game's directory.

```
[display]
window/stretch/aspect="expand"
window/vsync/vsync_mode=0

[application]
run/max_fps=0
```
