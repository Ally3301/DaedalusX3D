#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#include "theme.h"
}
#else
#include "PSX3DTheme.h"
#endif
#include "PSX3DIcons.h"
#ifdef __cplusplus
extern "C" {
#endif
bool PSX3D_FrontendIsInGame(void);
void PSX3D_FrontendGpuIdle(void);
void PSX3D_FrontendLog(const char *fmt, ...);
#ifdef __cplusplus
}
#endif
#define config_in_game() PSX3D_FrontendIsInGame()
#define video_gpu_idle() PSX3D_FrontendGpuIdle()
#define dev_enabled() false
#define log_printf(...) PSX3D_FrontendLog(__VA_ARGS__)
#define ui_depth(d) (0.0f)
enum { UI_ICON_NONE, UI_ICON_STAR, UI_ICON_SORT, UI_ICON_RESCAN, UI_ICON_LEFT, UI_ICON_RIGHT, UI_ICON_FOLDER,
    UI_ICON_BACK, UI_ICON_PLAY, UI_ICON_CHECK, UI_ICON_CROSS, UI_ICON_PLUS, UI_ICON_TRASH, UI_ICON_SAVE,
    UI_ICON_LOAD, UI_ICON_RESET, UI_ICON_UP, UI_ICON_OPEN, UI_ICON_CHANGE, UI_ICON_TOGGLE, UI_ICON_DISC, UI_ICON_EXIT };
