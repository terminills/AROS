#!/usr/bin/env bash
set -euo pipefail

# Populate every Ion AROS icon source from the normalized artwork library.
# Several closely related objects intentionally share a glyph; the native
# .info descriptor still preserves the correct Workbench object type.

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
icons_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
target_dir="$icons_dir/AROS"
images_dir="$icons_dir/Images"

pick_default() {
    case "$1" in
        def_CDROM) echo disk-cdrom ;;
        def_BusyCDROM) echo disk-busy-cdrom ;;
        def_NDOSCDROM) echo disk-ndos-cdrom ;;
        def_DVD) echo disk-dvd ;;
        def_Disk) echo disk-floppy ;;
        def_BusyDisk) echo disk-busy-floppy ;;
        def_FATDisk|def_FFSDisk|def_KickDisk|def_PFSDisk|def_SFSDisk|def_NDOSDisk) echo disk-filesystem ;;
        def_Harddisk|def_FATHarddisk|def_FFSHarddisk|def_NDOSHarddisk) echo disk-harddisk ;;
        def_BusyHarddisk) echo disk-busy-harddisk ;;
        def_RAM) echo disk-ram ;;
        def_RAD) echo disk-rad ;;
        def_USB) echo disk-usb ;;
        def_UnknownDevice) echo device-unknown ;;
        def_Drawer) echo image:Drawer ;;
        def_Project) echo file-project ;;
        def_Picture) echo file-picture ;;
        def_Text) echo file-text ;;
        def_Document) echo file-document ;;
        def_Tool) echo file-tool ;;
        def_Authors) echo file-authors ;;
        def_CSS) echo file-css ;;
        def_HTML) echo file-html ;;
        def_Legal) echo file-legal ;;
        def_Movie|def_CDXL|def_ANIM|def_GIFAnim) echo file-movie ;;
        def_Music|def_Instrument|def_MP3ID3|def_MP3|def_PLS|def_M3U|def_FLAC) echo file-music ;;
        def_PDF) echo file-pdf ;;
        def_Patch) echo file-patch ;;
        def_Postscript) echo file-postscript ;;
        def_Presentation) echo file-presentation ;;
        def_Printer) echo pref-printer ;;
        def_ReadMe) echo file-readme ;;
        def_Spreadsheet) echo file-spreadsheet ;;
        def_XCF) echo file-xcf ;;
        def_Sound) echo file-sound ;;
        def_Keymap) echo file-keymap ;;
        def_DataType|def_DT64) echo file-datatype ;;
        def_Installer|def_MSD|def_MSI) echo file-installer ;;
        def_AudioMode) echo file-audiomode ;;
        def_Zune|def_Pref) echo pref-zune ;;
        def_TrueType|def_Bitmap_Font|def_Outline_Font) echo file-truetype ;;
        def_Otag) echo file-otag ;;
        def_A68K) echo file-a68k ;;
        def_Catalog) echo file-catalog ;;
        def_Country) echo file-country ;;
        def_Home|def_UserHome) echo file-home ;;
        def_Lua|def_AmiLua) echo file-lua ;;
        def_Python) echo file-python ;;
        def_REXX) echo file-html ;;
        def_Archive|def_LHA) echo file-archive ;;
        def_NetworkHost) echo device-networkhost ;;
        def_Trashcan) echo file-trashcan ;;
        def_ELF_EXE_Linux) echo file-tool ;;
        def_KickROM) echo file-a68k ;;
        def_Wireless) echo device-wireless ;;
        def_Sashimi) echo debug-sashimi ;;
        *) return 1 ;;
    esac
}

copy_icon() {
    local source_key=$1
    local destination=$2
    local source
    case "$source_key" in
        image:*) source="$images_dir/${source_key#image:}.png" ;;
        *) source="$script_dir/$source_key.png" ;;
    esac
    mkdir -p "$(dirname -- "$destination")"
    cp "$source" "$destination"
}

mapped=0
while IFS= read -r -d '' descriptor; do
    rel=${descriptor#"$target_dir/"}
    rel=${rel%.info.src}

    # The thirteen root drawers are the hand-normalized first pass.
    if [[ $rel != */* && -f "$target_dir/$rel.png" ]]; then
        continue
    fi

    key=
    case "$rel" in
        Developer/Debug) key=drawer-debug ;;
        Developer/Debug/Bifteck) key=debug-bifteck ;;
        Developer/Debug/Hardware) key=drawer-hardware ;;
        Developer/Debug/Hardware/PCITool) key=tool-pcitool ;;
        Developer/Debug/Sashimi) key=debug-sashimi ;;
        Developer/Debug/Tests) key=drawer-tests ;;
        Developer/Debug/Tests/DualPNG) key=debug-dualpng ;;
        Developer/Include) key=drawer-include ;;
        Developer/Lib) key=drawer-lib ;;

        Devs/DOSDrivers) key=drawer-drivers ;;
        Devs/DOSDrivers/AUDIO) key=file-audiomode ;;
        Devs/DOSDrivers/ICD0|Devs/DOSDrivers/ICD1) key=disk-harddisk ;;
        Devs/DOSDrivers/IDF0|Devs/DOSDrivers/IDF1|Devs/DOSDrivers/PC0) key=disk-floppy ;;
        Devs/DOSDrivers/ISO0) key=disk-cdrom ;;
        Devs/DOSDrivers/PAR|Devs/DOSDrivers/SER|Devs/DOSDrivers/SER0|Devs/DOSDrivers/SER1) key=pref-serial ;;
        Devs/DOSDrivers/PIPE) key=commodity-exchange ;;
        Devs/DOSDrivers/PRT) key=pref-printer ;;
        Devs/DataTypes) key=drawer-datatypes ;;
        Devs/Drivers) key=drawer-drivers ;;
        Devs/Keymaps) key=drawer-keymaps ;;
        Devs/Midi) key=dev-midi ;;
        Devs/Monitors) key=drawer-monitors ;;
        Devs/Monitors/*) key=dev-monitor-wrapper ;;
        Devs/Printers) key=drawer-printers ;;

        Locale/Catalogs) key=drawer-catalogs ;;
        Locale/Countries) key=drawer-countries ;;
        Locale/Flags) key=drawer-flags ;;
        Locale/Help) key=drawer-help ;;
        Locale/Languages) key=drawer-languages ;;

        Prefs/Env-Archive/SYS/def_AppIcon)
            copy_icon file-project "$target_dir/Prefs/Env-Archive/SYS/def_AppIcon1.png"
            copy_icon file-project "$target_dir/Prefs/Env-Archive/SYS/def_AppIcon2.png"
            mapped=$((mapped + 1))
            continue
            ;;
        Prefs/Env-Archive/SYS/*) key=$(pick_default "${rel##*/}") ;;
        Prefs/AHI) key=pref-ahi ;;
        Prefs/Appearance|Prefs/Theme) key=pref-appearance ;;
        Prefs/Asl) key=pref-asl ;;
        Prefs/BoingIconBar) key=pref-boingbar ;;
        Prefs/Boot) key=pref-boot ;;
        Prefs/Editor) key=pref-editor ;;
        Prefs/Font) key=pref-font ;;
        Prefs/IControl) key=pref-icontrol ;;
        Prefs/Input) key=pref-input ;;
        Prefs/Locale) key=pref-locale ;;
        Prefs/Network) key=pref-network ;;
        Prefs/OpenURL) key=pref-openurl ;;
        Prefs/PSI) key=pref-psi ;;
        Prefs/Palette) key=pref-palette ;;
        Prefs/Pointer) key=pref-pointer ;;
        Prefs/Presets) key=drawer-presets ;;
        Prefs/Presets/*) key=pref-appearance ;;
        Prefs/Printer) key=pref-printer ;;
        Prefs/ReqTools) key=pref-reqtools ;;
        Prefs/ScreenMode) key=pref-screenmode ;;
        Prefs/Serial) key=pref-serial ;;
        Prefs/Time) key=pref-time ;;
        Prefs/Trackdisk) key=pref-trackdisk ;;
        Prefs/Trident) key=pref-trident ;;
        Prefs/Wanderer) key=pref-wanderer ;;
        Prefs/Zune) key=pref-zune ;;

        System/About) key=system-about ;;
        System/DiskImageGUI)
            copy_icon system-diskimage "$target_dir/System/DiskImageGUI-1.png"
            copy_icon system-diskimage "$target_dir/System/DiskImageGUI-2.png"
            mapped=$((mapped + 1))
            continue
            ;;
        System/FTManager) key=system-ftmanager ;;
        System/Find) key=system-find ;;
        System/FixFonts) key=system-fixfonts ;;
        System/Format) key=system-format ;;
        System/LogView) key=system-logview ;;
        System/Shell) key=system-shell ;;
        System/Snoopy) key=system-snoopy ;;
        System/SysMon) key=system-sysmon ;;
        System/VMM) key=system-vmm ;;
        System/WorkBook) key=system-workbook ;;

        Tools/AHIRecord) key=tool-ahirecord ;;
        Tools/BoingIconBar) key=pref-boingbar ;;
        Tools/Calculator) key=tool-calculator ;;
        Tools/Commodities) key=drawer-presets ;;
        Tools/Commodities/ASCIITable) key=commodity-asciitable ;;
        Tools/Commodities/AltKeyQ) key=commodity-altkeyq ;;
        Tools/Commodities/AutoPoint) key=commodity-autopoint ;;
        Tools/Commodities/Blanker) key=commodity-blanker ;;
        Tools/Commodities/ClickToFront) key=commodity-clicktofront ;;
        Tools/Commodities/DepthMenu) key=commodity-depthmenu ;;
        Tools/Commodities/Exchange) key=commodity-exchange ;;
        Tools/Commodities/FKey) key=commodity-fkey ;;
        Tools/Commodities/NoCapsLock) key=commodity-nocapslock ;;
        Tools/Commodities/Opaque) key=commodity-opaque ;;
        Tools/Editor) key=pref-editor ;;
        Tools/GraphicDump) key=tool-graphicdump ;;
        Tools/HDToolBox) key=tool-hdtoolbox ;;
        Tools/InitPrinter) key=tool-initprinter ;;
        Tools/InstallAROS) key=tool-installeros ;;
        Tools/KeyShow) key=tool-keyshow ;;
        Tools/PrintFiles) key=tool-printfiles ;;
        Tools/QuickPart) key=drawer-hardware ;;
        Tools/QuickPart/QuickPart) key=tool-hdtoolbox ;;
        Tools/ScreenGrabber) key=tool-screengrabber ;;
        Tools/ShowConfig) key=tool-showconfig ;;
        Tools/SysExplorer) key=tool-sysexplorer ;;
        Tools/WiMP) key=tool-wimp ;;

        Utilities/Clock) key=pref-time ;;
        Utilities/Help) key=drawer-help ;;
        Utilities/Installer) key=tool-installeros ;;
        Utilities/More) key=file-text ;;
        Utilities/MultiView) key=file-picture ;;
        *) echo "unmapped Ion icon source: $rel" >&2; exit 1 ;;
    esac

    copy_icon "$key" "$target_dir/$rel.png"
    mapped=$((mapped + 1))
done < <(find "$target_dir" -type f -name '*.info.src' -print0 | sort -z)

echo "assigned artwork to $mapped non-root Ion icon sources"
