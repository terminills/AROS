# Ion

Ion is an original AROS desktop theme built around a midnight-navy surface,
electric-cyan activity edges, and restrained amber focus accents.  The theme
contains Intuition window decoration, menu imagery, Zune preferences, toolbar
gadgets, an AROS wallpaper, and a matching Workbench icon preset.

The visual source images for the wallpaper and icon language were generated
for this theme.  Native AROS theme assets are packaged under `system/`,
`menu/`, `images/`, and `Env-Archive/`.

The title and border surfaces are deliberately tile-safe.  AROS repeats these
small images to fill arbitrary window sizes, so Ion keeps the full transition
in the wallpaper and uses a continuous navy surface for window borders.

Select `Ion` in Appearance preferences and the matching `Ion` icon preset.
`SYS:Prefs/Presets/Wallpapers/Ion/` holds the wallpaper cropped (not
stretched) for common screen sizes, `Ion-<width>x<height>.png`, and
`Ion-Drawer.png`, a tileable backdrop for drawer windows.  Pick the one that
matches the screen in Wanderer preferences.

A system built with `--with-aros-prefs=ion --with-theme=Ion --with-iconset=Ion`
starts with all of this selected: Zune prefs, the 1920x1080 desktop wallpaper
and the drawer backdrop come from the `ion` prefs set.

The window gadgets, scroller arrows, containers and knobs are drawn for Ion
(dark glass, cyan glyphs, an amber close gadget); titles use light text with
a shadow so they read on the navy bars.

Palette:

- surface: `#081321`, `#0d2037`, `#17253a`
- active: `#16ccec`, `#75f3ff`
- focus: `#ffb44a`
