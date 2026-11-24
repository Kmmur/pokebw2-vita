#ifndef POKEBW2_VITA_SDL_SHIM_H
#define POKEBW2_VITA_SDL_SHIM_H

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int64_t Sint64;
typedef int64_t SDL_Time;

typedef struct { float x, y, w, h; } SDL_FRect;
typedef struct { int format, channels, freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef struct SDL_GPUTransferBuffer SDL_GPUTransferBuffer;
typedef struct SDL_GPUCommandBuffer SDL_GPUCommandBuffer;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Event { uint32_t type; } SDL_Event;
typedef uint32_t SDL_Scancode;

typedef enum { SDL_IO_SEEK_SET = 0, SDL_IO_SEEK_CUR = 1, SDL_IO_SEEK_END = 2 } SDL_IOWhence;
typedef enum { SDL_IO_STATUS_READY = 0, SDL_IO_STATUS_ERROR = 1, SDL_IO_STATUS_EOF = 2 } SDL_IOStatus;

typedef struct SDL_IOStream {
    FILE *fp;
    const uint8_t *mem;
    size_t mem_size;
    size_t mem_pos;
    SDL_IOStatus status;
} SDL_IOStream;

typedef struct {
    int year, month, day, hour, minute, second, nanosecond, day_of_week, utc_offset;
} SDL_DateTime;

#define SDL_INIT_VIDEO 0x20u
#define SDL_arraysize(a) (sizeof(a) / sizeof((a)[0]))
#define SDL_zero(x) memset(&(x), 0, sizeof(x))
#define SDL_clamp(x, a, b) ((x) < (a) ? (a) : ((x) > (b) ? (b) : (x)))
#define SDL_min(a, b) ((a) < (b) ? (a) : (b))
#define SDL_max(a, b) ((a) > (b) ? (a) : (b))
#define SDL_malloc malloc
#define SDL_calloc calloc
#define SDL_realloc realloc
#define SDL_free free
#define SDL_memcpy memcpy
#define SDL_memset memset
#define SDL_memcmp memcmp
#define SDL_strcmp strcmp
#define SDL_strncmp strncmp
#define SDL_strlcpy(d, s, n) do { strncpy((d), (s), (n) - 1); (d)[(n) - 1] = '\0'; } while (0)
#define SDL_fabsf fabsf
#define SDL_fmod fmod
#define SDL_sscanf sscanf

static inline int SDL_asprintf(char **out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    *out = (char *)malloc((size_t)n + 1);
    if (!*out) return -1;
    va_start(ap, fmt);
    vsnprintf(*out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return n;
}

SDL_IOStream *SDL_IOFromFile(const char *file, const char *mode);
SDL_IOStream *SDL_IOFromConstMem(const void *mem, size_t size);
bool SDL_CloseIO(SDL_IOStream *context);
size_t SDL_ReadIO(SDL_IOStream *context, void *ptr, size_t size);
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream *context);
int64_t SDL_SeekIO(SDL_IOStream *context, int64_t offset, SDL_IOWhence whence);
int64_t SDL_TellIO(SDL_IOStream *context);
void *SDL_LoadFile(const char *file, size_t *datasize);

bool SDL_GetCurrentTime(SDL_Time *ticks);
bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *dt, bool localTime);
uint64_t SDL_GetPerformanceCounter(void);
uint32_t SDL_rand_bits_r(uint64_t *state);

bool SDL_SetError(const char *fmt, ...);
const char *SDL_GetError(void);
void SDL_Log(const char *fmt, ...);
void SDL_LogError(int category, const char *fmt, ...);
#define SDL_LOG_CATEGORY_APPLICATION 0

static inline uint32_t SDL_WasInit(uint32_t flags) { (void)flags; return 0; }
static inline bool SDL_DisableScreenSaver(void) { return true; }
static inline bool SDL_EnableScreenSaver(void) { return true; }
static inline SDL_AudioStream *SDL_OpenAudioDeviceStream(int dev, const SDL_AudioSpec *spec, void *cb, void *ud) {
    (void)dev; (void)spec; (void)cb; (void)ud; return NULL;
}
#define SDL_AUDIO_DEVICE_DEFAULT_RECORDING 0

#endif

// Extra SDL3 primitives used by pc/sdk/*
#ifndef POKEBW2_VITA_SDL_SHIM_EXTRA
#define POKEBW2_VITA_SDL_SHIM_EXTRA
#include <pthread.h>
#include <malloc.h>
#include <sys/stat.h>

typedef pthread_mutex_t SDL_Mutex;
typedef volatile int SDL_SpinLock;
typedef enum { SDL_PATHTYPE_NONE = 0, SDL_PATHTYPE_FILE = 1, SDL_PATHTYPE_DIRECTORY = 2 } SDL_PathType;
typedef struct { SDL_PathType type; uint64_t size; } SDL_PathInfo;

static inline void *SDL_aligned_alloc(size_t alignment, size_t size) {
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    return memalign(alignment, size);
}
static inline void SDL_aligned_free(void *mem) { free(mem); }
static inline char *SDL_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
static inline SDL_Mutex *SDL_CreateMutex(void) {
    SDL_Mutex *m = (SDL_Mutex *)malloc(sizeof(SDL_Mutex));
    if (m) pthread_mutex_init(m, NULL);
    return m;
}
static inline void SDL_DestroyMutex(SDL_Mutex *m) {
    if (m) { pthread_mutex_destroy(m); free(m); }
}
static inline void SDL_LockMutex(SDL_Mutex *m) { if (m) pthread_mutex_lock(m); }
static inline void SDL_UnlockMutex(SDL_Mutex *m) { if (m) pthread_mutex_unlock(m); }
static inline void SDL_LockSpinlock(SDL_SpinLock *l) { while (__sync_lock_test_and_set(l, 1)) {} }
static inline void SDL_UnlockSpinlock(SDL_SpinLock *l) { __sync_lock_release(l); }

void SDL_Delay(uint32_t ms);
size_t SDL_WriteIO(SDL_IOStream *context, const void *ptr, size_t size);
bool SDL_FlushIO(SDL_IOStream *context);
int64_t SDL_GetIOSize(SDL_IOStream *context);
bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info);

static inline void SDL_ClearAudioStream(SDL_AudioStream *s) { (void)s; }
static inline void SDL_DestroyAudioStream(SDL_AudioStream *s) { (void)s; }
static inline bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) { (void)s; return true; }
static inline int SDL_GetAudioStreamData(SDL_AudioStream *s, void *buf, int len) { (void)s; (void)buf; (void)len; return 0; }
#endif
#define SDL_AUDIO_S8 1
