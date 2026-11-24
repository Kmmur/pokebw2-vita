#ifndef POKEBW2_VITA_IME_H
#define POKEBW2_VITA_IME_H

#include <stdbool.h>
#include <stdint.h>

void VitaIme_Init(void);
void VitaIme_Shutdown(void);
void VitaIme_ResetSession(void);
int VitaIme_WasTriggeredThisSession(void);
int VitaIme_IsActive(void);
int VitaIme_Open(const char *title_ascii, const uint16_t *initial_utf16, uint32_t max_len);
void VitaIme_Update(void);
int VitaIme_ConsumeResult(uint16_t *out_utf16, uint32_t max_chars, int *confirmed, int *canceled);

#endif
