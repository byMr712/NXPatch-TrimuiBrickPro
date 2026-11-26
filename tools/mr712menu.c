/* mr712menu — меню порта с описанием выбранного пункта.
 * Аргументы: <dir> <confirm_btn> <cancel_btn> <a|b>
 * Крестовина листает с автоповтором и по кругу (как в настройках).
 * При выборе: чёрный экран с сообщением, затем install/remove/выход.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_Joystick SDL_Joystick;

extern int SDL_Init(uint32_t);
extern void SDL_Quit(void);
extern SDL_Window *SDL_CreateWindow(const char *, int, int, int, int, uint32_t);
extern SDL_Surface *SDL_GetWindowSurface(SDL_Window *);
extern void *SDL_RWFromFile(const char *, const char *);
extern SDL_Surface *SDL_LoadBMP_RW(void *, int);
extern int SDL_UpperBlit(SDL_Surface *, const void *, SDL_Surface *, void *);
extern int SDL_UpdateWindowSurface(SDL_Window *);
extern int SDL_PollEvent(void *);
extern int SDL_NumJoysticks(void);
extern SDL_Joystick *SDL_JoystickOpen(int);
extern int SDL_JoystickEventState(int);
extern const char *SDL_GetError(void);
extern void SDL_Delay(uint32_t);
extern uint32_t SDL_GetTicks(void);

#define INIT_VIDEO 0x20u
#define INIT_JOY 0x200u
#define WIN_FULLSCREEN 0x1001u

#define EV_QUIT 0x100u
#define EV_KEYDOWN 0x300u
#define EV_JOYHAT 0x602u
#define EV_JOYBUTTON 0x603u
#define EV_JOYBUTTONUP 0x604u

#define HAT_UP 1
#define HAT_DOWN 4

#define KEY_UP 0x40000052
#define KEY_DOWN 0x40000051
#define KEY_RET 13
#define KEY_ESC 27

#define REPEAT_FIRST 380
#define REPEAT_NEXT 140

typedef struct {
    uint32_t type;
    uint32_t ts;
    int32_t which;
    uint8_t b1, b2, b3, b4;
    uint8_t pad[40];
} Ev;

static int index_ = 0;
static int confirm_btn = 0, cancel_btn = 1;
static const char *suffix = "";
static const char *g_dir = ".";
static SDL_Surface *img[3];
static SDL_Surface *msg[3];
static SDL_Window *win;

static int held = 0;            /* -1 вверх, +1 вниз */
static uint32_t next_repeat = 0;

static void show(SDL_Surface *s) {
    SDL_UpperBlit(s, NULL, SDL_GetWindowSurface(win), NULL);
    SDL_UpdateWindowSurface(win);
}

static void move(int dir) {
    index_ = (index_ + (dir < 0 ? 2 : 1)) % 3;
    show(img[index_]);
    next_repeat = SDL_GetTicks() + REPEAT_NEXT;
}

static void quit_now(void) {
    SDL_Quit();
    _exit(0);
}

static void run_action(int idx) {
    show(msg[idx]);
    if (idx == 2) {
        SDL_Delay(600);
        quit_now();
    }
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "%s/%s", g_dir, idx == 0 ? "install.sh" : "remove.sh");
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", cmd, (char *)NULL);
        _exit(127);
    }
    for (;;) {
        SDL_Delay(100);
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid)
            break;
    }
    SDL_Delay(2000);
    quit_now();
}

int main(int argc, char **argv) {
    g_dir = argc > 1 ? argv[1] : ".";
    char path[600];

    if (argc > 2) confirm_btn = atoi(argv[2]);
    if (argc > 3) cancel_btn = atoi(argv[3]);
    if (argc > 4 && argv[4][0] == 'b') suffix = "b";

    if (SDL_Init(INIT_VIDEO | INIT_JOY) != 0) {
        fprintf(stderr, "menu: SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    win = SDL_CreateWindow("Mr712", 0, 0, 1024, 768, WIN_FULLSCREEN);
    if (!win) {
        fprintf(stderr, "menu: window: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/menu%d%s.bmp", g_dir, i, suffix);
        void *rw = SDL_RWFromFile(path, "rb");
        if (rw) img[i] = SDL_LoadBMP_RW(rw, 1);
        snprintf(path, sizeof(path), "%s/msg%d.bmp", g_dir, i);
        rw = SDL_RWFromFile(path, "rb");
        if (rw) msg[i] = SDL_LoadBMP_RW(rw, 1);
        if (!img[i] || !msg[i]) {
            fprintf(stderr, "menu: no image %s\n", path);
            SDL_Quit();
            return 2;
        }
    }

    if (SDL_NumJoysticks() > 0) {
        SDL_JoystickOpen(0);
        SDL_JoystickEventState(1);
    }
    fprintf(stderr, "menu: joysticks=%d confirm=%d cancel=%d hint=%s\n",
            SDL_NumJoysticks(), confirm_btn, cancel_btn, suffix[0] ? suffix : "a");

    show(img[0]);

    Ev ev;
    while (1) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == EV_QUIT)
                quit_now();

            if (ev.type == EV_JOYBUTTON) {
                int btn = ev.b1;
                if (btn == confirm_btn) run_action(index_);
                if (btn == cancel_btn) quit_now();
                if (btn == 11) { held = -1; move(-1); next_repeat = SDL_GetTicks() + REPEAT_FIRST; }
                if (btn == 12) { held = 1; move(1); next_repeat = SDL_GetTicks() + REPEAT_FIRST; }
            } else if (ev.type == EV_JOYBUTTONUP) {
                int btn = ev.b1;
                if ((btn == 11 && held < 0) || (btn == 12 && held > 0))
                    held = 0;
            } else if (ev.type == EV_JOYHAT) {
                int v = ev.b2;
                if (v == HAT_UP) {
                    held = -1;
                    move(-1);
                    next_repeat = SDL_GetTicks() + REPEAT_FIRST;
                } else if (v == HAT_DOWN) {
                    held = 1;
                    move(1);
                    next_repeat = SDL_GetTicks() + REPEAT_FIRST;
                } else if (v == 0) {
                    held = 0;
                }
            } else if (ev.type == EV_KEYDOWN) {
                int sym = 0;
                memcpy(&sym, &ev.pad[4], 4);
                if (sym == KEY_UP) { held = -1; move(-1); next_repeat = SDL_GetTicks() + REPEAT_FIRST; }
                else if (sym == KEY_DOWN) { held = 1; move(1); next_repeat = SDL_GetTicks() + REPEAT_FIRST; }
                else if (sym == KEY_RET) run_action(index_);
                else if (sym == KEY_ESC) quit_now();
            }
        }

        if (held && SDL_GetTicks() >= next_repeat)
            move(held);

        SDL_Delay(15);
    }
}
