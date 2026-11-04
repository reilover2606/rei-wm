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

Colours are `"#rrggbb"`. Hovering a tile or button lightens its fill by a fixed amount toward its text colour.

## Elements

| element | what it is | properties that apply |
|---|---|---|
| `toolbar` | the bar's background and its clock and volume text | `color`, `opacity`, `text_color`, `inactive_text_color` (a muted volume) |
| `workspace_button` | a workspace number: `color` (a workspace with windows, not showing), `active_color` (the focused one), `inactive_color` (showing on another monitor) | everything |
| `tray_tile` | an application in the tray; the stack of tiles behind it is derived from these colours | everything |
| `tray_overflow` | the "+N" tile that holds what no longer fits | everything |
| `tooltip` | the hover tooltip | `color`, `text_color`, shape, border, `opacity`, shadow |
| `menu` | the pop-up window list | `color`, border, shape, `opacity`, shadow |
| `battery` | the battery icon: `color` is the level fill, `active_color` the fill while charging (with a bolt), `inactive_color` the fill and text at 15% or less; `border_color`/`border_width` the outline, `shape`/`radius` the cell's corners | `color`, `active_color`, `inactive_color`, `text_color`, border, `shape`, `radius` |
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
