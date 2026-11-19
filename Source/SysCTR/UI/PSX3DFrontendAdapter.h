#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void PSX3D_FrontendInitialize(void);
void PSX3D_FrontendLogMemory(const char *stage);
void PSX3D_FrontendShutdown(void);
void PSX3D_FrontendSetTheme(int index);
void PSX3D_FrontendGameStarted(const char *path);
void PSX3D_FrontendGameEnded(void);
int PSX3D_FrontendRunHome(char *path, size_t len);
int PSX3D_FrontendRunLibrary(char *path, size_t len);
#ifdef __cplusplus
}
#endif
