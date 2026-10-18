#!/usr/bin/env bash
set -euo pipefail

# Rebuild the normalized 96x96 Ion artwork cells from the retained 4x4
# generation atlases.  The visible subject is constrained to a 72x72 optical
# box and aligned to y=88, matching the root drawer icons.

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
atlas_dir="$script_dir/../Atlases"
ffmpeg_bin=${FFMPEG:-ffmpeg}

export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu

split_atlas() {
    local atlas=$1
    shift
    local -a names=("$@")
    local -a xedges yedges
    # The generated atlases have four visually even rows and columns, but the
    # transparent gutters are not located at exact mathematical quarters.
    # These measured cut lines sit inside the real empty gutters.  Keeping the
    # per-atlas geometry here prevents neighbouring subjects from leaking into
    # the normalized icon at the top or bottom of a cell.
    case "$atlas" in
        atlas-commodities.png)
            xedges=(0 322 626 934 1254); yedges=(0 404 687 965 1254) ;;
        atlas-filetypes-extra.png)
            xedges=(0 325 631 938 1254); yedges=(0 360 651 937 1254) ;;
        atlas-filetypes.png)
            xedges=(0 323 625 930 1254); yedges=(0 340 634 926 1254) ;;
        atlas-folders.png)
            xedges=(0 322 629 939 1254); yedges=(0 397 688 975 1254) ;;
        atlas-prefs.png)
            xedges=(0 324 623 928 1254); yedges=(0 431 724 980 1254) ;;
        atlas-storage.png)
            xedges=(0 325 627 935 1254); yedges=(0 334 616 905 1254) ;;
        atlas-system-prefs.png)
            xedges=(0 317 627 938 1254); yedges=(0 353 642 928 1254) ;;
        atlas-tools.png)
            xedges=(0 320 626 933 1254); yedges=(0 361 651 936 1254) ;;
        *)
            echo "no measured atlas geometry for $atlas" >&2
            return 1
            ;;
    esac
    local idx row col x y w h crop

    if ((${#names[@]} != 16)); then
        echo "atlas $atlas needs exactly 16 names" >&2
        return 1
    fi

    for idx in "${!names[@]}"; do
        row=$((idx / 4))
        col=$((idx % 4))
        x=${xedges[$col]}
        y=${yedges[$row]}
        w=$((xedges[$((col + 1))] - x))
        h=$((yedges[$((row + 1))] - y))
        crop=$(
            "$ffmpeg_bin" -hide_banner -loglevel info -i "$atlas_dir/$atlas" \
                -vf "crop=$w:$h:$x:$y,alphaextract,cropdetect=limit=0.08:round=1:reset=0:skip=0" \
                -frames:v 1 -f null - 2>&1 |
                sed -n 's/.*\(crop=[0-9:]*\).*/\1/p' | tail -n 1
        )
        if [[ -z $crop ]]; then
            echo "could not find alpha bounds for $atlas cell $idx (${names[$idx]})" >&2
            return 1
        fi
        "$ffmpeg_bin" -hide_banner -loglevel error -y -i "$atlas_dir/$atlas" \
            -vf "crop=$w:$h:$x:$y,$crop,scale=72:72:force_original_aspect_ratio=decrease:flags=lanczos,pad=96:96:(ow-iw)/2:88-ih:color=black@0,format=rgba" \
            "$script_dir/${names[$idx]}.png"
    done
}

split_atlas atlas-folders.png \
    drawer-debug drawer-hardware drawer-tests drawer-include \
    drawer-lib drawer-datatypes drawer-drivers drawer-keymaps \
    drawer-monitors drawer-printers drawer-catalogs drawer-countries \
    drawer-flags drawer-help drawer-languages drawer-presets

split_atlas atlas-prefs.png \
    pref-ahi pref-appearance pref-asl pref-boot \
    pref-editor pref-time pref-locale pref-serial \
    pref-input pref-icontrol pref-font pref-screenmode \
    pref-palette pref-pointer pref-printer pref-network

split_atlas atlas-system-prefs.png \
    pref-reqtools pref-wanderer pref-zune pref-trackdisk \
    pref-psi pref-trident pref-openurl pref-boingbar \
    system-about system-fixfonts system-format system-ftmanager \
    system-workbook system-shell system-sysmon system-snoopy

split_atlas atlas-tools.png \
    system-find system-vmm system-logview system-diskimage \
    tool-calculator tool-hdtoolbox tool-installeros tool-showconfig \
    tool-keyshow tool-initprinter tool-printfiles tool-graphicdump \
    tool-ahirecord tool-wimp tool-screengrabber tool-sysexplorer

split_atlas atlas-commodities.png \
    commodity-altkeyq commodity-asciitable commodity-autopoint commodity-blanker \
    commodity-clicktofront commodity-depthmenu commodity-exchange commodity-fkey \
    commodity-nocapslock commodity-opaque tool-pcitool debug-bifteck \
    debug-sashimi debug-dualpng dev-midi dev-monitor-wrapper

split_atlas atlas-filetypes.png \
    file-document file-text file-picture file-movie \
    file-music file-sound file-pdf file-html \
    file-css file-archive file-installer file-tool \
    file-project file-legal file-authors file-readme

split_atlas atlas-filetypes-extra.png \
    file-spreadsheet file-presentation file-postscript file-patch \
    file-xcf file-keymap file-datatype file-audiomode \
    file-truetype file-otag file-a68k file-catalog \
    file-country file-home file-lua file-python

split_atlas atlas-storage.png \
    disk-cdrom disk-busy-cdrom disk-ndos-cdrom disk-dvd \
    disk-floppy disk-busy-floppy disk-filesystem disk-harddisk \
    disk-busy-harddisk device-unknown disk-ram disk-rad \
    disk-usb device-wireless device-networkhost file-trashcan

echo "rebuilt 128 normalized Ion artwork cells"
