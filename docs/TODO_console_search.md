# «Поиск игры в консоли» — состояние и план

Дата: 2026-11-26. Патч в коде `tools/nxpatch.c` лежит под флагом `-DCS_CONSOLE_SEARCH`
(по умолчанию выключен, штатная сборка даёт `a8f38cd0…`).

## Что уже работает

- Пункт **«Поиск игры в консоли»** добавляется в низ меню игры (id 42).
  Две точки вставки: `GameList_openContextMenuFor` (0x41c910) и инлайн-копия этого же кода
  в `GameList_handleInput` (0x41e3b8, `ldr w1,[sp,#0xd0]`).
  Обе гейтятся по типу записи (`ENTRY_ROM` или `entryFolderGame`).
- По этому пункту открывается штатная клавиатура с заголовком «Поиск в консоли», ввод работает.

## Что падает

1. **После нажатия «Готово»** приложение падает (nextui перезапускается).
   Проверено по breadcrumb-логу `/tmp/cs.log` (putFile): доходит до создания Directory
   (`Directory_new("/mnt/SDCARD/.search", 0)`) и до первого элемента, дальше — падение.
2. Даже **без фильтра** (пустой фильтр) после вставки списка (`Array_push` + `top=dir`)
   приложение падает.

## Причина (рабочая версия)

Штатное открытие поиска — не функция, а код, **вклеенный в `GameList_handleInput`**
(блок 0x41efd4…0x41f0b4). Он после создания списка делает ещё:
`PLAT_clearLayers(0x4117d0)`, `ScrollText_clear(0x40c6f0)`, `readyResume(0x4a6200)` и,
главное, пишет в **стек-фрейм вызывающего** (`str w20,[sp,#0x8c]`, `strb w20,[sp,#0x90]`) —
флаги результата кадра. Из обработчика контекстного меню этот фрейм недоступен,
поэтому состояние остаётся несогласованным → падение при отрисовке.

Отдельно: наш фильтр падал и сам (маркеры F1…F5), вероятно из-за работы с `entries`
в момент, когда Directory ещё/уже не консистентен — требует отдельной отладки.

## План следующей попытки (согласован с пользователем)

1. **Не строить поиск заново**, а переиспользовать штатный путь:
   - пункт меню ставит флаг (`setenv("NX_SEARCH_ROOT", top->path, 1)`) и закрывает меню;
   - в `GameList_handleInput` условие запуска поиска (проверка START) расширяем:
     «START **или** установлен NX_SEARCH_ROOT», после запуска флаг сбрасываем.
   - Тогда весь блок 0x41efd4…0x41f0b4 выполняется в родном фрейме, состояние согласовано.
2. **Фильтр** вставляем внутрь этого блока: после `bl Directory_new` (0x41f044)
   инструкцию `mov x19,x0` (0x41f048) заменить на `b stub`:
   ```
   stub: bl filter    ; x0 = Directory*, фильтр по env-префиксу
         mov x19, x0
         b 0x41f04c
   ```
3. Фильтр переписать аккуратно:
   - не освобождать отброшенные записи (просто компактить массив) — утечка допустима;
   - проверять `entries != NULL` и `count > 0`;
   - сравнивать `strncmp(path, prefix, len)` + `path[len] == '/'`.

## Полезные адреса (nextui.elf 2.5.1)

| что | адрес |
|---|---|
| GameList_openContextMenuFor | 0x41c820 (точка вставки 0x41c910) |
| инлайн-копия меню | точка вставки 0x41e3b8 (`ldr w1,[sp,#0xd0]`) |
| GameList_runContextAction | 0x49c330 |
| инлайн Search_open | 0x41efd4 … 0x41f0b4 (Directory_new — 0x4a7b00, вызов 0x41f044) |
| addItem | 0x41c7b0 |
| entryFolderGame | 0x496cb0 |
| UIKeyboard_openWith | 0x429c60 |
| Directory_new | 0x4a7b00 |
| Array_push | 0x4ace30 |
| Entry_free | 0x4acea0 |
| GameList_rowCountAt | 0x497370 |
| PLAT_clearLayers | 0x4117d0 |
| ScrollText_clear | 0x40c6f0 |
| readyResume | 0x4a6200 |
| PAD_reset | 0x431970 |
| requestBackgroundReupload | 0x4a18b0 |
| top / stack / search_query | 0x4d8da0 / 0x4d8e80 / 0x510430 |
| FAUX_SEARCH_PATH «/mnt/SDCARD/.search» | 0x4ae930 |
| PLT: snprintf 0x406710, free 0x407350, strlen 0x4074c0, strncmp 0x406b30, getenv 0x407220, setenv 0x406640 | — |

Структуры: `Entry{path 0, name 8, unique 0x10, type 0x18}`;
`Directory{path 0, name 8, entries 0x10, …, selected 0x8c, start 0x90, end 0x94}`;
`Array{count 0, capacity 4, items 8}`.
