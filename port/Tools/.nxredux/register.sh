#!/bin/sh
# Регистрация патчера в «Настройках» приставки.
#
# При первом входе в патчер пункт «NX Redux Patch» дописывается в самый низ
# списка настроек (settings.elf) — дальше патчер можно запускать и оттуда.
# Ярлык из «Инструментов» убирается ТОЛЬКО когда патч уже установлен:
#   • зашли до установки — ярлык остаётся (install.sh уберёт его после установки);
#   • патч уже стоит (например, архив распаковали заново) — убираем сразу.
# Копия официального settings.elf сохраняется как settings.elf.before — из неё
# install.sh собирает перевод, а remove.sh возвращает нетронутые настройки,
# снова добавляя только этот пункт.
# Лог: /mnt/SDCARD/Images/Bootlogo/Mr712_patch.log

SD="${SDCARD_PATH:-/mnt/SDCARD}"
PAK="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
[ -n "$PAK" ] || PAK="$SD/Tools/.nxredux"
LOG="$SD/Images/Bootlogo/Mr712_patch.log"
TOOLS="$SD/.system/paks/Tools"
SETPAK="$TOOLS/ Настройки.pak"
[ -d "$SETPAK" ] || SETPAK="$TOOLS/Settings.pak"
SETELF="$SETPAK/settings.elf"
PATCHER="$PAK/nxpatch"
VISIBLE="$SD/Tools/NX Redux Patch.pak"
BIN="$SD/.system/bin/nextui.elf"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

if [ ! -f "$SETELF" ]; then
	log "register: settings.elf не найден — пункт не добавлен"
	exit 0
fi

if grep -q 'Mr712MENU' "$SETELF" 2>/dev/null; then
	log "register: пункт в настройках уже есть"
else
	# официальный settings.elf (без наших меток) — обновляем основу для установки
	if ! grep -q 'Mr712RU' "$SETELF" 2>/dev/null; then
		cp "$SETELF" "$PAK/settings.elf.before" 2>> "$LOG" && log "register: копия settings.elf сохранена"
	fi
	chmod +x "$PATCHER" 2>/dev/null
	if "$PATCHER" "$SETELF" /tmp/nx_se.elf --settings-entry >> "$LOG" 2>&1; then
		if cp /tmp/nx_se.elf "$SETELF.new" && mv -f "$SETELF.new" "$SETELF"; then
			sync
			log "register: пункт NX Redux Patch добавлен в настройки"
		else
			log "register: ОШИБКА записи settings.elf"
		fi
		rm -f /tmp/nx_se.elf "$SETELF.new"
	else
		rc=$?
		rm -f /tmp/nx_se.elf "$SETELF.new"
		log "register: пункт не добавлен (rc=$rc) — ярлык остаётся в Инструментах"
		exit 0
	fi
fi

# патч уже установлен — ярлык из «Инструментов» больше не нужен
if grep -q 'Mr712RU' "$BIN" 2>/dev/null; then
	if [ -d "$VISIBLE" ]; then
		rm -rf "$VISIBLE" 2>> "$LOG" && log "register: патч стоит, ярлык в Инструментах скрыт"
	fi
else
	log "register: патч ещё не установлен — ярлык в Инструментах остаётся"
fi
