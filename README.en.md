> **Language:** [Русский](README.md) · English

# [MR] NX Redux Patch (RU)

![Platform](https://img.shields.io/badge/Platform-TrimUI_Brick_Pro-blue.svg)
![NX Redux](https://img.shields.io/badge/NX_Redux-2.5.1-blue.svg)
![Interface](https://img.shields.io/badge/Interface-Russian-blue.svg)
![Patch](https://img.shields.io/badge/Patch-1.0-blue.svg)
![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)

A patch for **NX Redux (NextUI) on the TrimUI Brick Pro** that translates the interface into Russian, replaces the boot logo and adds a few menu improvements. The patch is applied on the fly to the already installed `nextui.elf` / `settings.elf` and can be removed just as easily.

| Patcher menu | Translated game menu |
| :---: | :---: |
| ![Patcher](images/patch-menu.png) | ![Game menu](images/game-menu-ru.png) |

## What Does This Patch Do

- **Full Russian interface** for NextUI and Settings: main menu, tabs (CONSOLES, COLLECTIONS, TOOLS, RECENT, SEARCH), game list, game menu, every dialog (delete, rename, collections, artwork fetching), button hints and the on-screen keyboard.
- **Settings** are translated completely, including item descriptions and the simple mode help text.
- **Boot logo** `Big Mr712.bmp` with a signature; the clean variant `Big` stays available in `Settings → Boot logo`.
- **Dedicated patcher menu** with a description of the selected item, black "Install / Remove / Exit" screens and wrap-around navigation.
- **Grid wrap-around**: in Tools, Consoles and Collections, pressing down on the last row moves to the first one, and up on the first row moves to the last one.
- **Search within the console**: a game-menu item that matches the **beginning of the game title** in the current console folder (the file extension is ignored). Back from the results returns to the search prompt with the previous query; back again returns to the game list.
- **"Continue" row at the top of a console**: the first row is the last game played in that console, with its cover; A resumes it from the save.
- **Version check**: if `version.txt` is not `NXRedux-v2.5.1`, the patch warns and offers to cancel (cancel is the default).

## Compatibility

- Device: **TrimUI Brick Pro** (any NextUI device with the same memory layout should work as well).
- Firmware: **NX Redux 2.5.1** (checked through `version.txt`).
- The patch works with the **installed** binaries: nothing is downgraded, official updates keep working.
- Official `nextui.elf` and `settings.elf` are not part of this repository — the patch is applied to the files already present on the card.
- The translation is version independent: strings are located by content. Code changes (grid wrap, console search) are build specific and are simply skipped on another version.

## Installation

1. Copy the contents of the `port` folder to the root of the SD card, keeping the structure:
   ```
   Roms/Ports (PORTS)/NX REDUX Mr712 Patch.sh
   Roms/Ports (PORTS)/.ports/nxredux-mr712/...
   ```
2. Insert the card into the console and power it on.
3. Open **Ports** and start **NX REDUX Mr712 Patch** (or `TOOLS → NX REDUX Mr712 Patch`).
4. Choose **Install patch** and confirm.
5. Wait for the "Done" message and reboot the console.

Safety: the first run stores `nextui.elf.before` and `settings.elf.before`; the rollback uses them.

## Removal

1. Start **NX REDUX Mr712 Patch** and choose **Remove patch**.
2. Reboot the console.

The official `nextui.elf` and `settings.elf` are restored, the original boot logo and the Settings folder name are returned. Logos added to the Settings list are kept.

## How It Works (For Developers)

The patch does not rebuild NextUI; it edits machine code inside the ELF files.

### String relocation

`nxpatch` (AArch64, static) finds the original string in `.rodata` by content, copies the translation into a new RX segment (the "cave") and rewrites every reference:

- `ADRP + ADD` pairs (including chained `ADD`s and non-chained `add Xd, Xbase, #imm`);
- eight-byte pointers in string tables.

The original strings stay in the file, which is why English labels are still visible in `strings`, while the interface already uses the Russian ones. Patched files are marked with `Mr712RU`; applying the patch twice returns code 2 ("already patched").

### Truncated menu labels

The compiler expands `snprintf(dst, 64, "%s", "literal")` into a fixed-length copy of `strlen(literal)+1` bytes. After replacing the literal with a longer Russian string, the label was cut in the middle of a UTF-8 sequence. `nxpatch` recognizes such copies by their shape (coverage exactly `strlen+1`, the item `id` stored at offset `0x40`) and replaces them with a call to a small helper in the cave that copies up to NUL.

### Grid wrap-around

The grid input handler contains two "stop" branches (up on the first row and down on the last row). Both are redirected to cave stubs that compute the new index and return into the common selection path. The site is found by a ten-instruction signature; on a mismatch the step is skipped.

### Patcher menu

`mr712menu` is a small SDL2 application that draws pre-rendered BMP frames. The button layout and hint labels are read from `minuisettings.txt` (`buttonlayout`, `hintlabels`), so the menu follows the system settings instead of assuming an Xbox pad.

## Limitations

- Code patches (grid wrap, the planned per-console search) are tied to a specific NX Redux build. After a firmware update, run the patch again: if the signature does not match, that step is skipped without any side effects (the log shows `grid wrap 0`).
- The translation and the artwork are version independent and apply to any suitable binary.
- The patch does not touch saves, game lists, collections or user settings.

## Repository Layout

```
port/    ready-to-use set for the SD card (copy to the card root)
tools/   sources: nxpatch.c, mr712menu.c, translation tables, helper scripts
docs/    technical notes and plans
images/  screenshots used by the READMEs
```

The tools are cross-compiled for `aarch64-linux-musl`; the ready binary `nxpatch` is already inside `port`.

## Credits

The original interface and firmware belong to the NextUI and NX Redux projects. This repository contains neither of their binaries and changes nothing on the card without an explicit user command.

## License

This project is licensed under the **Apache-2.0 License**.
