#!/bin/sh
# Ярлык «NX Redux Patch» во вкладке «Инструменты».
# Сам патчер лежит в Tools/.nxredux: после первой регистрации в настройках
# приставки этот ярлык удаляется, и запуск идёт из «Настроек».

SD="${SDCARD_PATH:-/mnt/SDCARD}"
H="$SD/Tools/.nxredux"
LOG="$SD/Images/Bootlogo/Mr712_patch.log"

if [ ! -f "$H/launch.sh" ]; then
	echo "$(date '+%Y-%m-%d %H:%M:%S') ярлык: нет $H/launch.sh" >> "$LOG"
	exit 1
fi

chmod +x "$H/launch.sh" "$H/register.sh" "$H/install.sh" "$H/remove.sh" "$H/exit.sh" 2>/dev/null
exec sh "$H/launch.sh"
