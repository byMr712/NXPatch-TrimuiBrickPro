#!/bin/sh
# NX Redux Patch — установка/удаление из «Инструментов».
# Запускается как Tools/NX Redux Patch.pak/launch.sh.
# Раскладка кнопок берётся из настроек системы (Button layout / Hint labels).

SD="${SDCARD_PATH:-/mnt/SDCARD}"
HERE="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
[ -n "$HERE" ] || HERE="$SD/Tools/NX Redux Patch.pak"
PAK="$HERE"
LOG="$SD/Images/Bootlogo/Mr712_patch.log"
MENU="$PAK/mr712menu"
CFG="$SD/.userdata/shared/minuisettings.txt"
export SDCARD_PATH="$SD"
export LD_LIBRARY_PATH="/usr/trimui/lib:${LD_LIBRARY_PATH}"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

# на FAT/exFAT exec-биты могут потеряться
chmod +x "$PAK/mr712menu" "$PAK/nxpatch" "$PAK/options.elf" "$PAK/install.sh" "$PAK/remove.sh" "$PAK/exit.sh" "$PAK/yes.sh" "$PAK/no.sh" 2>/dev/null

bl="$(grep -m1 '^buttonlayout=' "$CFG" 2>/dev/null | cut -d= -f2)"
hl="$(grep -m1 '^hintlabels=' "$CFG" 2>/dev/null | cut -d= -f2)"
if [ "$bl" = "1" ]; then
	conf=0; cancel=1
	if [ "$hl" = "1" ]; then hint=b; else hint=a; fi
else
	conf=1; cancel=0; hint=a
fi
log "layout: buttonlayout=$bl hintlabels=$hl -> confirm=$conf cancel=$cancel hint=$hint"

if [ -x "$MENU" ] && [ -f "$PAK/menu0.bmp" ]; then
	"$MENU" "$PAK" "$conf" "$cancel" "$hint"
	rc=$?
	log "menu rc=$rc"
	[ "$rc" -eq 2 ] || exit 0
	log "menu: fallback to options.elf"
fi

PICKER="$PAK/options.elf"
[ -f "$PICKER" ] || PICKER="$SD/.system/bin/options.elf"
killall -9 show2.elf >/dev/null 2>&1
cd "$PAK" 2>/dev/null || cd "$SD"
CHOSEN="$("$PICKER" --pick \
	--entry "1. Установить патч" "$PAK/install.sh" \
	--entry "2. Удалить патч" "$PAK/remove.sh" \
	--entry "3. Выйти" "$PAK/exit.sh" 2>> "$LOG")"
case "$CHOSEN" in
	*install.sh) exec "$PAK/install.sh" ;;
	*remove.sh)  exec "$PAK/remove.sh" ;;
esac
exit 0
