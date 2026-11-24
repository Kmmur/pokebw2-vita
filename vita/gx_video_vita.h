#ifndef POKEBW2_VITA_GX_VIDEO_H
#define POKEBW2_VITA_GX_VIDEO_H

#include "settings_vita.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int x, y, w, h;
} VitaRect;

bool VitaVideo_Init(void);
void VitaVideo_Shutdown(void);
void VitaVideo_Present(void);
void VitaVideo_PresentPaused(void);
void VitaVideo_ApplySettings(void);
void VitaVideo_SetSinglePeek(bool peek);
bool VitaVideo_IsSinglePeek(void);
void VitaVideo_SetTouchCursor(bool visible, uint32_t ds_x, uint32_t ds_y);
VitaRect VitaVideo_GetSubRect(void);
VitaRect VitaVideo_GetTouchRect(void);
bool VitaVideo_IsTouchVisible(void);
void VitaVideo_GetActiveResInfo(int *w, int *h, bool *is_fallback);
void VitaVideo_DrawDiagnostic(uint32_t r, uint32_t g, uint32_t b);
void VitaVideo_RequestScreenshot(void);
void VitaVideo_GetTimings(uint64_t *t3d, uint64_t *t2d_wait, uint64_t *t2d_up, uint64_t *tswap);

#endif
