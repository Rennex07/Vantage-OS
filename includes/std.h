// The time I forgot std doesn't exist in bare metal T_T

#pragma once

#ifdef VANTAGE_SIM
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#else
#define NULL ((void*)0)

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;

typedef unsigned long long uintptr_t;

typedef unsigned long size_t;

void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);

/* The freestanding ABI may emit these calls for large structure copies/zeros. */
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* dest, int value, size_t n);
int memcmp(const void* a, const void* b, size_t n);
#endif
