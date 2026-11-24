#include "log_vita.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *LOG_PATH = "ux0:data/pokebw2/log.txt";

void VitaLog_Init(void) {
    sceIoMkdir("ux0:data/pokebw2", 0777);
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
}

void VitaLog_Printf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 2;
    if (buf[len - 1] != '\n') {
        buf[len++] = '\n';
        buf[len] = '\0';
    }
    sceClibPrintf("%s", buf);
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)len);
        sceIoClose(fd);
    }
}

void VitaLog_Shutdown(void) {}
