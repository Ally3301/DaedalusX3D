#include "stdafx.h"
extern "C" {
#include "psx3d.h"
#include "devtools.h"
#include "presetupdate.h"
#include "scene.h"
}
#include "UserInterface.h"
#include "FrontendDraw.h"
#include "Core/ROM.h"
#include "Core/RomSettings.h"
#include "Utility/IO.h"
#include "Utility/ROMFile.h"
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <malloc.h>
#include <3ds/allocator/linear.h>
#include <3ds/env.h>

extern bool shouldQuit;
extern "C" {
void PSX3D_FrontendLogMemory(const char *stage) {
    // SD writes from inside the menu draw loop kill the process on the
    // Old 3DS: every crash log on the SD card ends exactly at the first
    // 5-second "frontend-5s" entry (SD stats write from icons' spent()),
    // and older builds crashed the same way. Startup markers (written
    // before the GPU loop starts) are safe and kept.
    static const char *const kAllowed[] = {
        "after-ui-initialize", "after-system-init",
        "frontend-before", "frontend-after-ui", "frontend-after-library-ui",
        "frontend-after-rom-scan", "icons-loaded"
    };
    bool allowed = false;
    for (unsigned i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); i++)
        if (stage && !strcmp(stage, kAllowed[i])) { allowed = true; break; }
    if (!allowed)
        return;

    const struct mallinfo mi = mallinfo();
    const u32 linearFree = linearSpaceFree();
    const u32 heapCapacity = envGetHeapSize();
    const u32 linearCapacity = envGetLinearHeapSize();
    const u32 linearUsed = linearCapacity >= linearFree ? linearCapacity - linearFree : 0;
    FILE *f = fopen("sdmc:/DaedalusX64.log", "a");
    if (f) {
        fprintf(f, "memory[%s]: heap-capacity=%lu KiB heap-arena=%lu KiB heap-used=%lu KiB heap-free=%lu KiB linear-capacity=%lu KiB linear-used=%lu KiB linear-free=%lu KiB\n",
            stage ? stage : "?", (unsigned long)(heapCapacity / 1024),
            (unsigned long)(mi.arena / 1024), (unsigned long)(mi.uordblks / 1024),
            (unsigned long)(mi.fordblks / 1024), (unsigned long)(linearCapacity / 1024),
            (unsigned long)(linearUsed / 1024), (unsigned long)(linearFree / 1024));
        fclose(f);
    }
}
psx3d_config g_cfg = {};
int ui_eye = 0;
float ui_parallax = 0.0f;
bool ui_wants_3d = false;
bool covers_slow = false;
unsigned covers_sd_ms = 0;
bool g_new3ds = false;
}

namespace {
// Fixed-storage library: the Old 3DS app heap is nearly full once citro3d,
// the dynarec tables and the texture cache are up (the SD log showed ~200 KiB
// free at this point), and every std::string/deque node and heap-allocated
// sort buffer makes that worse and adds fragmentation. All game metadata now
// lives in one static arena; nothing here touches the heap.
struct GameEntry {
    game_t game{};
    char title[64];
    char filename[64];
    char path[300];
};
struct PlayEntry {
    char key[64];
    u32 last = 0, seconds = 0;
    bool favorite = false;
};
const int kMaxGames = 256;
const int kMaxPlays = 128;
GameEntry s_games[kMaxGames];
int s_gameCount = 0;
PlayEntry s_playlog[kMaxPlays];
int s_playCount = 0;
char s_playing[64];
u32 s_playStart = 0;
const char *kPlaylogPath = "sdmc:/3ds/DaedalusX64/frontend-playlog.txt";
const char *kConfigPath = "sdmc:/3ds/DaedalusX64/frontend.ini";
PlayEntry *FindPlay(const char *key, bool create) {
    if (!key || !key[0]) return nullptr;
    for (int i = 0; i < s_playCount; i++)
        if (!strcmp(s_playlog[i].key, key)) return &s_playlog[i];
    if (!create || s_playCount >= kMaxPlays) return nullptr;
    PlayEntry &p = s_playlog[s_playCount++];
    memset(&p, 0, sizeof(p));
    snprintf(p.key, sizeof(p.key), "%s", key);
    return &p;
}
void SavePlaylog() {
    FILE *f = fopen(kPlaylogPath, "w"); if (!f) return;
    for (int i = 0; i < s_playCount; i++)
        fprintf(f, "%s\t%lu\t%lu\t%d\n", s_playlog[i].key,
            (unsigned long)s_playlog[i].last, (unsigned long)s_playlog[i].seconds,
            s_playlog[i].favorite ? 1 : 0);
    fclose(f);
}
}

extern "C" {
void config_load(void) {
    snprintf(g_cfg.games_dir, sizeof(g_cfg.games_dir), "%s", "sdmc:/3ds/DaedalusX64/Roms/");
    FILE *f = fopen(kConfigPath, "r");
    if (f) {
        char line[768];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "view=", 5)) g_cfg.library_view = atoi(line + 5);
            else if (!strncmp(line, "last=", 5)) snprintf(g_cfg.last_title, sizeof(g_cfg.last_title), "%.*s", (int)sizeof(g_cfg.last_title) - 1, line + 5);
            else if (!strncmp(line, "theme=", 6)) g_cfg.theme = atoi(line + 6);
        }
        fclose(f);
    }
    g_cfg.last_title[strcspn(g_cfg.last_title, "\r\n")] = 0;
    if (g_cfg.library_view < 0 || g_cfg.library_view > 3) g_cfg.library_view = 0;
    s_playCount = 0;
    FILE *history = fopen(kPlaylogPath, "r");
    if (history) {
        char row[640];
        while (fgets(row, sizeof(row), history)) {
            char *key = strtok(row, "\t\r\n");
            char *last = strtok(nullptr, "\t\r\n");
            char *seconds = strtok(nullptr, "\t\r\n");
            char *favorite = strtok(nullptr, "\t\r\n");
            if (!key || !*key) continue;
            if (s_playCount >= kMaxPlays) break;
            PlayEntry &entry = s_playlog[s_playCount++];
            memset(&entry, 0, sizeof(entry));
            snprintf(entry.key, sizeof(entry.key), "%s", key);
            entry.last = last ? (u32)strtoul(last, nullptr, 10) : 0;
            entry.seconds = seconds ? (u32)strtoul(seconds, nullptr, 10) : 0;
            entry.favorite = favorite && atoi(favorite) != 0;
        }
        fclose(history);
    }
}
void config_save(void) {
    FILE *f = fopen(kConfigPath, "w"); if (!f) return;
    fprintf(f, "view=%d\nlast=%s\ntheme=%d\n", g_cfg.library_view, g_cfg.last_title, g_cfg.theme); fclose(f);
}
void PSX3D_FrontendSetTheme(int index) { g_cfg.theme = index; config_save(); }
void PSX3D_FrontendInitialize(void) {
    PSX3D_FrontendLogMemory("frontend-before");
    config_load();
    ui_init();
    PSX3D_FrontendLogMemory("frontend-after-ui");
    libui_init();
    PSX3D_FrontendLogMemory("frontend-after-library-ui");
    library_load(g_cfg.games_dir, false);
    PSX3D_FrontendLogMemory("frontend-after-rom-scan");
    if (g_cfg.last_title[0])
        libui_select_title(g_cfg.last_title);
}
void PSX3D_FrontendShutdown(void) {
    libui_exit();
    ui_exit();
}
int PSX3D_FrontendRunHome(char *path, size_t len) {
    if (!path || !len) return HOME_QUIT;
    path[0] = 0;
    home_launch_t launch = {};
    const int result = home_run(library_count() > 0, path, len, &launch);
    // This Daedalus integration does not have PSX3D's Continue-state format
    // (no home_note_game caller, no states directory) or folder-browser
    // backend. Keep those actions inside the game library. HOME_AGAIN (the
    // devtools remote restart) has no backend either: treat it as quit so the
    // selector loop cannot spin on a state the adapter never produces.
    if (result == HOME_PLAY || result == HOME_FOLDERS)
        return HOME_LIBRARY;
    if (result == HOME_AGAIN)
        return HOME_QUIT;
    return result;
}
int PSX3D_FrontendRunLibrary(char *path, size_t len) {
    if (!path || !len) return 0;
    path[0] = 0;
    return libui_run(path, len);
}
bool library_load(const char *root, bool rescan) {
    (void)rescan; s_gameCount = 0;
    if (!root || !root[0]) return false;
    IO::FindHandleT handle; IO::FindDataT data;
    if (!IO::FindFileOpen(root, &handle, data)) return true;
    do {
        if (!IsRomfilename(data.Name)) continue;
        if (s_gameCount >= kMaxGames) break;
        GameEntry &e = s_games[s_gameCount];
        memset(&e, 0, sizeof(e));
        snprintf(e.filename, sizeof(e.filename), "%s", data.Name);
        snprintf(e.path, sizeof(e.path), "%s%s%s", root,
            root[strlen(root) - 1] == '/' ? "" : "/", e.filename);
        char name[64];
        {
            std::string rom_name;
            if (!ROM_GetRomName(e.path, rom_name) || rom_name.empty())
                snprintf(name, sizeof(name), "%s", e.filename);
            else
                snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, rom_name.c_str());
        }
        snprintf(e.title, sizeof(e.title), "%s", name);
        s_gameCount++;
    } while (IO::FindFileNext(handle, data));
    IO::FindFileClose(handle);
    // Insertion sort in place: no heap, no allocator churn, and the entries
    // never move after this, so the game_t pointers wired below stay valid.
    for (int a = 1; a < s_gameCount; a++) {
        GameEntry tmp = s_games[a];
        int b = a;
        while (b > 0 && strcasecmp(s_games[b - 1].title, tmp.title) < 0 ? false : false) break;
        while (b > 0 && strcasecmp(s_games[b - 1].title, tmp.title) > 0) {
            s_games[b] = s_games[b - 1];
            b--;
        }
        s_games[b] = tmp;
    }
    for (int i = 0; i < s_gameCount; i++) {
        GameEntry &stored = s_games[i];
        stored.game.title = stored.title;
        stored.game.cover_key = stored.filename;
        stored.game.group = nullptr; stored.game.ndiscs = 1;
        stored.game.discs[0] = stored.filename;
        stored.game.disc_no[0] = 1;
    }
    return true;
}
bool library_guess_dir(void) { return library_load(g_cfg.games_dir, false); }
int library_count(void) { return s_gameCount; }
const game_t *library_get(int i) {
    if (i < 0 || i >= s_gameCount) return nullptr;
    return &s_games[i].game;
}
int library_series_count(void) { return 0; }
int library_game_series(int) { return -1; }
const char *library_series_name(int) { return ""; }
int library_series_size(int) { return 0; }
int library_series_game(int, int) { return -1; }
int library_omitted(void) { return 0; }
const char *library_launch_path(const game_t *g, char *out, size_t len) {
    if (!g || !out || !len) return nullptr;
    // Entries are fixed storage: identity match is stable. Fall back to the
    // file name in case a caller kept a pointer across a rescan.
    for (int i = 0; i < s_gameCount; i++) {
        GameEntry &e = s_games[i];
        if (&e.game == g || (g->cover_key && !strcmp(e.filename, g->cover_key))) {
            if (strlen(e.path) >= len) return nullptr;
            snprintf(out, len, "%s", e.path); return out;
        }
    }
    return nullptr;
}
bool library_is_ps1_image(const char *) { return false; }
bool library_missing_file(const char *, char *, size_t) { return false; }
void states_when_text(char *out, size_t len, u32 when) {
    if (!out || !len)
        return;
    u32 days = (u32)time(nullptr) / 86400 - when / 86400;
    snprintf(out, len, "%s", days == 0 ? "today" : days == 1 ? "yesterday" : "earlier");
}
u32 playlog_last(const char *key) { PlayEntry *p = FindPlay(key, false); return p ? p->last : 0; }
u32 playlog_seconds(const char *key) { PlayEntry *p = FindPlay(key, false); return p ? p->seconds : 0; }
bool playlog_favourite(const char *key) { PlayEntry *p = FindPlay(key, false); return p && p->favorite; }
bool playlog_toggle_favourite(const char *key) { PlayEntry *p = FindPlay(key, true); if (!p) return false; p->favorite = !p->favorite; SavePlaylog(); return p->favorite; }
void playlog_begin(const char *key) { snprintf(s_playing, sizeof(s_playing), "%s", key ? key : ""); s_playStart = (u32)time(nullptr); }
void playlog_end(void) { if (s_playing[0]) { PlayEntry *p = FindPlay(s_playing, true); if (p) { p->last = (u32)time(nullptr); if (p->last >= s_playStart) p->seconds += p->last - s_playStart; SavePlaylog(); } } s_playing[0] = 0; }
void playlog_pause(bool) {}
void PSX3D_FrontendGameStarted(const char *path) {
    if (!path) return;
    for (int i = 0; i < s_gameCount; i++)
        if (!strcmp(s_games[i].path, path)) { playlog_begin(s_games[i].filename); return; }
    playlog_begin(path);
}
void PSX3D_FrontendGameEnded(void) { playlog_end(); }
void covers_init(void) {}
void covers_exit(void) {}
C3D_Tex *covers_get(int, Tex3DS_SubTexture *) { return nullptr; }
u32 covers_frame(void) { return 0; }
void covers_reset(void) {}
void covers_focus(int) {}
void covers_pump(void) {}
void covers_stats_reset(void) {}
int covers_test_tick(void) { return 0; }
void covers_note_select(void) {}
void covers_note_draw(bool, bool) {}
bool covers_busy(void) { return false; }
bool covers_stop(void) { return true; }
void covers_stop_wait(void) {}
void covers_resume(void) {}
void covers_release_gpu(void) {}
void cover_placeholder(float cx, float cy, float size, float dim, float z, const char *title, u32 accent) {
    const theme_t *t = theme(); float x = cx - size * .5f, y = cy - size * .5f;
    u32 top = theme_alpha(t->card_top, (int)(255 * (1.0f - dim)));
    u32 bot = theme_alpha(t->card_bottom, (int)(255 * (1.0f - dim)));
    C2D_DrawRectangle(x, y, z, size, size, top, top, bot, bot);
    C2D_DrawRectSolid(x, y, z + .001f, 3, size, theme_alpha(accent, (int)(255 * (1.0f - dim))));
    ui_text_ex(NULL, cx, cy - 12, .8f, theme_alpha(t->card_ink, (int)(255 * (1.0f - dim))), C2D_AlignCenter, title ? title : "N64");
}
void music_play(bool) {}
void music_update(void) {}
void music_unload(void) {}
void sfx_play(int) {}
static void AboutTop(void *) {
    libui_draw_backdrop();
    C2D_DrawRectSolid(0, 0, 0.3f, 400, 240, theme_alpha(theme()->menu_bg, 70));
    ui_menu_header("About DaedalusX64", "Nintendo 3DS");
    ui_text_ex(libui_font(), 200, 86, 0.9f, theme()->game_title, C2D_AlignCenter, "DaedalusX64");
    ui_text_ex(NULL, 200, 122, 0.48f, theme()->game_sub, C2D_AlignCenter, "Nintendo 64 emulator");
    ui_text_ex(NULL, 200, 150, 0.42f, theme()->muted, C2D_AlignCenter, "Homebrew frontend using Citro2D / Citro3D");
}
static void AboutBottom(void *) {
    scene_draw(true);
    ui_menu_header("Controls", "Frontend");
    ui_text_wrap(14, 48, 0.48f, theme()->white, 292, "Library: A opens a game, X changes sorting, Y favourites. In game: press SELECT to pause. Use the D-pad or touch screen to navigate.");
    static const ui_button_t back = { KEY_B, "Back", UI_ICON_BACK, false };
    ui_bar_draw(&back, 1);
}
void log_printf(const char *, ...) {}
bool dev_tick(bool) { return false; }
int dev_ui_take(void) { return DEV_UI_NONE; }
int dev_ui_pending(void) { return 0; }
bool dev_transition(void) { return false; }
bool settings_ui_run(bool, void (*)(void *)) { return false; }
bool settings_ui_take_folders(void) { return false; }
bool settings_ui_active(void) { return false; }
bool config_in_game(void) { return false; }
const char *emu_game_title(void) { return "DaedalusX64"; }
game_preset_options_t game_capabilities(void) { game_preset_options_t p = {}; return p; }
bool presets_update_poll(void) { return false; }
const char *slow_mode_notice(bool) { return nullptr; }
void input_poll(void) { hidScanInput(); }
u32 input_keys_down(void) { return hidKeysDown(); }
u32 input_keys_held(void) { return hidKeysHeld(); }
int input_control_slot(int) { return -1; }
const char *input_control_value(int, void *) { return ""; }
u32 input_control_color(int) { return theme()->muted; }
int input_map_get(int) { return -1; }
const char *input_ps1_name(int id) { static const char *names[] = { "Cross", "Circle", "Square", "Triangle", "L1", "R1" }; return id >= 0 && id < 6 ? names[id] : ""; }
void video_present_vsync(bool, void (*top)(void *), void (*bottom)(void *), void *ud) {
    gspWaitForVBlank();
    if (!UI::Frontend::BeginFrame()) return;
    UI::Frontend::BeginTop(); if (top) top(ud);
    UI::Frontend::BeginBottom(); if (bottom) bottom(ud);
    UI::Frontend::EndFrame();
}
void scene_draw(bool bottom) {
    const theme_t *t = theme();
    if (bottom) {
        C2D_DrawRectSolid(0, 0, .95f, 320, 240, t->menu_bg);
        return;
    }
    C2D_DrawRectangle(0, 0, .95f, 400, 240, t->original_sky[0], t->original_sky[1], t->original_sky[2], t->original_sky[3]);
    C2D_DrawCircleSolid(200, 92, .92f, 42, theme_alpha(t->sun_hot, 220));
    for (int y = 130; y < 240; y += 12) {
        float p = (float)(y - 130) / 110.0f;
        C2D_DrawLine(0, (float)y, theme_alpha(t->original_grid[(y / 12) % 3], (int)(90 + p * 100)), 400, (float)y, theme_alpha(t->original_grid[(y / 12) % 3], (int)(90 + p * 100)), 1.0f, .94f);
    }
    for (int x = -200; x <= 600; x += 40)
        C2D_DrawLine(200, 130, theme_alpha(t->original_grid[1], 150), (float)x, 240, theme_alpha(t->original_grid[1], 150), 1.0f, .94f);
}
void scene_release(void) {}
bool states_auto_peek(const char *, u32 *, C3D_Tex *, bool *) { return false; }
void states_thumb_draw(C3D_Tex *, float, float, float, float, float) {}
bool mp_launch_request(char *, size_t) { return false; }
void share_run(void) {
    static const ui_button_t back = { KEY_B, "Back", UI_ICON_BACK, false };
    while (aptMainLoop()) {
        input_poll();
        const u32 keys = ui_header_back(ui_bar_input(&back, 1, input_keys_down()));
        if (keys & (KEY_B | KEY_A | KEY_START))
            break;
        ui_begin();
        video_present_vsync(false, AboutTop, AboutBottom, nullptr);
    }
}
bool share_about_visible(void) { return true; }
}
