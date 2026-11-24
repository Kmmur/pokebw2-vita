#ifndef POKEBW2_VITA_OVERLAY_H
#define POKEBW2_VITA_OVERLAY_H

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <stdbool.h>
#include <stdint.h>

void VitaOverlay_Init(void);
void VitaOverlay_Shutdown(void);
bool VitaOverlay_IsOpen(void);
void VitaOverlay_Open(void);
void VitaOverlay_Close(void);
void VitaOverlay_SetLiveStats(float fps, uint32_t free_ram_kb, uint32_t free_cdram_kb);

// Returns true while overlay consumes input (gameplay should remain paused).
bool VitaOverlay_Update(const SceCtrlData *pad, const SceTouchData *touch);

// Renders the native Vita-style overlay menu over the current vitaGL frame.
void VitaOverlay_Draw(void);

// Returns true if the overlay's button combo is currently pressed.
bool VitaOverlay_CheckCombo(uint32_t buttons);

#endif
