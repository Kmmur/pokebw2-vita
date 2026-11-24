#ifndef POKEBW2_TYPES_H
#define POKEBW2_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef float f32;
typedef double f64;
typedef uintptr_t uptr;
typedef intptr_t sptr;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define NELEMS(array) (sizeof(array) / sizeof((array)[0]))
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))

typedef char VitaRequires32BitPointers[sizeof(void *) == 4 ? 1 : -1];
typedef char VitaRequires32BitWords[sizeof(u32) == 4 ? 1 : -1];
typedef char VitaRequiresFullWidthAddresses[sizeof(uptr) == sizeof(void *) ? 1 : -1];

#endif
