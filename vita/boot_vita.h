#ifndef POKEBW2_VITA_BOOT_H
#define POKEBW2_VITA_BOOT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { VITA_BOOT_MOVIES, VITA_BOOT_TITLE, VITA_BOOT_MENU, VITA_BOOT_NEW_GAME } VitaBootEntry;

bool VitaBoot_Start(const char *root, VitaBootEntry entry);
void VitaBoot_Update(void);
void VitaBoot_UpdateStep(bool tick_audio);
void VitaBoot_Draw(void);
void VitaBoot_Stop(void);
void VitaBoot_SetInput(uint16_t keys, bool touch_held, bool touch_pressed, uint32_t touch_x, uint32_t touch_y);
bool VitaBoot_IsFieldScene(void);
bool VitaBoot_IsBattleScene(void);
bool VitaBoot_IsMenuScene(void);
bool VitaBoot_IsTextEntryScene(void);
bool VitaBoot_IsTitleScene(void);
bool VitaBoot_IsIntroScene(void);
bool VitaBoot_IsStarterSelectScene(void);
bool VitaBoot_IsTouchPrimaryScene(void);
bool VitaBoot_IsFieldMenuOpen(void);
bool VitaBoot_IsWide3DActive(void);
bool VitaBoot_Reused3DFrame(void);

#endif
