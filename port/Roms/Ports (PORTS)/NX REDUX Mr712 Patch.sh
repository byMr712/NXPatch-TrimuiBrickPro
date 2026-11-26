#!/bin/sh
# NX REDUX Mr712 Patch — меню с описанием пунктов.
# Раскладка кнопок берётся из настроек системы (Button layout / Hint labels).

SD="${SDCARD_PATH:-/mnt/SDCARD}"
PAK="$SD/Roms/Ports (PORTS)/.ports/nxredux-mr712"
LOG="$SD/Images/Bootlogo/Mr712_patch.log"
MENU="$PAK/mr712menu"
CFG="$SD/.userdata/shared/minuisettings.txt"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

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
	LD_LIBRARY_PATH=/usr/trimui/lib "$MENU" "$PAK" "$conf" "$cancel" "$hint"
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
