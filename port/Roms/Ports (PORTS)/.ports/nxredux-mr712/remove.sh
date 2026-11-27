#!/bin/sh
# Удаление патча Mr712: возвращает nextui.elf и settings.elf к тому,
# что стояло до патча (.before), иначе — к официальным файлам из порта.

SD="${SDCARD_PATH:-/mnt/SDCARD}"
PAK="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
[ -n "$PAK" ] || PAK="$SD/Roms/Ports (PORTS)/.ports/nxredux-mr712"
LOG="$SD/Images/Bootlogo/Mr712_patch.log"
BIN="$SD/.system/bin/nextui.elf"
TOOLS="$SD/.system/paks/Tools"
SETPAK="$TOOLS/ Настройки.pak"
[ -d "$SETPAK" ] || SETPAK="$TOOLS/Settings.pak"
SETELF="$SETPAK/settings.elf"
LOGO_DIR="$SETPAK/bootlogo/brick"
DEV="/dev/mmcblk0p1"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

log "=== remove"

restore() { # $1 = current, $2 = .before, $3 = .orig
	if [ -f "$2" ]; then
		cp "$2" "$1" && sync && rm -f "$2" && { log "восстановлен $(basename "$1") из $(basename "$2")"; return 0; }
	fi
	if grep -q 'Mr712RU' "$1" 2>/dev/null; then
		if [ -f "$3" ]; then
			cp "$3" "$1" && sync && log "восстановлен $(basename "$1") из $(basename "$3")"
			return 0
		fi
	fi
	log "$(basename "$1"): восстанавливать нечего"
	return 0
}

restore "$BIN" "$PAK/nextui.elf.before" "$PAK/nextui.elf.orig"
if [ -f "$SETELF" ]; then
	restore "$SETELF" "$PAK/settings.elf.before" "$PAK/settings.elf.orig"
fi

# вернуть папке штатное имя, если снимаем патч вместе с ней
if [ -d "$TOOLS/ Настройки.pak" ] && [ ! -d "$TOOLS/Settings.pak" ]; then
	if mv "$TOOLS/ Настройки.pak" "$TOOLS/Settings.pak" 2>> "$LOG"; then
		log "папка настроек: обратно Settings.pak"
	fi
fi

# названия инструментов: вернуть исходный Tools/map.txt
MAPDST="$SD/Tools/map.txt"
if [ -f "$MAPDST.mr712.bak" ]; then
	if cp "$MAPDST.mr712.bak" "$MAPDST"; then
		rm -f "$MAPDST.mr712.bak"
		sync
		log "map.txt: исходная копия восстановлена"
	fi
elif [ -f "$PAK/tools_map.txt" ] && [ -f "$MAPDST" ]; then
	awk -F'\t' 'NR==FNR { skip[$1]=1; next } !($1 in skip)' "$PAK/tools_map.txt" "$MAPDST" > /tmp/nx_tools_map.txt 2>> "$LOG"
	if [ -s /tmp/nx_tools_map.txt ]; then
		cp /tmp/nx_tools_map.txt "$MAPDST" && sync && log "map.txt: записи патча удалены"
	else
		rm -f "$MAPDST" && sync && log "map.txt: удалён"
	fi
	rm -f /tmp/nx_tools_map.txt
fi

# логотип загрузчика
if [ -f "$PAK/bootlogo_before.bmp" ]; then
	MNT="$(awk -v d="$DEV" '$1==d {print $2; exit}' /proc/mounts)"
	OURS=0
	if [ -z "$MNT" ]; then
		MNT="/mnt/bootlogo_apply"
		mkdir -p "$MNT"
		if mount -t vfat "$DEV" "$MNT" >> "$LOG" 2>&1; then
			OURS=1
		else
			log "ВНИМАНИЕ: не смонтировать $DEV"
			MNT=""
		fi
	fi
	if [ -n "$MNT" ]; then
		cp "$PAK/bootlogo_before.bmp" "$MNT/bootlogo.bmp" 2>> "$LOG" && log "логотип восстановлен"
		sync
		[ "$OURS" = "1" ] && umount "$MNT" >> "$LOG" 2>&1
	fi
fi

# логотипы в списке Settings остаются (в т.ч. без подписи)

log "готово, перезагрузка"
sync
sleep 1
reboot
