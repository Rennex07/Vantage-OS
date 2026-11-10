#include <vantage/base.h>

void* memcpy(void* dest, const void* src, size_t size) {
    uint8_t* out = dest;
    const uint8_t* in = src;
    size_t i;
    for (i = 0; i < size; i++) out[i] = in[i];
    return dest;
}

void* memset(void* dest, int value, size_t size) {
    uint8_t* out = dest;
    size_t i;
    for (i = 0; i < size; i++) out[i] = (uint8_t)value;
    return dest;
}

int memcmp(const void* a, const void* b, size_t size) {
    const uint8_t* left = a;
    const uint8_t* right = b;
    for (size_t i = 0; i < size; i++)
        if (left[i] != right[i]) return (int)left[i] - (int)right[i];
    return 0;
}
