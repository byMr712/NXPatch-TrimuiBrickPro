/*
 * nxpatch — переносит русские строки в новый RX-блок ELF и правит ссылки.
 *
 *   nxpatch <input.elf> <output.elf> <strings.tsv>
 *
 * Формат TSV: English<TAB>Russian, по строке на пару; # — комментарий.
 * Работает с ET_EXEC AArch64 (nextui.elf, settings.elf). Не трогает код,
 * кроме пар ADRP+ADD, указывающих на строку.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKER "Mr712RU"
#define MARKER_LEN 16

typedef struct {
    uint32_t type, flags;
    uint64_t off, vaddr, paddr, filesz, memsz, align;
    uint64_t phoff;
} Phdr;

typedef struct {
    char name[64];
    uint32_t type;
    uint64_t flags, addr, off, size;
} Shdr;

typedef struct {
    char *en;
    char *ru;
    uint64_t new_addr;
    int found;
    int relax; /* строку можно искать без NUL перед ней (ровно одно вхождение) */
} Map;

typedef struct {
    uint64_t adrp, add, target;
    uint32_t rn, rd;
} Pair;

static uint8_t *buf;
static size_t buf_len;

static Phdr *ph;
static int phnum;
static Shdr *sh;
static int shnum;
static uint64_t shoff_field;

static Map *map;
static int mapnum;
static Pair *pairs;
static int pairnum, paircap;

static uint8_t cave[0xF000];
static size_t cave_len;

static uint64_t cave_vaddr, ins_at, insert_size, cave_ext, max_cave;

/* Код-помощник для copy-сайтов контекстного меню (см. fix_label_copy).
 * x16 = dst, x17 = src, копирует до NUL; сохраняет/восстанавливает x0,x1 и флаги. */
static const uint8_t copy_helper[] = {
    0xe0, 0x07, 0xbf, 0xa9, 0x20, 0x16, 0x40, 0x38, 0x00, 0x16, 0x00, 0x38,
    0xc0, 0xff, 0xff, 0x35, 0xe0, 0x07, 0xc1, 0xa8, 0xc0, 0x03, 0x5f, 0xd6
};
static uint64_t helper_va;

static uint32_t rd32(const uint8_t *p);
static void wr32(uint8_t *p, uint32_t v);
static uint8_t *file_ptr(uint64_t va, uint64_t len);

/* разобранная инструкция доступа к памяти */
typedef struct {
    int ok, is_load, width, base, index, imm;
} Mem;

static int decode_mem(uint32_t w, Mem *m) {
    memset(m, 0, sizeof(*m));
    m->index = -1;
    /* LDP/STP */
    if ((w & 0x3E000000u) == 0x28000000u) {
        unsigned mode = (w >> 23) & 3; /* 01 post, 10 offset, 11 pre */
        m->ok = 1;
        m->is_load = (int)((w >> 22) & 1);
        m->width = (w >> 31) ? 16 : 8;  /* X-пара / W-пара */
        int imm7 = (int)((w >> 15) & 0x7F);
        if (imm7 & 0x40) imm7 -= 0x80;
        m->imm = (mode == 2) ? imm7 * (m->width / 2) : 0;
        m->base = (int)((w >> 5) & 31);
        return 1;
    }
    /* LDR/STR: unsigned offset, unscaled (LDUR/STUR), register offset */
    if ((w & 0x3B000000u) == 0x39000000u ||
        (w & 0x3B200000u) == 0x38000000u ||
        (w & 0x3B200C00u) == 0x38200800u) {
        int size = (int)((w >> 30) & 3);
        int opc = (int)((w >> 22) & 3);
        if (opc > 1) return 0; /* PRFM / нестандартные */
        m->ok = 1;
        m->is_load = (opc == 1);
        m->width = 1 << size;
        m->base = (int)((w >> 5) & 31);
        if ((w & 0x3B200C00u) == 0x38200800u) {
            m->index = (int)((w >> 16) & 31);
            m->imm = 0;
        } else if ((w & 0x3B000000u) == 0x39000000u) {
            m->imm = (int)((w >> 10) & 0xFFF) << size;
        } else {
            int imm9 = (int)((w >> 12) & 0x1FF);
            if (imm9 & 0x100) imm9 -= 0x200;
            m->imm = imm9;
        }
        return 1;
    }
    return 0;
}

static uint32_t enc_mov_x(int rd, int rn) {
    return 0xAA0003E0u | ((uint32_t)rn << 16) | (uint32_t)rd;
}
static uint32_t enc_add_x(int rd, int rn, int rm) {
    return 0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_bl(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if (d < -(1 << 27) || d >= (1 << 27)) return 0;
    return 0x94000000u | (uint32_t)((d >> 2) & 0x3FFFFFF);
}
static uint32_t enc_b(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if (d < -(1 << 27) || d >= (1 << 27)) return 0;
    return 0x14000000u | (uint32_t)((d >> 2) & 0x3FFFFFF);
}
static void patch_bcond(uint8_t *p, uint64_t pc, uint64_t target) {
    int64_t d = (int64_t)target - (int64_t)pc;
    uint32_t imm = (uint32_t)((d >> 2) & 0x7FFFF);
    uint32_t w = rd32(p);
    wr32(p, (w & 0xFF00001Fu) | (imm << 5)); /* imm19 = биты 23..5, cond = 3..0 */
}

/* Прокрутка сетки по кругу (Инструменты/Консоли/Коллекции): на первой строке
 * вверх — на последнюю, на последней вниз — на первую. Две ветви-«стоп»
 * перенаправляем на свои заглушки в кейве. */
static const uint32_t grid_sig[10] = {
    0x710002ff, 0x52800021, 0x5a811421, 0x2b010000, 0,
    0xb942fbe1, 0x6b01001f, 0, 0x6b04007f, 0x540077ed
};
static const uint8_t grid_up_code[32] = {
    0xe1, 0xfb, 0x42, 0xb9, 0x21, 0x04, 0x00, 0x51, 0x20, 0x7c, 0x03, 0x1b, 0x00, 0x00, 0x04, 0x0b,
    0x1f, 0x00, 0x19, 0x6b, 0x4b, 0x00, 0x00, 0x54, 0x20, 0x07, 0x00, 0x51, 0x00, 0x00, 0x00, 0x14
};
static const uint8_t grid_down_code[20] = {
    0xe0, 0x03, 0x04, 0x2a, 0x21, 0x07, 0x00, 0x51, 0x1f, 0x00, 0x19, 0x6b, 0x00, 0xb0, 0x81, 0x1a,
    0x00, 0x00, 0x00, 0x14
};
static uint64_t grid_up_va, grid_down_va;
static size_t grid_up_off, grid_down_off;
/* ==================== «Поиск игры в консоли» ====================
 * Новый пункт в меню игры (id 42): своя реализация поиска — штатная
 * клавиатура, затем копии записей текущей папки консоли, чьё отображаемое
 * имя начинается с запроса (без расширения файла). Оригинальный блок
 * поиска не вызывается. Код собирается в кейве.
 * Все адреса — из nextui.elf 2.5.1, применение защищено проверкой сигнатур. */
static size_t cb_off;
static size_t cs_label_off, cs_prompt_off, cs_roms_off, cs_tmpdir_off, cs_title_off;
static size_t cs_back_off;
static uint64_t cs_stub_item_va, cs_stub_item2_va, cs_stub_act_va;
static uint64_t cs_leave_hook_va, cs_hint_hook_va;
static int cs_built, cs_patched;

#define A_CTX_ADD     0x41c7b0ULL  /* addItem(items, &count, label, id) */
#define A_ENTRY_FOLDER 0x496cb0ULL /* entryFolderGame(entry) */
#define A_KBD         0x429c60ULL  /* UIKeyboard_openWith(prompt, initial, flags) */
#define A_POLL        0x432d10ULL  /* PLAT_pollInput() */
#define A_DIR_NEW     0x4a7b00ULL  /* Directory_new(path, n) */
#define A_ROWCOUNT    0x497370ULL  /* GameList_rowCountAt(false) */
#define A_ISSEARCH    0x496b50ULL  /* GameList_isSearch() */
#define A_ARRAY_PUSH  0x4ace30ULL  /* Array_push(arr, item) */
#define A_ENTRY_FREE  0x4acea0ULL  /* Entry_free(entry) */
#define A_ENTRY_NEW_NAMED 0x4adac0ULL /* Entry_newNamed(path,type,name) */
#define A_ARRAY_NEW    0x4ad150ULL /* Array_new() */
#define A_ENTRYARRAY_FREE 0x4acd00ULL /* EntryArray_free(arr) */
#define P_SNPRINTF    0x406710ULL
#define P_FREE        0x407350ULL
#define P_STRLEN      0x4074c0ULL
#define P_STRNCMP     0x406b30ULL
#define P_GETENV      0x407220ULL
#define P_SETENV      0x406640ULL
#define P_STRDUP      0x407200ULL
#define P_STRNCASECMP 0x406720ULL
#define P_ATOMICSET   0x4068c0ULL  /* SDL_AtomicSet(&flag, 1) */
#define A_BGREUPLOAD  0x4a18b0ULL  /* requestBackgroundReupload() */
#define A_PADRESET    0x431970ULL  /* PAD_reset() */
#define A_PUTFILE     0x43c640ULL  /* putFile(path, text) */
#define G_TOP         0x4d8da0ULL  /* Directory* top */
#define G_STACK       0x4d8e80ULL  /* Array* stack */
#define G_BGREUPLOAD  0x510428ULL
#define G_SEARCHQ     0x510430ULL  /* char search_query[256] */
#define G_PAD         0x50e0d0ULL
#define L_FAUXSEARCH  0x4ae930ULL  /* FAUX_SEARCH_PATH */
#define L_FMT_S       0x4b5588ULL  /* "%s" */

static void cb32(uint32_t w) { wr32(cave + cb_off, w); cb_off += 4; }

static uint64_t cbpc(void) { return cave_vaddr + cb_off; }

static void e_adrp(int rd, uint64_t target) { /* ADRP */
    uint64_t pc = cbpc();
    int64_t pages = ((int64_t)(target & ~(uint64_t)0xFFF) - (int64_t)(pc & ~(uint64_t)0xFFF)) >> 12;
    cb32(0x90000000u | ((uint32_t)(pages & 3) << 29) | ((uint32_t)((pages >> 2) & 0x7FFFF) << 5) | (uint32_t)rd);
}
static void e_addi(int rd, int rn, uint32_t imm) { /* ADD Xd, Xn, #imm */
    cb32(0x91000000u | ((imm & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void e_mov(int rd, int rm) { cb32(0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd); }
static void e_movi(int rd, uint32_t imm) { cb32(0x52800000u | ((imm & 0xFFFF) << 5) | (uint32_t)rd); }
static void e_bl(uint64_t target) { cb32(enc_bl(cbpc(), target)); }
static void e_b(uint64_t target) { cb32(enc_b(cbpc(), target)); }
static void e_bcond(int cond, uint64_t target) {
    int64_t d = (int64_t)target - (int64_t)cbpc();
    cb32(0x54000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | (uint32_t)cond);
}
static void e_cbz(int rt, uint64_t target, int nz) {
    int64_t d = (int64_t)target - (int64_t)cbpc();
    cb32((nz ? 0x35000000u : 0x34000000u) | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | (uint32_t)rt);
}
static void e_ldr_w(int rt, int rn, uint32_t off) {
    cb32(0xB9400000u | ((off / 4) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_ldr_x(int rt, int rn, uint32_t off) {
    cb32(0xF9400000u | ((off / 8) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_str_x(int rt, int rn, uint32_t off) {
    cb32(0xF9000000u | ((off / 8) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_str_w(int rt, int rn, uint32_t off) {
    cb32(0xB9000000u | ((off / 4) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_ldsw(int rt, int rn, uint32_t off) {
    cb32(0xB9800000u | ((off / 4) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_ldrb(int rt, int rn, uint32_t off) {
    cb32(0x39400000u | (off << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_cmp_i(int rn, uint32_t imm) { cb32(0x71000000u | ((imm & 0xFFF) << 10) | ((uint32_t)rn << 5) | 31u); }
static void e_cmp_r(int rn, int rm) { cb32(0x6B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | 31u); }
static void e_add_w(int rd, int rn, uint32_t imm) { cb32(0x11000000u | ((imm & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd); }
static void e_stp_pre(int rt, int rt2, int rn, int imm7) {
    cb32(0xA9800000u | ((uint32_t)(imm7 & 0x7F) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_stp_off(int rt, int rt2, int rn, int imm7) {
    cb32(0xA9000000u | ((uint32_t)(imm7 & 0x7F) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_ldp_off(int rt, int rt2, int rn, int imm7) {
    cb32(0xA9400000u | ((uint32_t)(imm7 & 0x7F) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_ldp_post(int rt, int rt2, int rn, int imm7) {
    cb32(0xA8C00000u | ((uint32_t)(imm7 & 0x7F) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_str_reg(int rt, int rn, int rm, int shift) {
    cb32(0xF8200800u | ((uint32_t)rm << 16) | (3u << 13) | (shift ? 0x1000u : 0u) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_ldr_reg(int rt, int rn, int rm, int shift) {
    cb32(0xF8600800u | ((uint32_t)rm << 16) | (3u << 13) | (shift ? 0x1000u : 0u) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_csel(int rd, int rn, int rm, int cond) {
    cb32(0x1A800000u | ((uint32_t)rm << 16) | ((uint32_t)cond << 12) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static void e_ret(void) { cb32(0xD65F03C0u); }
static void e_movn(int rd, uint32_t imm16) { cb32(0x12800000u | ((imm16 & 0xFFFF) << 5) | (uint32_t)rd); }
static void e_sub_w(int rd, int rn, uint32_t imm) { cb32(0x51000000u | ((imm & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd); }
static void e_sub_reg(int rd, int rn, int rm) { cb32(0xCB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd); }
static void e_add_reg_shift(int rd, int rn, int rm, int shift) { cb32(0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)(shift & 0x3F) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd); }
static uint64_t cs_str(size_t *off, const char *s) {
    size_t n = strlen(s) + 1;
    while (cb_off % 8) cave[cb_off++] = 0;
    *off = cb_off;
    memcpy(cave + cb_off, s, n);
    cb_off += n;
    return cave_vaddr + (uint64_t)*off;
}


static int fix_grid_wrap(uint8_t *code, uint64_t len, uint64_t taddr) {
    if (!grid_up_va || !grid_down_va) return 0;
    for (uint64_t off = 0; off + 40 <= len; off += 4) {
        int ok = 1;
        for (int i = 0; i < 10 && ok; i++) {
            if (i == 4 || i == 7) continue;
            if (rd32(code + off + (uint64_t)i * 4) != grid_sig[i]) ok = 0;
        }
        if (!ok) continue;
        if ((rd32(code + off + 16) & 0xFF000010u) != 0x54000000u) continue;
        if ((rd32(code + off + 28) & 0xFF000010u) != 0x54000000u) continue;

        uint64_t ret = taddr + off + 0x38;
        uint32_t b_up = enc_b(grid_up_va + 28, ret);
        uint32_t b_dn = enc_b(grid_down_va + 16, ret);
        if (!b_up || !b_dn) continue;
        wr32(cave + grid_up_off + 28, b_up);
        wr32(cave + grid_down_off + 16, b_dn);
        patch_bcond(code + off + 16, taddr + off + 16, grid_up_va);
        patch_bcond(code + off + 28, taddr + off + 28, grid_down_va);
        return 1;
    }
    return 0;
}
static int allow_relax;

static void e_ldrb_reg(int rt, int rn, int rm) {
    cb32(0x38600800u | ((uint32_t)rm << 16) | (3u << 13) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void e_stp_w_off(int rt, int rt2, int rn, uint32_t off) {
    cb32(0x29000000u | (((off / 4) & 0x7F) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static void fix_bcond_cave(size_t at, int cond, uint64_t target) {
    int64_t d = (int64_t)target - (int64_t)(cave_vaddr + at);
    wr32(cave + at, 0x54000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | (uint32_t)cond);
}
static void fix_cbz64_cave(size_t at, uint64_t target, int rt) {
    int64_t d = (int64_t)target - (int64_t)(cave_vaddr + at);
    wr32(cave + at, 0xB4000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | (uint32_t)rt);
}
static void fix_cbz_cave(size_t at, uint64_t target, int rt) {
    int64_t d = (int64_t)target - (int64_t)(cave_vaddr + at);
    wr32(cave + at, 0x34000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | (uint32_t)rt);
}

static uint64_t g_mark_log;
static uint64_t g_mark_st[16];
static void e_mark(int n) {
    e_adrp(0, g_mark_log); e_addi(0, 0, (uint32_t)(g_mark_log & 0xFFF));
    e_adrp(1, g_mark_st[n]); e_addi(1, 1, (uint32_t)(g_mark_st[n] & 0xFFF));
    e_bl(A_PUTFILE);
}
/* «Поиск игры в консоли»: своя реализация.
 * Пункт меню -> клавиатура -> результаты (обычный Directory поверх списка).
 * Назад в результатах -> клавиатура с прошлым запросом; отмена в ней -> список игр.
 * Поиск идёт по началу отображаемого имени в текущей папке консоли. */
static int build_console_search(void) {
    if (cave_len + 4096 > max_cave) return 0;
    cb_off = cave_len;
    while (cb_off % 8) cave[cb_off++] = 0;

    uint64_t s_label = cs_str(&cs_label_off, "Поиск игры в консоли");
    uint64_t s_prompt = cs_str(&cs_prompt_off, "Поиск в консоли");
    uint64_t s_roms = cs_str(&cs_roms_off, "/mnt/SDCARD/Roms/");
    uint64_t s_tmpdir = cs_str(&cs_tmpdir_off, "/tmp/.nxsearch");
    uint64_t s_title = cs_str(&cs_title_off, "Поиск: %s");
    uint64_t s_back = cs_str(&cs_back_off, "НАЗАД В ПОИСК");
    while (cb_off % 8) cave[cb_off++] = 0;

    /* ==================== заполнение результатов ====================
     * x0 = Directory* консоли, x1 = Directory* результатов.
     * Копирует записи, чьё имя начинается с search_query, и считает alphas. */
    uint64_t fill_va = cave_vaddr + cb_off;
    e_stp_pre(19, 20, 31, -12);          /* 0x60 */
    e_stp_off(21, 22, 31, 2);
    e_stp_off(23, 24, 31, 4);
    e_stp_off(25, 26, 31, 6);
    e_stp_off(27, 28, 31, 8);
    e_str_x(30, 31, 0x50);
    e_mov(19, 0);                        /* console */
    e_mov(20, 1);                        /* dir */
    e_adrp(0, G_SEARCHQ); e_addi(0, 0, (uint32_t)(G_SEARCHQ & 0xFFF));
    e_bl(P_STRLEN);
    e_mov(22, 0);                        /* qlen */
    e_ldr_x(23, 19, 0x10);               /* console->entries */
    e_ldr_w(24, 23, 0);                  /* count */
    e_ldr_x(25, 23, 8);                  /* items */
    e_ldr_x(26, 20, 0x10);               /* dir->entries */
    e_movi(27, 0);                       /* i */
    uint64_t f_loop = cbpc();
    e_cmp_r(27, 24);
    size_t f_ge = cb_off; cb32(0);       /* b.ge -> fill_done */
    e_ldr_reg(28, 25, 27, 3);            /* e */
    size_t f_n0 = cb_off; cb32(0);       /* cbz x28 -> f_next */
    e_ldr_x(0, 28, 8);                   /* e->name */
    size_t f_n1 = cb_off; cb32(0);       /* cbz x0 -> f_next */
    e_adrp(1, G_SEARCHQ); e_addi(1, 1, (uint32_t)(G_SEARCHQ & 0xFFF));
    e_mov(2, 22);
    e_bl(P_STRNCASECMP);
    size_t f_nz = cb_off; cb32(0);       /* cbnz w0 -> f_next */
    e_ldr_x(0, 28, 0);
    e_ldr_w(1, 28, 0x18);
    e_ldr_x(2, 28, 8);
    e_bl(A_ENTRY_NEW_NAMED);
    size_t f_nc = cb_off; cb32(0);       /* cbz x0 -> f_next */
    e_mov(21, 0);                        /* copy */
    e_ldr_x(1, 28, 0x10);                /* unique */
    size_t f_nu = cb_off; cb32(0);       /* cbz x1 -> f_push */
    e_mov(0, 1);
    e_bl(P_STRDUP);
    e_str_x(0, 21, 0x10);
    uint64_t f_push = cbpc();
    e_mov(0, 26);
    e_mov(1, 21);
    e_bl(A_ARRAY_PUSH);
    uint64_t f_next = cbpc();
    e_add_w(27, 27, 1);
    e_b(f_loop);
    uint64_t f_done = cbpc();

    /* --- alphas по первой букве имени (для L1/R1) --- */
    e_ldr_w(24, 26, 0);                  /* arr->count */
    e_ldr_x(25, 26, 8);                  /* arr->items */
    e_movi(27, 0);                       /* i */
    e_movn(6, 0);                        /* prev = -1 */
    e_movi(7, 0);                        /* alpha index */
    e_movi(8, 0);                        /* groups */
    uint64_t a_loop = cbpc();
    e_cmp_r(27, 24);
    size_t a_ge = cb_off; cb32(0);       /* b.ge -> a_done */
    e_ldr_reg(28, 25, 27, 3);
    size_t a_n0 = cb_off; cb32(0);       /* cbz x28 -> a_next */
    e_ldr_x(0, 28, 8);                   /* name */
    size_t a_n1 = cb_off; cb32(0);       /* cbz x0 -> a_next */
    e_ldrb(4, 0, 0);                     /* c = name[0] */
    e_sub_w(5, 4, 0x41);                 /* c - 'A' */
    e_cmp_i(5, 25);
    size_t a_u = cb_off; cb32(0);        /* b.hi -> a_l1 */
    e_add_w(4, 4, 32);                   /* tolower A-Z */
    uint64_t a_l1 = cbpc();
    e_cmp_i(4, 0x61);                    /* 'a' */
    size_t a_lo = cb_off; cb32(0);       /* b.lo -> a_l2 */
    e_cmp_i(4, 0x7a);                    /* 'z' */
    size_t a_hi = cb_off; cb32(0);       /* b.hi -> a_l2 */
    e_sub_w(4, 4, 0x61);
    e_add_w(4, 4, 1);                    /* bucket = c-'a'+1 */
    size_t a_b3 = cb_off; cb32(0);       /* b -> a_l3 */
    uint64_t a_l2 = cbpc();
    e_cmp_i(4, 0x7a);
    e_movi(5, 27);
    e_csel(4, 5, 31, 0x8 /*HI*/);        /* bucket = c>'z' ? 27 : 0 */
    uint64_t a_l3 = cbpc();
    e_cmp_r(4, 6);
    size_t a_same = cb_off; cb32(0);     /* b.eq -> a_same */
    e_cmp_i(8, 28);                      /* INT_ARRAY_MAX */
    size_t a_full = cb_off; cb32(0);     /* b.ge -> a_no_append */
    e_addi(9, 20, 0x1c);                 /* &alphas.items */
    e_add_reg_shift(9, 9, 8, 2);         /* + count*4 */
    e_str_w(27, 9, 0);                   /* items[count] = i */
    e_mov(7, 8);                         /* alpha = count */
    e_add_w(8, 8, 1);
    e_str_w(8, 20, 0x18);                /* alphas.count */
    uint64_t a_no_append = cbpc();
    e_mov(6, 4);                         /* prev = bucket */
    uint64_t l_same = cbpc();
    e_str_w(7, 28, 0x1c);                /* entry->alpha */
    uint64_t a_next = cbpc();
    e_add_w(27, 27, 1);
    e_b(a_loop);
    uint64_t a_done = cbpc();
    e_str_w(8, 20, 0x18);
    e_ldr_x(30, 31, 0x50);
    e_ldp_off(27, 28, 31, 8);
    e_ldp_off(25, 26, 31, 6);
    e_ldp_off(23, 24, 31, 4);
    e_ldp_off(21, 22, 31, 2);
    e_ldp_post(19, 20, 31, 12);
    e_ret();
    /* fixups fill */
    fix_bcond_cave(f_ge, 0xA, f_done);
    fix_cbz64_cave(f_n0, f_next, 28);
    fix_cbz64_cave(f_n1, f_next, 0);
    {   int64_t d = (int64_t)f_next - (int64_t)(cave_vaddr + f_nz);
        wr32(cave + f_nz, 0x35000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | 0u); }
    fix_cbz64_cave(f_nc, f_next, 0);
    fix_cbz64_cave(f_nu, f_push, 1);
    /* fixups alphas */
    fix_bcond_cave(a_ge, 0xA, a_done);
    fix_cbz64_cave(a_n0, a_next, 28);
    fix_cbz64_cave(a_n1, a_next, 0);
    fix_bcond_cave(a_u, 0x8, a_l1);
    fix_bcond_cave(a_lo, 0x3, a_l2);
    fix_bcond_cave(a_hi, 0x8, a_l2);
    {   int64_t d = (int64_t)a_l3 - (int64_t)(cave_vaddr + a_b3);
        wr32(cave + a_b3, 0x14000000u | (uint32_t)((d >> 2) & 0x3FFFFFF)); }
    fix_bcond_cave(a_same, 0x0, l_same);
    fix_bcond_cave(a_full, 0xA, a_no_append);

    /* ==================== сам поиск ====================
     * Без аргументов. Если top — результаты, режим редактирования:
     * клавиатура с прошлым запросом, замена списка. Иначе — новый поиск.
     * Возвращает 1 (показать/обновить) или 0 (отмена). */
    uint64_t search_va = cave_vaddr + cb_off;
    e_stp_pre(19, 20, 31, -46);          /* 0x170, буфер заголовка 0x60 */
    e_stp_off(21, 22, 31, 2);
    e_stp_off(23, 24, 31, 4);
    e_stp_off(25, 26, 31, 6);
    e_stp_off(27, 28, 31, 8);
    e_str_x(30, 31, 0x50);
    size_t s_cancel = 0, s_ret1 = 0;
    e_bl(A_ISSEARCH);
    e_mov(23, 0);                        /* edit = isSearch */
    size_t p_ini = cb_off; cb32(0);      /* cbz w0 -> initial_console */
    /* edit: консоль = stack[count-2] */
    e_adrp(0, G_STACK); e_ldr_x(0, 0, (uint32_t)(G_STACK & 0xFFF));
    e_ldr_w(1, 0, 0);
    e_cmp_i(1, 2);
    size_t p_sc = cb_off; cb32(0);       /* b.lt cancel */
    e_ldr_x(2, 0, 8);
    e_sub_w(1, 1, 2);
    e_ldr_reg(20, 2, 1, 3);              /* console */
    size_t p_cr = cb_off; cb32(0);       /* b -> console_ready */
    uint64_t l_initial_console = cbpc();
    e_adrp(0, G_TOP); e_ldr_x(20, 0, (uint32_t)(G_TOP & 0xFFF));
    uint64_t l_console_ready = cbpc();
    size_t p_ct = cb_off; cb32(0);       /* cbz x20 -> cancel */
    /* клавиатура: initial = edit ? search_query : NULL */
    e_adrp(0, s_prompt); e_addi(0, 0, (uint32_t)(s_prompt & 0xFFF));
    e_movi(1, 0);
    size_t p_noi = cb_off; cb32(0);      /* cbz w23 -> kbd */
    e_adrp(1, G_SEARCHQ); e_addi(1, 1, (uint32_t)(G_SEARCHQ & 0xFFF));
    uint64_t l_kbd = cbpc();
    e_movi(2, 1);                        /* KB_START_CANCELS */
    e_bl(A_KBD);
    e_mov(21, 0);                        /* query */
    e_bl(A_BGREUPLOAD);
    e_bl(A_POLL);
    e_bl(A_PADRESET);
    size_t p_nq = cb_off; cb32(0);       /* cbz x21 -> cancel */
    e_ldrb(0, 21, 0);
    size_t p_eq0 = cb_off; cb32(0);      /* cbz w0 -> cancel */
    /* search_query = query */
    e_adrp(0, G_SEARCHQ); e_addi(0, 0, (uint32_t)(G_SEARCHQ & 0xFFF));
    e_movi(1, 0x100);
    e_adrp(2, L_FMT_S); e_addi(2, 2, (uint32_t)(L_FMT_S & 0xFFF));
    e_mov(3, 21);
    e_bl(P_SNPRINTF);
    e_mov(0, 21); e_bl(P_FREE);
    /* заголовок "Поиск: q" в буфер на стеке */
    e_addi(0, 31, 0x60);
    e_movi(1, 0x100);
    e_adrp(2, s_title); e_addi(2, 2, (uint32_t)(s_title & 0xFFF));
    e_adrp(3, G_SEARCHQ); e_addi(3, 3, (uint32_t)(G_SEARCHQ & 0xFFF));
    e_bl(P_SNPRINTF);
    /* edit -> top, initial -> новый Directory */
    size_t p_edit = cb_off; cb32(0);     /* cbnz w23 -> edit_build */
    e_adrp(0, s_tmpdir); e_addi(0, 0, (uint32_t)(s_tmpdir & 0xFFF));
    e_movi(1, 0);
    e_bl(A_DIR_NEW);
    e_mov(19, 0);
    size_t p_dn = cb_off; cb32(0);       /* cbz x19 -> cancel */
    e_ldr_x(0, 19, 0); e_bl(P_FREE);
    e_adrp(0, L_FAUXSEARCH); e_addi(0, 0, (uint32_t)(L_FAUXSEARCH & 0xFFF));
    e_bl(P_STRDUP);
    e_str_x(0, 19, 0);
    size_t p_common = cb_off; cb32(0);   /* b -> common */
    uint64_t l_edit_build = cbpc();
    e_adrp(0, G_TOP); e_ldr_x(19, 0, (uint32_t)(G_TOP & 0xFFF));
    size_t p_et = cb_off; cb32(0);       /* cbz x19 -> cancel */
    uint64_t l_common = cbpc();
    /* общее: заменить entries и name */
    e_ldr_x(0, 19, 0x10); e_bl(A_ENTRYARRAY_FREE);
    e_bl(A_ARRAY_NEW);
    e_str_x(0, 19, 0x10);
    e_ldr_x(0, 19, 8); e_bl(P_FREE);
    e_addi(0, 31, 0x60);
    e_bl(P_STRDUP);
    e_str_x(0, 19, 8);
    /* заполнить */
    e_mov(0, 20); e_mov(1, 19);
    e_bl(fill_va);
    e_str_w(31, 19, 0x8c);               /* selected = 0 */
    e_movi(0, 0); e_bl(A_ROWCOUNT);
    e_ldr_x(1, 19, 0x10); e_ldr_w(2, 1, 0);
    e_movi(3, 1);
    e_cmp_i(0, 0);
    e_csel(0, 0, 3, 0xC /*GT*/);
    e_cmp_r(0, 2);
    e_csel(0, 0, 2, 0xD /*LE*/);
    e_stp_w_off(31, 0, 19, 0x90);        /* start=0, end */
    /* initial: push + top */
    size_t p_push = cb_off; cb32(0);     /* cbnz w23 -> pushed (edit не пушит) */
    e_adrp(0, G_STACK); e_ldr_x(0, 0, (uint32_t)(G_STACK & 0xFFF));
    e_mov(1, 19);
    e_bl(A_ARRAY_PUSH);
    e_adrp(0, G_TOP); e_str_x(19, 0, (uint32_t)(G_TOP & 0xFFF));
    uint64_t l_pushed = cbpc();
    e_movi(0, 1); e_bl(0x4117d0);        /* LAYER_SCROLLTEXT */
    e_bl(0x40c6f0);                      /* ScrollText_clear */
    e_ldr_x(1, 19, 0x10); e_ldr_w(2, 1, 0);
    e_cmp_i(2, 0);
    size_t p_nosel = cb_off; cb32(0);    /* b.le -> ret1 */
    e_ldr_x(0, 1, 8);
    e_ldr_x(0, 0, 0);
    e_bl(0x4a6200);                      /* readyResume(items[0]) */
    s_ret1 = cbpc();
    e_movi(0, 1);
    size_t p_ret = cb_off; cb32(0);      /* b -> ret */
    s_cancel = cbpc();
    e_movi(0, 0);
    uint64_t s_ret = cbpc();
    e_ldr_x(30, 31, 0x50);
    e_ldp_off(27, 28, 31, 8);
    e_ldp_off(25, 26, 31, 6);
    e_ldp_off(23, 24, 31, 4);
    e_ldp_off(21, 22, 31, 2);
    e_ldp_post(19, 20, 31, 46);
    e_ret();
    /* fixups search */
    fix_cbz_cave(p_ini, l_initial_console, 0);
    fix_bcond_cave(p_sc, 0xB /*LT*/, s_cancel);
    {   int64_t d = (int64_t)l_console_ready - (int64_t)(cave_vaddr + p_cr);
        wr32(cave + p_cr, 0x14000000u | (uint32_t)((d >> 2) & 0x3FFFFFF)); }
    fix_cbz64_cave(p_ct, s_cancel, 20);
    fix_cbz_cave(p_noi, l_kbd, 23);
    fix_cbz64_cave(p_nq, s_cancel, 21);
    fix_cbz_cave(p_eq0, s_cancel, 0);
    {   int64_t d = (int64_t)l_edit_build - (int64_t)(cave_vaddr + p_edit);
        wr32(cave + p_edit, 0x35000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | 23u); }
    {   int64_t d = (int64_t)l_common - (int64_t)(cave_vaddr + p_common);
        wr32(cave + p_common, 0x14000000u | (uint32_t)((d >> 2) & 0x3FFFFFF)); }
    fix_cbz64_cave(p_et, s_cancel, 19);
    fix_cbz64_cave(p_dn, s_cancel, 19);
    {   int64_t d = (int64_t)l_pushed - (int64_t)(cave_vaddr + p_push);
        wr32(cave + p_push, 0x35000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | 23u); }
    fix_bcond_cave(p_nosel, 0xD, s_ret1);
    {   int64_t d = (int64_t)s_ret - (int64_t)(cave_vaddr + p_ret);
        wr32(cave + p_ret, 0x14000000u | (uint32_t)((d >> 2) & 0x3FFFFFF)); }

    /* ==================== leaveList: назад в поиск ====================
     * Если top — результаты, открыть клавиатуру (прошлый запрос).
     * Подтвердили — остаёмся на обновлённых результатах; отмена — выходим. */
    cs_leave_hook_va = cave_vaddr + cb_off;
    e_stp_pre(19, 20, 31, -8);           /* 0x40 */
    e_stp_off(0, 1, 31, 2);              /* 0x10 */
    e_stp_off(2, 3, 31, 4);              /* 0x20 */
    e_stp_off(29, 30, 31, 6);            /* 0x30 */
    e_bl(A_ISSEARCH);
    size_t lv_orig = 0;
    size_t lv_no = cb_off; cb32(0);      /* cbz w0 -> leave_orig */
    e_bl(search_va);
    size_t lv_stay = cb_off; cb32(0);    /* cbz w0 -> leave_orig */
    e_ldp_off(2, 3, 31, 4);
    e_ldp_off(0, 1, 31, 2);
    e_ldp_off(29, 30, 31, 6);
    e_ldp_post(19, 20, 31, 8);
    e_ret();
    lv_orig = cbpc();
    e_ldp_off(2, 3, 31, 4);
    e_ldp_off(0, 1, 31, 2);
    e_ldp_off(29, 30, 31, 6);
    e_ldp_post(19, 20, 31, 8);
    e_stp_pre(19, 20, 31, -10);          /* оригинальный пролог 0x50 */
    e_b(0x416604ULL);
    fix_cbz_cave(lv_no, lv_orig, 0);
    fix_cbz_cave(lv_stay, lv_orig, 0);

    /* ==================== подсказки на результатах ====================
     * B «Назад в меню поиска», X «Продолжить» (если есть), A «Открыть».
     * Только для renderHints (0x4993f0..0x499898); остальные экраны не трогаем. */
    cs_hint_hook_va = cave_vaddr + cb_off;
    e_stp_pre(19, 20, 31, -8);           /* 0x40 */
    e_stp_off(0, 1, 31, 2);
    e_stp_off(2, 3, 31, 4);
    e_stp_off(29, 30, 31, 6);
    e_mov(19, 1);                        /* pairs */
    e_adrp(9, 0x4993f0ULL); e_addi(9, 9, 0x3f0);
    e_sub_reg(9, 30, 9);
    e_cmp_i(9, 0x4a8);
    size_t h_hi = cb_off; cb32(0);       /* b.hi -> pass */
    e_bl(A_ISSEARCH);
    size_t h_no = cb_off; cb32(0);       /* cbz w0 -> pass */
    /* первый пара — B/BACK; меняем только текст, остальные подсказки
     * (Y netplay, X resume, A open) остаются штатными и условными */
    e_adrp(0, s_back); e_addi(0, 0, (uint32_t)(s_back & 0xFFF));
    e_str_x(0, 19, 8);
    e_movi(0, 24);
    e_str_x(0, 31, 0x28);                /* left = 24 (saved x3) */
    uint64_t h_pass = cbpc();
    e_ldp_off(2, 3, 31, 4);
    e_ldp_off(0, 1, 31, 2);
    e_ldp_off(29, 30, 31, 6);
    e_ldp_post(19, 20, 31, 8);
    cb32(0xD10843FFu);                   /* sub sp,sp,#0x210 */
    e_b(0x4293c4ULL);
    fix_bcond_cave(h_hi, 0x8 /*HI*/, h_pass);
    fix_cbz_cave(h_no, h_pass, 0);

    /* ==================== обработчик id 42 ==================== */
    cs_stub_act_va = cave_vaddr + cb_off;
    e_cmp_i(0, 42);
    size_t p_ne = cb_off; cb32(0);
    e_stp_pre(29, 30, 31, -2);
    e_bl(search_va);
    e_ldp_post(29, 30, 31, 2);
    e_ret();
    uint64_t l_orig = cbpc();
    e_stp_pre(19, 20, 31, -16);
    e_b(0x49c334ULL);
    fix_bcond_cave(p_ne, 0x1, l_orig);

    /* ==================== пункт меню ==================== */
    cs_stub_item_va = cave_vaddr + cb_off;
    e_adrp(0, G_TOP); e_ldr_x(0, 0, (uint32_t)(G_TOP & 0xFFF));
    size_t g1 = cb_off; cb32(0);
    e_ldr_x(0, 0, 0);
    size_t g2 = cb_off; cb32(0);
    e_adrp(1, s_roms); e_addi(1, 1, (uint32_t)(s_roms & 0xFFF));
    e_movi(2, 17);
    e_bl(P_STRNCMP);
    size_t g3 = cb_off; cb32(0);
    e_ldr_w(0, 19, 0x18);
    e_cmp_i(0, 2);
    size_t p_eq = cb_off; cb32(0);
    e_mov(0, 19);
    e_bl(A_ENTRY_FOLDER);
    size_t p_cbz = cb_off; cb32(0);
    uint64_t l_add = cbpc();
    e_addi(0, 31, 0x240);
    e_addi(1, 31, 0x3c);
    e_adrp(2, s_label); e_addi(2, 2, (uint32_t)(s_label & 0xFFF));
    e_movi(3, 42);
    e_bl(A_CTX_ADD);
    uint64_t l_skip = cbpc();
    e_ldr_w(19, 31, 0x3c);
    e_b(0x41c914ULL);
    fix_bcond_cave(p_eq, 0x0, l_add);
    fix_cbz_cave(p_cbz, l_skip, 0);
    fix_cbz64_cave(g1, l_skip, 0);
    fix_cbz64_cave(g2, l_skip, 0);
    {   int64_t d = (int64_t)l_skip - (int64_t)(cave_vaddr + g3);
        wr32(cave + g3, 0x35000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | 0u); }

    cs_stub_item2_va = cave_vaddr + cb_off;
    e_adrp(0, G_TOP); e_ldr_x(0, 0, (uint32_t)(G_TOP & 0xFFF));
    size_t h1 = cb_off; cb32(0);
    e_ldr_x(0, 0, 0);
    size_t h2 = cb_off; cb32(0);
    e_adrp(1, s_roms); e_addi(1, 1, (uint32_t)(s_roms & 0xFFF));
    e_movi(2, 17);
    e_bl(P_STRNCMP);
    size_t h3 = cb_off; cb32(0);
    e_ldr_w(0, 21, 0x18);
    e_cmp_i(0, 2);
    size_t q_eq = cb_off; cb32(0);
    e_mov(0, 21);
    e_bl(A_ENTRY_FOLDER);
    size_t q_cbz = cb_off; cb32(0);
    uint64_t q_add = cbpc();
    e_addi(0, 31, 0x2d0);
    e_addi(1, 31, 0xd0);
    e_adrp(2, s_label); e_addi(2, 2, (uint32_t)(s_label & 0xFFF));
    e_movi(3, 42);
    e_bl(A_CTX_ADD);
    uint64_t q_skip = cbpc();
    e_ldr_w(1, 31, 0xd0);
    e_b(0x41e3bcULL);
    fix_bcond_cave(q_eq, 0x0, q_add);
    fix_cbz_cave(q_cbz, q_skip, 0);
    fix_cbz64_cave(h1, q_skip, 0);
    fix_cbz64_cave(h2, q_skip, 0);
    {   int64_t d = (int64_t)q_skip - (int64_t)(cave_vaddr + h3);
        wr32(cave + h3, 0x35000000u | (((uint32_t)((d >> 2) & 0x7FFFF)) << 5) | 0u); }

    cave_len = cb_off;
    return 1;
}


static int patch_console_search(void) {
    if (!cs_stub_item_va || !cs_stub_item2_va || !cs_stub_act_va || !cs_leave_hook_va || !cs_hint_hook_va) return 0;
    uint8_t *p = file_ptr(0x41c910ULL, 4);
    uint8_t *q = file_ptr(0x49c330ULL, 4);
    uint8_t *r = file_ptr(0x41e3b8ULL, 4);
    uint8_t *lv = file_ptr(0x416600ULL, 4);
    uint8_t *hb = file_ptr(0x4293c0ULL, 4);
    if (!p || !q || !r || !lv || !hb) return 0;
    if (rd32(p) != 0xB9403FF3u) return 0; /* ldr w19,[sp,#0x3c] */
    if (rd32(r) != 0xB940D3E1u) return 0; /* ldr w1,[sp,#0xd0] */
    if (rd32(q) != 0xA9B853F3u) return 0; /* stp x19,x20,[sp,#-0x80]! */
    if (rd32(lv) != 0xA9BB53F3u) return 0; /* stp x19,x20,[sp,#-0x50]! */
    if (rd32(hb) != 0xD10843FFu) return 0; /* sub sp,sp,#0x210 */
    wr32(p, enc_b(0x41c910ULL, cs_stub_item_va));
    wr32(r, enc_b(0x41e3b8ULL, cs_stub_item2_va));
    wr32(q, enc_b(0x49c330ULL, cs_stub_act_va));
    wr32(lv, enc_b(0x416600ULL, cs_leave_hook_va));
    wr32(hb, enc_b(0x4293c0ULL, cs_hint_hook_va));
    return 1;
}

/*
 * Компилятор развернул addItem(..., "литерал", id) в копию фиксированной длины
 * (strlen(литерала)+1 байт) — при подмене литерала на длинную русскую строку
 * метка обрезается посреди UTF-8. Находим такую копию (4 инструкции подряд:
 * загрузки из регистра строки + записи в приёмник) и заменяем её на вызов
 * помощника, который копирует до NUL.
 */
static int fix_label_copy(uint64_t add_addr, int R, size_t en_len) {
    if (!helper_va) return 0;
    for (int i = 1; i <= 6; i++) {
        uint64_t a = add_addr + (uint64_t)i * 4;
        uint8_t *p0 = file_ptr(a, 16);
        if (!p0) return 0;
        Mem g[4];
        int bad = 0;
        for (int j = 0; j < 4; j++)
            if (!decode_mem(rd32(p0 + j * 4), &g[j])) { bad = 1; break; }
        if (bad) continue;
        if (!g[0].is_load || g[0].base != R) continue;

        int cov[256];
        memset(cov, 0, sizeof(cov));
        int maxb = 0, loads = 0, stores = 0, dbase = -1, dindex = -1;
        for (int j = 0; j < 4 && !bad; j++) {
            if (g[j].is_load) {
                if (g[j].base != R) { bad = 1; break; }
                loads++;
                int o = g[j].imm;
                for (int k = 0; k < g[j].width && o + k < 256; k++) cov[o + k] = 1;
                if (o + g[j].width > maxb) maxb = o + g[j].width;
            } else {
                stores++;
                if (dbase < 0) { dbase = g[j].base; dindex = g[j].index; }
            }
        }
        if (bad || !loads || !stores || dbase < 0) continue;
        if (maxb != (int)en_len + 1) continue;   /* копируется ровно литерал+NUL */
        for (int k = 0; k < maxb; k++)
            if (!cov[k]) { bad = 1; break; }
        if (bad) continue;
        /* приёмник — поле метки структуры 68 байт: сразу после копии туда
         * кладут id по смещению 0x40 (str wN, [xM, #0x40]); это отсекает
         * копии обычных структур/полей в чужих бинарниках */
        if (dbase == 31) continue;                       /* sp — не наш случай */
        {
            int first_store = -1;
            for (int j = 0; j < 4; j++) if (!g[j].is_load) { first_store = j; break; }
            if (first_store < 0 || g[first_store].imm != 0) continue;
        }
        {
            int okid = 0;
            uint64_t pc = a + 16;
            for (int hop = 0; hop < 3 && !okid; hop++) {
                uint64_t next = 0;
                for (int j = 0; j < 6; j++) {
                    uint8_t *q = file_ptr(pc + (uint64_t)j * 4, 4);
                    if (!q) break;
                    uint32_t w = rd32(q);
                    if ((w & 0xFFC00000u) == 0xB9000000u && ((w >> 10) & 0xFFF) == 0x10) { okid = 1; break; } /* [xN,#0x40] */
                    if ((w & 0xFC000000u) == 0x14000000u) { /* b — идём по переходу */
                        int64_t imm = (int64_t)(w & 0x3FFFFFF);
                        if (imm & 0x2000000) imm -= 0x4000000;
                        next = pc + (uint64_t)j * 4 + (uint64_t)(imm * 4);
                        break;
                    }
                }
                if (!next) break;
                pc = next;
            }
            if (!okid) continue;
        }

        uint32_t ins[4];
        int n = 0;
        ins[n++] = (dindex >= 0) ? enc_add_x(16, dbase, dindex) : enc_mov_x(16, dbase);
        if (R != 17) ins[n++] = enc_mov_x(17, R);
        uint32_t bl = enc_bl(a + (uint64_t)n * 4, helper_va);
        if (!bl) continue;
        ins[n++] = bl;
        while (n < 4) ins[n++] = 0xD503201Fu; /* nop */

        uint8_t *p = file_ptr(a, 16);
        if (!p) continue;
        for (int j = 0; j < 4; j++) wr32(p + j * 4, ins[j]);
        if (getenv("NXPATCH_VERBOSE"))
            fprintf(stderr, "copy site %llx literal %u bytes\n", (unsigned long long)a, (unsigned)en_len);
        return 1;
    }
    return 0;
}

static void die(const char *msg) {
    fprintf(stderr, "nxpatch: %s\n", msg);
    exit(1);
}

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint64_t align_up(uint64_t n, uint64_t a) { return (n + a - 1) & ~(a - 1); }

static uint8_t *file_ptr(uint64_t va, uint64_t len) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].type == 1 && va >= ph[i].vaddr && va + len <= ph[i].vaddr + ph[i].filesz)
            return buf + ph[i].off + (va - ph[i].vaddr);
    }
    return NULL;
}

static uint64_t hexval(const char *s) { return strtoull(s, NULL, 0); }

static void unescape(char *s) {
    char *d = s;
    while (*s) {
        if (s[0] == '\\' && s[1] == 'n') { *d++ = '\n'; s += 2; }
        else if (s[0] == '\\' && s[1] == '\\') { *d++ = '\\'; s += 2; }
        else *d++ = *s++;
    }
    *d = 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: nxpatch <in.elf> <out.elf> <strings.tsv> [--relax]\n");
        return 1;
    }
    if (argc >= 5 && strcmp(argv[4], "--relax") == 0) allow_relax = 1;

    FILE *f = fopen(argv[1], "rb");
    if (!f) die("cannot open input");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) die("empty input");
    buf = malloc((size_t)sz + (1 << 20));
    if (!buf) die("out of memory");
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("short read");
    fclose(f);
    buf_len = (size_t)sz;

    if (memcmp(buf, "\x7f" "ELF", 4) != 0 || buf[4] != 2 || buf[5] != 1) die("not ELF64 LE");
    if (rd16(buf + 16) != 2) die("not ET_EXEC");
    if (rd16(buf + 18) != 0xB7) die("not AArch64");

    /* already patched? */
    for (size_t i = 0; i + 8 <= buf_len; i++) {
        if (memcmp(buf + i, MARKER, 7) == 0) {
            fprintf(stderr, "nxpatch: already patched\n");
            return 2;
        }
    }

    uint64_t e_phoff = rd64(buf + 32);
    uint16_t e_phentsize = rd16(buf + 54);
    uint16_t e_phnum = rd16(buf + 56);
    shoff_field = rd64(buf + 40);
    uint16_t e_shentsize = rd16(buf + 58);
    uint16_t e_shnum = rd16(buf + 60);
    uint16_t e_shstrndx = rd16(buf + 62);

    phnum = e_phnum;
    ph = calloc(phnum, sizeof(Phdr));
    for (int i = 0; i < phnum; i++) {
        uint8_t *p = buf + e_phoff + (uint64_t)i * e_phentsize;
        ph[i].type = rd32(p);
        ph[i].flags = rd32(p + 4);
        ph[i].off = rd64(p + 8);
        ph[i].vaddr = rd64(p + 16);
        ph[i].paddr = rd64(p + 24);
        ph[i].filesz = rd64(p + 32);
        ph[i].memsz = rd64(p + 40);
        ph[i].align = rd64(p + 48);
        ph[i].phoff = e_phoff + (uint64_t)i * e_phentsize;
    }

    shnum = e_shnum;
    sh = calloc(shnum, sizeof(Shdr));
    uint8_t *shstr = NULL;
    uint64_t shstr_size = 0;
    if (shnum && e_shstrndx < shnum) {
        uint8_t *p = buf + shoff_field + (uint64_t)e_shstrndx * e_shentsize;
        shstr = buf + rd64(p + 24);
        shstr_size = rd64(p + 32);
    }
    for (int i = 0; i < shnum; i++) {
        uint8_t *p = buf + shoff_field + (uint64_t)i * e_shentsize;
        uint32_t nameoff = rd32(p);
        sh[i].type = rd32(p + 4);
        sh[i].flags = rd64(p + 8);
        sh[i].addr = rd64(p + 16);
        sh[i].off = rd64(p + 24);
        sh[i].size = rd64(p + 32);
        if (shstr && nameoff < shstr_size) {
            snprintf(sh[i].name, sizeof(sh[i].name), "%s", (char *)shstr + nameoff);
        }
    }

    Phdr *rx = NULL, *rw = NULL;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].type != 1) continue;
        if ((ph[i].flags & 1) && !rx) rx = &ph[i];
        if ((ph[i].flags & 2) && !(ph[i].flags & 1) && !rw) rw = &ph[i];
    }
    if (!rx || !rw) die("no RX/RW segments");

    ins_at = rx->off + rx->filesz;
    cave_vaddr = rx->vaddr + rx->filesz;
    uint64_t rw_page = rw->vaddr & ~(uint64_t)0xFFF;
    max_cave = rw_page - align_up(cave_vaddr, 0x1000);
    if (max_cave > 0xF000) max_cave = 0xF000;
    if (max_cave < 0x2000) die("no room for a cave");

    Shdr *text = NULL, *rodata = NULL;
    for (int i = 0; i < shnum; i++) {
        if (!strcmp(sh[i].name, ".text")) text = &sh[i];
        if (!strcmp(sh[i].name, ".rodata")) rodata = &sh[i];
    }
    if (!text || !rodata) die("no .text/.rodata");

    /* ---- mapping ---- */
    FILE *m = fopen(argv[3], "rb");
    if (!m) die("cannot open mapping");
    char line[4096];
    int cap = 512;
    map = calloc(cap, sizeof(Map));
    while (fgets(line, sizeof(line), m)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (mapnum == cap) { cap *= 2; map = realloc(map, cap * sizeof(Map)); }
        map[mapnum].en = strdup(line);
        map[mapnum].ru = strdup(tab + 1);
        unescape(map[mapnum].en);
        unescape(map[mapnum].ru);
        map[mapnum].found = 0;
        mapnum++;
    }
    fclose(m);
    if (!mapnum) die("empty mapping");

    /* ---- cave ---- */
    cave_len = 0;
    memset(cave, 0, MARKER_LEN);
    memcpy(cave, MARKER, strlen(MARKER));
    cave_len = MARKER_LEN;
    for (int i = 0; i < mapnum; i++) {
        while (cave_len % 4) cave[cave_len++] = 0;
        map[i].new_addr = cave_vaddr + cave_len;
        size_t l = strlen(map[i].ru);
        if (cave_len + l + 1 > max_cave) die("cave overflow");
        memcpy(cave + cave_len, map[i].ru, l + 1);
        cave_len += l + 1;
    }

    /* ---- код-помощник для copy-сайтов контекстного меню ---- */
    size_t cave_len_strings = cave_len;
    while (cave_len % 8) cave[cave_len++] = 0;
    helper_va = 0;
    if (cave_len + sizeof(copy_helper) <= max_cave) {
        memcpy(cave + cave_len, copy_helper, sizeof(copy_helper));
        helper_va = cave_vaddr + cave_len;
        cave_len += sizeof(copy_helper);
    }

    /* ---- заглушки зацикливания сетки ---- */
    grid_up_va = grid_down_va = 0;
    grid_up_off = grid_down_off = 0;
    while (cave_len % 8) cave[cave_len++] = 0;
    if (cave_len + sizeof(grid_up_code) + 8 + sizeof(grid_down_code) <= max_cave) {
        grid_up_off = cave_len;
        memcpy(cave + cave_len, grid_up_code, sizeof(grid_up_code));
        cave_len += sizeof(grid_up_code);
        while (cave_len % 8) cave[cave_len++] = 0;
        grid_down_off = cave_len;
        memcpy(cave + cave_len, grid_down_code, sizeof(grid_down_code));
        cave_len += sizeof(grid_down_code);
        grid_up_va = cave_vaddr + grid_up_off;
        grid_down_va = cave_vaddr + grid_down_off;
    }

    /* ---- «Поиск игры в консоли»: пункт меню, обработчик, фильтр ---- */
#ifdef CS_CONSOLE_SEARCH
    cs_built = build_console_search();
#endif


    /* ---- find literals ---- */
    uint8_t *rod = buf + rodata->off;
    uint64_t rosz = rodata->size;
    uint64_t roaddr = rodata->addr;

    /* строка, у которой перед началом не NUL (приклеена к float-константе):
     * ищем такое только по --relax и только если вхождение ровно одно */
    for (int i = 0; i < mapnum; i++) {
        size_t el = strlen(map[i].en);
        int strict = 0, loose = 0;
        for (uint64_t p = 0; p + el + 1 <= rosz; p++) {
            if (rod[p] != (uint8_t)map[i].en[0]) continue;
            if (memcmp(rod + p, map[i].en, el) != 0) continue;
            if (rod[p + el] != 0) continue;
            loose++;
            if (p == 0 || rod[p - 1] == 0) strict++;
        }
        map[i].relax = (allow_relax && strict == 0 && loose == 1);
    }

    for (int i = 0; i < mapnum; i++) {
        size_t el = strlen(map[i].en);
        for (uint64_t p = 0; p + el + 1 <= rosz; p++) {
            if (rod[p] != (uint8_t)map[i].en[0]) continue;
            if (memcmp(rod + p, map[i].en, el) != 0) continue;
            if (rod[p + el] != 0) continue;
            if (p != 0 && rod[p - 1] != 0 && !map[i].relax) continue;
            map[i].found = 1;
            /* patch refs of this copy */
            uint64_t old = roaddr + p;
            int nptr = 0, npair = 0;

            for (int s = 0; s < shnum; s++) {
                if (!(sh[s].flags & 2) || sh[s].type == 8) continue;
                if (sh[s].addr == text->addr) continue;
                if (sh[s].off + sh[s].size > buf_len) continue;
                uint8_t *d = buf + sh[s].off;
                for (uint64_t q = 0; q + 8 <= sh[s].size; q += 8) {
                    if (rd64(d + q) == old) {
                        wr64(d + q, map[i].new_addr);
                        nptr++;
                    }
                }
            }
            (void)nptr;
            (void)npair;
        }
    }

    /* ---- collect ADRP+ADD pairs ---- */
    paircap = 4096;
    pairs = calloc(paircap, sizeof(Pair));
    uint8_t *code = buf + text->off;
    uint64_t code_len = text->size;
    for (uint64_t off = 0; off + 8 <= code_len; off += 4) {
        uint32_t w = rd32(code + off);
        if ((w & 0x9F000000u) != 0x90000000u) continue;
        uint32_t rd = w & 31;
        uint32_t immlo = (w >> 29) & 3;
        uint32_t immhi = (w >> 5) & 0x7FFFF;
        int64_t imm = ((int64_t)((immhi << 2) | immlo) << 43) >> 31; /* sign extend 21 -> bytes*/
        uint64_t pc = text->addr + off;
        uint64_t page = (pc & ~(uint64_t)0xFFF) + (uint64_t)imm;
        uint64_t acc = page;
        int adds = 0, add_off = -1, base_use = -1;
        for (int k = 1; k <= 5; k++) {
            uint64_t o2 = off + (uint64_t)k * 4;
            if (o2 + 4 > code_len) break;
            uint32_t w2 = rd32(code + o2);
            if ((w2 & 0xFFC00000u) == 0x91000000u) {
                uint32_t rn = (w2 >> 5) & 31, rdn = w2 & 31;
                if (rn != rd) {
                    /* unrelated ADD: only a write to the base register matters */
                    if (rdn == rd) break;
                    continue;
                }
                if (rdn == rd) {
                    acc += (w2 >> 10) & 0xFFF;
                    adds++;
                    add_off = (int)o2;
                    continue;
                }
                /* add Xd, Xbase, #imm — one non-chain use of the page register */
                if (adds != 0) break;
                if (o2 + 8 <= code_len) {   /* the base must not be reused right after */
                    uint32_t w3 = rd32(code + o2 + 4);
                    if (((w3 & 0xFFC00000u) == 0x91000000u) && (((w3 >> 5) & 31) == rd)) break;
                }
                base_use = (int)o2;
                add_off = (int)o2;
                acc += (w2 >> 10) & 0xFFF;
                break;
            }
            /* not an ADD: skip it only when it cannot clobber the page register */
            {
                int branch = 0;
                if ((w2 & 0x7C000000u) == 0x14000000u) branch = 1;               /* B / BL */
                else if ((w2 & 0xFF000010u) == 0x54000000u) branch = 1;          /* B.cond */
                else if ((w2 & 0xFFFFFC1Fu) == 0xD63F0000u) branch = 1;          /* BR / BLR */
                else if ((w2 & 0xFFFFFC1Fu) == 0xD65F0000u) branch = 1;          /* RET */
                if (!branch && (w2 & 31) == rd) break;
            }
        }
        if (adds != 1 && base_use < 0) continue;
        if (pairnum == paircap) { paircap *= 2; pairs = realloc(pairs, paircap * sizeof(Pair)); }
        pairs[pairnum].adrp = text->addr + off;
        pairs[pairnum].add = text->addr + add_off;
        pairs[pairnum].target = acc;
        pairs[pairnum].rn = rd;
        pairs[pairnum].rd = (base_use >= 0) ? (rd32(code + base_use) & 31) : rd;
        pairnum++;
    }

    /* ---- patch ADRP+ADD for literal copies ---- */
    int total_pairs = 0;
    int copies = 0;
    int grid_wrapped = 0;
    for (int i = 0; i < mapnum; i++) {
        if (!map[i].found) continue;
        size_t el = strlen(map[i].en);
        for (uint64_t p = 0; p + el + 1 <= rosz; p++) {
            if (rod[p] != (uint8_t)map[i].en[0]) continue;
            if (memcmp(rod + p, map[i].en, el) != 0) continue;
            if (rod[p + el] != 0) continue;
            if (p != 0 && rod[p - 1] != 0 && !map[i].relax) continue;
            uint64_t old = roaddr + p;
            for (int k = 0; k < pairnum; k++) {
                if (pairs[k].target != old) continue;
                uint64_t pa = pairs[k].adrp, aa = pairs[k].add;
                uint32_t w = rd32(file_ptr(pa, 4));
                uint32_t rd = w & 31;
                int64_t delta = (int64_t)((map[i].new_addr >> 12) - (pa >> 12));
                if (delta < -(1 << 20) || delta >= (1 << 20)) continue;
                uint32_t immlo = (uint32_t)(delta & 3);
                uint32_t immhi = (uint32_t)((delta >> 2) & 0x7FFFF);
                uint32_t new_adrp = 0x90000000u | (immlo << 29) | (immhi << 5) | rd;
                uint32_t new_add = 0x91000000u | ((uint32_t)(map[i].new_addr & 0xFFF) << 10) |
                                   (pairs[k].rn << 5) | pairs[k].rd;
                wr32(file_ptr(pa, 4), new_adrp);
                wr32(file_ptr(aa, 4), new_add);
                total_pairs++;
                if (fix_label_copy(aa, (int)pairs[k].rd, el)) copies++;
            }
        }
    }

    /* ---- insert ---- */
    grid_wrapped = fix_grid_wrap(code, code_len, text->addr);
    cs_patched = cs_built ? patch_console_search() : 0;
    if (copies == 0 && !grid_wrapped && !cs_patched) cave_len = cave_len_strings; /* кейв не понадобился */
    insert_size = align_up(cave_len, 0x10000);
    cave_ext = insert_size < (rw_page - cave_vaddr) ? insert_size : (rw_page - cave_vaddr);
    if (cave_vaddr + cave_ext < cave_vaddr + cave_len) die("cave does not fit");
    memmove(buf + ins_at + insert_size, buf + ins_at, buf_len - ins_at);
    memset(buf + ins_at, 0, insert_size);
    memcpy(buf + ins_at, cave, cave_len);
    buf_len += insert_size;

    for (int i = 0; i < phnum; i++) {
        if (&ph[i] == rx) {
            wr64(buf + ph[i].phoff + 32, ph[i].filesz + cave_ext);
            wr64(buf + ph[i].phoff + 40, ph[i].memsz + cave_ext);
        } else if (ph[i].type != 0 && ph[i].off >= ins_at && ph[i].filesz > 0) {
            wr64(buf + ph[i].phoff + 8, ph[i].off + insert_size);
        }
    }
    shoff_field += insert_size;
    wr64(buf + 40, shoff_field);
    for (int i = 0; i < shnum; i++) {
        uint8_t *p = buf + shoff_field + (uint64_t)i * e_shentsize;
        uint64_t so = rd64(p + 24);
        if (so >= ins_at) wr64(p + 24, so + insert_size);
    }

    FILE *o = fopen(argv[2], "wb");
    if (!o) die("cannot write output");
    if (fwrite(buf, 1, buf_len, o) != buf_len) die("short write");
    fclose(o);

    int missing = 0;
    for (int i = 0; i < mapnum; i++) if (!map[i].found) missing++;
    printf("OK: %d strings, %d missing, %d adrp+add, %d label copies, grid wrap %d, console search %d, cave %zu/%llu, size %zu\n",
           mapnum - missing, missing, total_pairs, copies, grid_wrapped, cs_patched, cave_len, (unsigned long long)max_cave, buf_len);
    return 0;
}
