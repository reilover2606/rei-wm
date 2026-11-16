# Themes: the look of rei-wm's own UI

Everything the compositor draws itself (the toolbar, its workspace buttons, the application
tray, tooltips, pop-up lists) is described by a **style** per **element**. A theme is a
`[theme.<element>]` table in `config.toml` for each element you want to change. Anything you do not set
keeps its default, and the defaults are derived from the three base colours in `[toolbar]`
(`background`, `foreground`, `highlight`), so a config without a `[theme]` looks exactly as before.
A bad name or value is a startup/reload error that names the key.

```toml
[theme.tray_tile]
shape = "pill"
active_color = "#7aa2f7"
shadow_opacity = 0.35
shadow_offset_y = 2
shadow_blur = 5

[theme.toolbar]
opacity = 0.85
```

## Style properties

| key | meaning |
|---|---|
| `color` | the normal fill |
| `active_color` | the fill when active/highlighted: the focused window's tile, the focused workspace, the hovered row of a list |
| `inactive_color` | the fill when dimmed: a tile whose windows are all minimized; a workspace shown on a monitor that is not focused |
| `text_color`, `active_text_color`, `inactive_text_color` | text in those same three states |
| `border_color`, `border_width` | an outline, in pixels (0 = none) |
| `shape` | `square`, `rounded`, `pill` (fully rounded ends) or `bevel` (corners cut at 45 degrees) |
| `radius` | corner size in pixels, for `rounded` and `bevel` |
| `opacity` | 0 to 1: how opaque the element's fill is (transparency) |
| `shadow_color`, `shadow_opacity`, `shadow_offset_x`, `shadow_offset_y`, `shadow_blur` | a soft drop shadow; `shadow_opacity = 0` means none |
| `glow_color` (white), `glow_size` (pixels, 0 = none), `glow_opacity` | a halo outside the element. On by default for `menu_row` (when hovered), `dialog` and `run_prompt` |

| `text_glow_size` (pixels, 0 = none), `text_glow_strength` (0 to 4), `text_glow_tint` (0 to 1), `text_glow_color` | light around the letters. With `text_glow_tint = 0` it has each letter's own colour; towards 1 it takes `text_glow_color` |
| `text_blink` | seconds per on/off cycle (0 = steady): the text shows for the first half of each cycle |
| `text_shine` (0 to 1), `text_shine_color` (white) | a metallic shine: the upper half of the letters gleams (strongest at the top), with a hard horizon in the middle and a darker reflection under it |

Colours are `"#rrggbb"`. Hovering a tile or button lightens its fill by a fixed amount toward its text colour.

## Text effects and shaders

The three `text_*` effects are drawn by GLSL fragment shaders, not painted into the pictures. An element whose
text has an effect keeps its text in a separate layer, which the renderer draws over the element through the
text shader; the effects combine (glowing, shining, blinking text is one pass). They are off by default and
apply to every element that has text. Blinking costs two redraws per cycle and nothing in between; glow and
shine are computed once per change of the text and kept, not once per frame.

### Window text: `[theme.window_text]`

This element is not rei-wm's own text. It names programs whose **windows** are drawn through a window shader,
the equivalent of picom's `--window-shader-fg`. The built-in shader, `glow`, makes the bright pixels of the
window (a terminal's text) bleed light into what is around them, each in its own colour, and leaves a dark
background dark.

```toml
[theme.window_text]
apps = "URxvt"            # app ids (Wayland) / classes (X11), comma separated, case-insensitive; "*" = every window
shader = "glow"           # or the path of a GLSL file (see below)
text_glow_size = 5        # pixels; 0 turns it off
text_glow_strength = 1.0
glow_threshold = 0.35     # only pixels brighter than this glow (0 = everything, for light-on-dark at any level)
text_glow_tint = 0        # towards 1: the glow takes text_glow_color instead of the text's colour
text_blink = 0            # seconds per cycle: the whole window blinks (0 = steady)
```

It is on by default for urxvt. Set `text_glow_size = 0` (or empty `apps`) to turn it off. The same from the
command line, as with picom, taking precedence over the theme:

```bash
rei-wm --window-shader-fg-rule glow:URxvt --window-shader-fg-rule ~/shaders/crt.glsl:foot
```
```bash
rei-wm --window-shader-fg ~/shaders/grayscale.glsl
```

The first form is for one application's windows, the second for every window.

### Writing a shader

A shader file is GLSL ES 3.00 and defines `vec4 window_shader()`, as picom's do; most picom shaders work
unchanged (their `#version` line and their declarations of `tex`, `texcoord`, `opacity` and `time` are
dropped, and `texture2D` is accepted):

```glsl
vec4 window_shader() {
    vec4 c = texelFetch(tex, ivec2(texcoord), 0);   // premultiplied RGBA
    float grey = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    return default_post_processing(vec4(vec3(grey), c.a));
}
```

Available: `sampler2D tex`, `vec2 texcoord` (this pixel, in pixels), `vec2 u_size` (the picture's size),
`float u_px` (texture pixels per logical pixel), `float time` (milliseconds), `float opacity`,
`vec4 u_p[4]` (the glow settings: `u_p[0]` = size, strength, tint, threshold; `u_p[1].rgb` = tint colour),
`float u_blink`, and the helpers `rei_texel(vec2 px)`, `rei_glow(vec2 px, float radius, float threshold)` and
`default_post_processing(vec4)`. A shader that mentions `time` is run on every frame, about thirty times a
second; any other is run once each time the window's content changes. A file that does not compile is
reported in the log and the window is drawn as it is; a file that does not exist is a config error. Edits to
the file are picked up when the config is next reloaded.

## Elements

| element | what it is | properties that apply |
|---|---|---|
| `toolbar` | the bar's background and its clock and volume text | `color`, `opacity`, `text_color`, `inactive_text_color` (a muted volume) |
| `workspace_button` | a workspace number: `color` (a workspace with windows, not showing), `active_color` (the focused one), `inactive_color` (showing on another monitor) | everything |
| `tray_tile` | an application in the tray; the stack of tiles behind it is derived from these colours | everything |
| `tray_overflow` | the "+N" tile that holds what no longer fits | everything |
| `tooltip` | the hover tooltip | `color`, `text_color`, shape, border, `opacity`, shadow |
| `sysinfo` | the CPU/RAM box left of the clock | `color`, `text_color`, shape, border |
| `dialog` | confirmation dialogs | `color`, `text_color`, shape, border, `opacity`, shadow, glow |
| `menu` | the pop-up window list | `color`, border, shape, `opacity`, shadow |
| `battery` | the battery icon: `color` is the level fill, `active_color` the fill while charging (with a bolt), `inactive_color` the fill and text at 15% or less; `border_color`/`border_width` the outline, `shape`/`radius` the cell's corners | `color`, `active_color`, `inactive_color`, `text_color`, border, `shape`, `radius` |
| `window_text` | not rei-wm's own: the windows of the listed programs (terminals), through a window shader | `apps`, `shader`, `text_glow_*`, `glow_threshold`, `text_blink` (see below) |
| `menu_row` | one row of a list | `active_color` (hovered row), `text_color`, `active_text_color`, `inactive_text_color` (minimized windows) |

Defaults at a glance: tiles, buttons and tooltips have `shape = "rounded"` with `radius = 4`; the
toolbar and menus are square; tooltips and menus have a soft shadow; everything is fully opaque
except the tooltip (0.97).

## Where this is going

The schema is meant to cover more than the toolbar: the window notches, window borders and shadows,
the wallpaper and the cursor are the next elements to join it, with the same properties and the same
`[theme.<element>]` spelling, so that a theme can be one file that restyles the whole desktop.
Elements and properties are added in `src/theme.h` (the `rei_style` and `rei_element` types) and
`src/theme.c` (names, defaults, parsing); the drawing helpers that apply a style
(`draw_box` and friends in `src/toolbar_render.c`) are the model for how a new element is wired in.

## Icons (the iconable elements)

`tray_tile`, `start_button` and `menu_row` are *iconable*. Their style also takes `icon` (true/false) and
`icon_size` (0-64 pixels; 0 fits the element). `[theme] flavor_icons = "dir"` gives each of them a random icon
from that directory of PNGs; files that are not square or are larger than 64x64 are rejected (logged).

## The theme editor

The settings app's **Theme** tab edits every value above for every element, with a preview that rei-wm itself
draws (`rei-wm --theme-preview out.png`: the toolbar with its Start button, tray, CPU/RAM box, battery and volume;
a Start menu with a hovered row; a tooltip; the volume popup; the Run prompt; a dialog; the message column and its
backdrop). The values shown are the effective theme (`rei-wm --print-theme`): the built-in look with your
`[theme.*]` tables applied. Saving writes only what you changed, into `[theme.<element>]` tables, and leaves the
rest of the file alone. The flavor icons folder is `[theme] flavor_icons`.

Every value has a ↺ button that returns it to the built-in value (`rei-wm --print-theme-defaults`), each element
has "Reset this element", and the list has "Reset everything". Resetting a value *removes* its line from the
config (an emptied `[theme.<element>]` table goes too) instead of writing the default, so the element keeps
following the toolbar colours. The three base colours (`[toolbar] background`, `foreground`, `highlight`, which
every built-in look is derived from) are at the top of the Theme tab; changing one moves every value you have
not set by hand, and the preview follows.
