#!/bin/sh
# Установка патча Mr712 «на лету»: перевод вписывается в те бинарники,
# которые сейчас лежат на карте (nextui.elf и settings.elf), без подмены
# старыми версиями.
# Лог: /mnt/SDCARD/Images/Bootlogo/Mr712_patch.log

SD="${SDCARD_PATH:-/mnt/SDCARD}"
PAK="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
[ -n "$PAK" ] || PAK="$SD/Roms/Ports (PORTS)/.ports/nxredux-mr712"
LOG="$SD/Images/Bootlogo/Mr712_patch.log"
BIN="$SD/.system/bin/nextui.elf"
MINARCH="$SD/.system/bin/minarch.elf"
TOOLS="$SD/.system/paks/Tools"
SETPAK="$TOOLS/ Настройки.pak"
[ -d "$SETPAK" ] || SETPAK="$TOOLS/Settings.pak"
SETELF="$SETPAK/settings.elf"
VER_FILE="$SD/.system/version.txt"
LOGO_DIR="$SETPAK/bootlogo/brick"
DEV="/dev/mmcblk0p1"
PICKER="$PAK/options.elf"
[ -x "$PICKER" ] || PICKER="$SD/.system/bin/options.elf"
SHOW2="$SD/.system/bin/show2.elf"
PATCHER="$PAK/nxpatch"

PATCH_VER="NXRedux-v2.5.1"

# наши прошлые пропатченные сборки: их нельзя брать за основу
OLD_NEXTUI="9263ea8c03c3e2a6bc4e325f619d4d33 c493a0a4ae13cf1277c8a0e366e8145c ceeeb642de4d558df847fac31eeeb288"
OLD_SETTINGS="32effeec997b7fd169feef3e46304628 694e977663b472f3d5ac7a88760c4e49"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

md5of() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

is_old() {
	for x in $2; do [ "$1" = "$x" ] && return 0; done
	return 1
}

patch_into() { # $1 = base, $2 = tsv, $3 = out
	"$PATCHER" "$1" "$3" "$2" $4 >> "$LOG" 2>&1
}

cur_ver="$(head -n1 "$VER_FILE" 2>/dev/null)"
[ -n "$cur_ver" ] || cur_ver="неизвестна"

log "=== install (прошивка: $cur_ver, патч: $PATCH_VER)"



if [ ! -x "$PATCHER" ]; then
	chmod +x "$PATCHER" 2>/dev/null
fi
if [ ! -x "$PATCHER" ]; then
	log "ОШИБКА: нет $PATCHER"
	exit 1
fi

# --- проверка версии ---
if [ "$cur_ver" != "$PATCH_VER" ]; then
	log "версия не совпадает — спрашиваю"
	if [ -x "$SHOW2" ] && [ -f "$PAK/warning.bmp" ]; then
		"$SHOW2" --mode=simple --image="$PAK/warning.bmp" --timeout=6 >> "$LOG" 2>&1
	fi
	CHOSEN="$("$PICKER" --pick \
		--entry "Нет — отменить" "$PAK/no.sh" \
		--entry "Да — установить на свой риск" "$PAK/yes.sh" 2>> "$LOG")"
	case "$CHOSEN" in
		*yes.sh) log "пользователь подтвердил установку" ;;
		*) log "отменено пользователем"; exit 0 ;;
	esac
fi

# --- названия инструментов: Tools/map.txt (NX Redux берёт его из папки Tools на карте) ---
MAPSRC="$PAK/tools_map.txt"
MAPDST="$SD/Tools/map.txt"
if [ -f "$MAPSRC" ]; then
	mkdir -p "$SD/Tools"
	if [ -f "$MAPDST" ] && [ ! -f "$MAPDST.bak" ]; then
		cp "$MAPDST" "$MAPDST.bak" 2>> "$LOG" && log "map.txt: сохранена исходная копия"
	fi
	if [ -f "$MAPDST" ]; then
		awk -F'\t' 'NR==FNR { skip[$1]=1; next } !($1 in skip)' "$MAPSRC" "$MAPDST" > /tmp/nx_tools_map.txt 2>> "$LOG"
	else
		: > /tmp/nx_tools_map.txt
	fi
	cat "$MAPSRC" >> /tmp/nx_tools_map.txt
	if cp /tmp/nx_tools_map.txt "$MAPDST"; then
		sync
		log "map.txt: названия инструментов переведены"
	else
		log "ОШИБКА: не удалось записать map.txt"
	fi
	rm -f /tmp/nx_tools_map.txt
fi

# --- папка настроек: как в оригинале (Settings.pak); если осталась переименованная — вернуть ---
if [ -d "$TOOLS/ Настройки.pak" ] && [ ! -d "$TOOLS/Settings.pak" ]; then
	if mv "$TOOLS/ Настройки.pak" "$TOOLS/Settings.pak" 2>> "$LOG"; then
		log "папка ' Настройки.pak' -> Settings.pak"
	fi
fi
SETPAK="$TOOLS/Settings.pak"
SETELF="$SETPAK/settings.elf"
LOGO_DIR="$SETPAK/bootlogo/brick"

# в штатном launch.sh cd без кавычек — с пробелом в имени папки он ломается
if [ -f "$SETPAK/launch.sh" ]; then
	sed -i 's|^cd \$(dirname "\$0")|cd "$(dirname "$0")"|' "$SETPAK/launch.sh" 2>> "$LOG" && log "launch.sh настроек: cd с кавычками"
	chmod +x "$SETPAK/launch.sh" 2>/dev/null
fi

# --- nextui.elf ---
# основа: установленный бинарник, если это не наша старая сборка
cur="$(md5of "$BIN")"
base="$BIN"
if is_old "$cur" "$OLD_NEXTUI" && [ -f "$PAK/nextui.elf.orig" ]; then
	base="$PAK/nextui.elf.orig"
	log "nextui.elf — старая сборка ($cur), основа: nextui.elf.orig"
fi
if [ -f "$base" ]; then
	if patch_into "$base" "$PAK/nextui.tsv" /tmp/nx_nextui.elf --relax; then
		# запоминаем то, что реально стояло (только если это не наш патч)
		if ! is_old "$cur" "$OLD_NEXTUI"; then
			cp "$BIN" "$PAK/nextui.elf.before" 2>> "$LOG"
		fi
		cp /tmp/nx_nextui.elf "$BIN" && sync && log "nextui.elf пропатчен на лету"
		rm -f /tmp/nx_nextui.elf
	else
		rc=$?
		if [ "$rc" = "2" ]; then log "nextui.elf уже пропатчен"; else log "ОШИБКА: nxpatch nextui rc=$rc"; fi
	fi
fi

# --- settings.elf ---
if [ -f "$SETELF" ]; then
	scur="$(md5of "$SETELF")"
	sbase="$SETELF"
	if is_old "$scur" "$OLD_SETTINGS" && [ -f "$PAK/settings.elf.orig" ]; then
		sbase="$PAK/settings.elf.orig"
		log "settings.elf — старая сборка ($scur), основа: settings.elf.orig"
	fi
	if patch_into "$sbase" "$PAK/settings.tsv" /tmp/nx_settings.elf --relax; then
		if ! is_old "$scur" "$OLD_SETTINGS"; then
			cp "$SETELF" "$PAK/settings.elf.before" 2>> "$LOG"
		fi
		cp /tmp/nx_settings.elf "$SETELF" && sync && log "settings.elf пропатчен на лету"
		rm -f /tmp/nx_settings.elf
	else
		rc=$?
		if [ "$rc" = "2" ]; then log "settings.elf уже пропатчен"; else log "ОШИБКА: nxpatch settings rc=$rc"; fi
	fi
else
	log "settings.elf не найден — перевод настроек пропущен"
fi

# --- minarch.elf (внутриигровое меню всех эмуляторов) ---
if [ -f "$MINARCH" ] && [ -f "$PAK/minarch.tsv" ]; then
	if "$PATCHER" "$MINARCH" /tmp/nx_minarch.elf "$PAK/minarch.tsv" --relax --minarch-display "$PAK/minarch_display.tsv" >> "$LOG" 2>&1; then
		cp "$MINARCH" "$PAK/minarch.elf.before" 2>> "$LOG"
		cp /tmp/nx_minarch.elf "$MINARCH" && sync && log "minarch.elf пропатчен на лету"
		rm -f /tmp/nx_minarch.elf
	else
		rc=$?
		if [ "$rc" = "2" ]; then log "minarch.elf уже пропатчен"; else log "ОШИБКА: nxpatch minarch rc=$rc"; fi
	fi
fi

# --- логотип в загрузчик ---
MNT="$(awk -v d="$DEV" '$1==d {print $2; exit}' /proc/mounts)"
OURS=0
if [ -z "$MNT" ]; then
	MNT="/mnt/bootlogo_apply"
	mkdir -p "$MNT"
	if mount -t vfat "$DEV" "$MNT" >> "$LOG" 2>&1; then
		OURS=1
	else
		log "ВНИМАНИЕ: не смонтировать $DEV — лого не записан"
		MNT=""
	fi
fi
if [ -n "$MNT" ]; then
	if [ -f "$MNT/bootlogo.bmp" ] && [ ! -f "$PAK/bootlogo_before.bmp" ]; then
		cp "$MNT/bootlogo.bmp" "$PAK/bootlogo_before.bmp" 2>> "$LOG" && log "бэкап логотипа сделан"
	fi
	cp "$PAK/Big Mr712.bmp" "$MNT/bootlogo.bmp" 2>> "$LOG" && log "логотип записан"
	sync
	[ "$OURS" = "1" ] && umount "$MNT" >> "$LOG" 2>&1
fi

if [ -d "$LOGO_DIR" ]; then
	rm -f "$LOGO_DIR/Big Mr712.bmp" "$LOGO_DIR/NX Redux Big.bmp" "$LOGO_DIR/NX Redux Clean.bmp" 2>> "$LOG"
	if [ -f "$PAK/Big.bmp" ]; then
		cp "$PAK/Big.bmp" "$LOGO_DIR/Big.bmp" 2>> "$LOG" && log "логотип без подписи добавлен в Settings"
	fi
fi

log "готово, перезагрузка"
sync
sleep 1
reboot
