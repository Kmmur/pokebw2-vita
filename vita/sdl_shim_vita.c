#include <psp2/kernel/processmgr.h>
#include "SDL3/SDL.h"
#include "log_vita.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

static char last_error[256];

SDL_IOStream *SDL_IOFromFile(const char *file, const char *mode) {
    FILE *fp = fopen(file, mode);
    if (!fp) {
        SDL_SetError("Cannot open %s", file);
        return NULL;
    }
    SDL_IOStream *s = (SDL_IOStream *)calloc(1, sizeof(*s));
    if (!s) { fclose(fp); return NULL; }
    s->fp = fp;
    return s;
}

SDL_IOStream *SDL_IOFromConstMem(const void *mem, size_t size) {
    SDL_IOStream *s = (SDL_IOStream *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->mem = (const uint8_t *)mem;
    s->mem_size = size;
    return s;
}

bool SDL_CloseIO(SDL_IOStream *s) {
    if (!s) return true;
    if (s->fp) fclose(s->fp);
    free(s);
    return true;
}

size_t SDL_ReadIO(SDL_IOStream *s, void *ptr, size_t size) {
    if (!s) return 0;
    if (s->fp) {
        size_t r = fread(ptr, 1, size, s->fp);
        if (r < size) s->status = feof(s->fp) ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_ERROR;
        else s->status = SDL_IO_STATUS_READY;
        return r;
    }
    size_t avail = s->mem_pos < s->mem_size ? s->mem_size - s->mem_pos : 0;
    size_t r = size < avail ? size : avail;
    memcpy(ptr, s->mem + s->mem_pos, r);
    s->mem_pos += r;
    s->status = (r < size) ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_READY;
    return r;
}

SDL_IOStatus SDL_GetIOStatus(SDL_IOStream *s) {
    return s ? s->status : SDL_IO_STATUS_ERROR;
}

int64_t SDL_SeekIO(SDL_IOStream *s, int64_t offset, SDL_IOWhence whence) {
    if (!s) return -1;
    if (s->fp) {
        int w = whence == SDL_IO_SEEK_SET ? SEEK_SET : whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END;
        if (fseek(s->fp, (long)offset, w) != 0) return -1;
        return (int64_t)ftell(s->fp);
    }
    int64_t base = whence == SDL_IO_SEEK_SET ? 0 : whence == SDL_IO_SEEK_CUR ? (int64_t)s->mem_pos : (int64_t)s->mem_size;
    int64_t pos = base + offset;
    if (pos < 0 || (size_t)pos > s->mem_size) return -1;
    s->mem_pos = (size_t)pos;
    return pos;
}

int64_t SDL_TellIO(SDL_IOStream *s) {
    if (!s) return -1;
    return s->fp ? (int64_t)ftell(s->fp) : (int64_t)s->mem_pos;
}

#include <psp2/io/fcntl.h>
void *SDL_LoadFile(const char *file, size_t *datasize) {
    SceUID fd = sceIoOpen(file, SCE_O_RDONLY, 0);
    if (fd < 0) { SDL_SetError("Cannot open %s", file); return NULL; }
    SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz < 0) { sceIoClose(fd); return NULL; }
    void *buf = malloc((size_t)sz + 1);
    if (!buf) { sceIoClose(fd); return NULL; }
    size_t total = 0;
    while (total < (size_t)sz) {
        size_t chunk = (size_t)sz - total;
        if (chunk > 4 * 1024 * 1024) chunk = 4 * 1024 * 1024;
        int r = sceIoRead(fd, (char *)buf + total, (SceSize)chunk);
        if (r <= 0) break;
        total += (size_t)r;
    }
    sceIoClose(fd);
    ((char *)buf)[total] = '\0';
    if (datasize) *datasize = total;
    return buf;
}

bool SDL_GetCurrentTime(SDL_Time *ticks) {
    SceDateTime dt;
    if (sceRtcGetCurrentClockLocalTime(&dt) < 0) {
        *ticks = 0;
        return false;
    }
    *ticks = (SDL_Time)dt.second * 1000000000LL;
    return true;
}

bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *out, bool localTime) {
    (void)ticks; (void)localTime;
    SceDateTime dt;
    if (sceRtcGetCurrentClockLocalTime(&dt) < 0) return false;
    out->year = dt.year;
    out->month = dt.month;
    out->day = dt.day;
    out->hour = dt.hour;
    out->minute = dt.minute;
    out->second = dt.second;
    out->nanosecond = dt.microsecond * 1000;
    out->day_of_week = sceRtcGetDayOfWeek(dt.year, dt.month, dt.day);
    out->utc_offset = 0;
    return true;
}

uint64_t SDL_GetPerformanceCounter(void) {
    return sceKernelGetProcessTimeWide();
}

uint32_t SDL_rand_bits_r(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

bool SDL_SetError(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, ap);
    va_end(ap);
    return false;
}

const char *SDL_GetError(void) {
    return last_error;
}

void SDL_Log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VitaLog_Printf("%s", buf);
}

void SDL_LogError(int category, const char *fmt, ...) {
    (void)category;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    VitaLog_Printf("[CPU-bound] %s", buf);
}

void SDL_Delay(uint32_t ms) {
    sceKernelDelayThread(ms * 1000);
}

size_t SDL_WriteIO(SDL_IOStream *s, const void *ptr, size_t size) {
    if (!s || !s->fp) return 0;
    return fwrite(ptr, 1, size, s->fp);
}

bool SDL_FlushIO(SDL_IOStream *s) {
    if (!s || !s->fp) return true;
    return fflush(s->fp) == 0;
}

int64_t SDL_GetIOSize(SDL_IOStream *s) {
    if (!s) return -1;
    if (!s->fp) return (int64_t)s->mem_size;
    long cur = ftell(s->fp);
    fseek(s->fp, 0, SEEK_END);
    long sz = ftell(s->fp);
    fseek(s->fp, cur, SEEK_SET);
    return (int64_t)sz;
}

bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (info) {
        info->type = S_ISDIR(st.st_mode) ? SDL_PATHTYPE_DIRECTORY : SDL_PATHTYPE_FILE;
        info->size = (uint64_t)st.st_size;
    }
    return true;
}
