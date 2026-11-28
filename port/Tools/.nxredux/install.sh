#!/bin/sh
# Установка патча Mr712 «на лету»: перевод вписывается в те бинарники,
# которые сейчас лежат на карте (nextui.elf и settings.elf), без подмены
# старыми версиями.
# Лог: /mnt/SDCARD/Images/Bootlogo/Mr712_patch.log

SD="${SDCARD_PATH:-/mnt/SDCARD}"
PAK="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
[ -n "$PAK" ] || PAK="$SD/Tools/.nxredux"
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

# --- переезд со старой установки «через Порты» ---
OLDPAK="$SD/Roms/Ports (PORTS)/.ports/nxredux-mr712"
if [ -d "$OLDPAK" ] && [ "$OLDPAK" != "$PAK" ]; then
	for f in nextui.elf.before settings.elf.before minarch.elf.before bootlogo_before.bmp; do
		if [ -f "$OLDPAK/$f" ] && [ ! -f "$PAK/$f" ]; then
			cp "$OLDPAK/$f" "$PAK/$f" 2>> "$LOG" && log "бэкап $f перенесён из старой папки порта"
		fi
	done
	rm -rf "$OLDPAK" 2>> "$LOG"
	rm -f "$SD/Roms/Ports (PORTS)/NX REDUX Mr712 Patch.sh" "$SD/Roms/Ports (PORTS)/NX Redux Patch.sh" 2>> "$LOG"
	rmdir "$SD/Roms/Ports (PORTS)/.ports" 2>/dev/null
	log "старая установка в «Портах» удалена"
fi

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
# map.txt.orig — копия БЕЗ наших строк; её и восстанавливает remove.sh.
# Так откат не зависит от того, что лежало в map.txt в момент установки.
MAPSRC="$PAK/tools_map.txt"
MAPDST="$SD/Tools/map.txt"
MAPORIG="$SD/Tools/map.txt.orig"
if [ -f "$MAPSRC" ]; then
	mkdir -p "$SD/Tools"
	if [ ! -f "$MAPORIG" ]; then
		if [ -f "$MAPDST" ]; then
			awk -F'\t' 'NR==FNR { skip[$1]=1; next } !($1 in skip)' "$MAPSRC" "$MAPDST" > /tmp/nx_tools_map.txt 2>> "$LOG"
		else
			: > /tmp/nx_tools_map.txt
		fi
		cp /tmp/nx_tools_map.txt "$MAPORIG" 2>> "$LOG" && log "map.txt: сохранены исходные названия"
		rm -f /tmp/nx_tools_map.txt
	fi
	cp "$MAPORIG" /tmp/nx_tools_map.txt 2>> "$LOG"
	cat "$MAPSRC" >> /tmp/nx_tools_map.txt
	if cp /tmp/nx_tools_map.txt "$MAPDST"; then
		sync
		log "map.txt: названия инструментов переведены"
	else
		log "ОШИБКА: не удалось записать map.txt"
	fi
	rm -f /tmp/nx_tools_map.txt "$MAPDST.bak"
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

# штатный launch.sh настроек не трогаем: папка теперь всегда Settings.pak (без пробела)
if [ -f "$SETPAK/launch.sh" ]; then
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
# основа: официальная копия, если файл уже пропатчен (пересборка новым патчером)
if grep -q 'Mr712RU' "$BIN" 2>/dev/null && [ -f "$PAK/nextui.elf.before" ]; then
	base="$PAK/nextui.elf.before"
elif ! grep -q 'Mr712RU' "$BIN" 2>/dev/null; then
	cp "$BIN" "$PAK/nextui.elf.before" 2>> "$LOG" && log "nextui.elf: копия официального файла обновлена"
fi
if [ -f "$base" ]; then
	if patch_into "$base" "$PAK/nextui.tsv" /tmp/nx_nextui.elf --relax; then
		cp /tmp/nx_nextui.elf "$BIN" && sync && log "nextui.elf пропатчен на лету"
		rm -f /tmp/nx_nextui.elf
	else
		rc=$?
		if [ "$rc" = "2" ]; then log "nextui.elf уже пропатчен"; else log "ОШИБКА: nxpatch nextui rc=$rc"; fi
	fi
fi

# --- settings.elf: перевод + пункт «NX Redux Patch» в настройках ---
# settings собирается заново из официальной копии settings.elf.before:
# в settings.elf есть место только под один кейв, поэтому перевод и пункт
# дописываются одним проходом (--settings-entry).
if [ -f "$SETELF" ]; then
	if grep -q 'Mr712RU' "$SETELF" 2>/dev/null || grep -q 'Mr712MENU' "$SETELF" 2>/dev/null; then
		sbase="$PAK/settings.elf.before"
		[ -f "$sbase" ] || sbase="$SETELF"
	else
		sbase="$SETELF"
		if [ ! -f "$PAK/settings.elf.before" ] || [ "$(md5of "$sbase")" != "$(md5of "$PAK/settings.elf.before")" ]; then
			cp "$SETELF" "$PAK/settings.elf.before" 2>> "$LOG" && log "settings.elf: копия официального файла обновлена"
		fi
	fi
	if patch_into "$sbase" "$PAK/settings.tsv" /tmp/nx_settings.elf "--relax --settings-entry"; then
		if cp /tmp/nx_settings.elf "$SETELF.new" && mv -f "$SETELF.new" "$SETELF"; then
			sync
			log "settings.elf пропатчен (перевод + пункт в настройках)"
		else
			log "ОШИБКА: не удалось записать settings.elf"
		fi
		rm -f /tmp/nx_settings.elf "$SETELF.new"
	else
		rc=$?
		if [ "$rc" = "2" ]; then log "settings.elf уже пропатчен"; else log "ОШИБКА: nxpatch settings rc=$rc"; fi
	fi
else
	log "settings.elf не найден — перевод настроек пропущен"
fi

# --- minarch.elf (внутриигровое меню всех эмуляторов) ---
if [ -f "$MINARCH" ] && [ -f "$PAK/minarch.tsv" ]; then
	# основа: официальная копия, если файл уже пропатчен (пересборка новым патчером)
	mb="$MINARCH"
	if grep -q 'Mr712RU' "$MINARCH" 2>/dev/null && [ -f "$PAK/minarch.elf.before" ]; then
		mb="$PAK/minarch.elf.before"
	elif ! grep -q 'Mr712RU' "$MINARCH" 2>/dev/null; then
		cp "$MINARCH" "$PAK/minarch.elf.before" 2>> "$LOG" && log "minarch.elf: копия официального файла обновлена"
	fi
	if "$PATCHER" "$mb" /tmp/nx_minarch.elf "$PAK/minarch.tsv" --relax --minarch-display "$PAK/minarch_display.tsv" >> "$LOG" 2>&1; then
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

# --- ярлык в «Инструментах» нужен только до установки ---
if grep -q 'Mr712RU' "$BIN" 2>/dev/null && [ -d "$SD/Tools/NX Redux Patch.pak" ]; then
	rm -rf "$SD/Tools/NX Redux Patch.pak" 2>> "$LOG" && log "ярлык в Инструментах скрыт"
fi

log "готово, перезагрузка"
sync
sleep 1
reboot
