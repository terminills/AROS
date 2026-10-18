# Ion icon preset

This preset supplies a coherent AROS desktop vocabulary at 96 by 96 pixels:
root and nested drawers, developer and debug tools, devices, preferences,
system tools, utilities, commodities, storage states, and file/datatype
defaults.  The Workbench icons are built from the same visual vocabulary and
the generic images are installed under
`Prefs/Presets/Icons/Ion/Default/Images`.

The preset includes 210 native Workbench descriptors.  A hosted x86-64 build
currently produces 212 preset icons because the AROS and Storage trees share
some sources while installing to separate destinations.  That is a strict
superset of the current 175-icon Gorilla Default preset.

All root drawer images use a 96 by 96 pixel canvas, a 72 pixel optical height,
and the same baseline.  Drawer-specific symbols sit on one common chassis so
Wanderer's automatic grid remains visually aligned.

The retained generation atlases are in `Icons/Atlases`.  The 128 normalized
subject cells are in `Icons/Artwork`; `build-from-atlases.sh` reproduces their
geometry and `assign-artwork.sh` maps them onto the complete AROS source tree.
