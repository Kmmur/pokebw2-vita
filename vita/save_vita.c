#include "save.h"
#include "dwc/bm.h"
#include "dwc/init.h"
#include "gfl/backup_system.h"
#include "nitro/host_card.h"
#include "save/join_avenue.h"
#include "save/save_control.h"
#include "system/text_speed.h"
#include <psp2/io/stat.h>
#include <string.h>

static bool initialized;

u32 DWCi_GetGameCode(void) {
    return 0x4952414a;
}

void DWC_Auth_GetId(DWCAuthWiFiId *id) {
    memset(id, 0, sizeof(*id));
}

bool PcSave_Init(void) {
    if (initialized)
        return true;
    sceIoMkdir("ux0:data/pokebw2", 0777);
    if (!PcCard_OpenBackup("ux0:data/pokebw2/black2.sav"))
        return false;
    mainBackupSystemInit(HEAPID_USER, NULL, NULL);
    saveMainSaveAreaAddress(SaveControl_Create(HEAPID_USER));
    setImportantJABlockAddresses(SaveControl_GetInstance());
    initialized = true;
    return true;
}

void PcSave_Shutdown(void) {
    if (initialized)
        SaveControl_Free();
    PcCard_CloseBackup();
    initialized = false;
}
