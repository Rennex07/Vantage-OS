#include "../../includes/wifi.h"

int wifi_nvram_pack(const unsigned char* text, size_t bytes, unsigned char* output,
                     size_t capacity, size_t* packed_bytes) {
    if (!text || !bytes || bytes > WIFI_NVRAM_MAX || !output || !packed_bytes || capacity < 4)
        return HW_INVALID;
    size_t used = 0, offset = 0;
    unsigned int variables = 0;
    while (offset < bytes) {
        size_t start = offset, end = offset;
        while (offset < bytes && text[offset] != '\n') {
            unsigned char c = text[offset];
            if (c != '\t' && c != '\r' && (c < 32 || c > 126)) return HW_INVALID;
            offset++;
        }
        end = offset;
        if (offset < bytes) offset++;
        for (size_t i = start; i < end; i++) if (text[i] == '#') { end = i; break; }
        while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) end--;
        if (start == end) continue;
        size_t equal = start;
        while (equal < end && text[equal] != '=') {
            unsigned char c = text[equal++];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_')) return HW_INVALID;
        }
        if (equal == start || equal == end || end - start > 1024 || end - start + 2 > capacity - used)
            return HW_INVALID;
        memcpy(output + used, text + start, end - start);
        used += end - start; output[used++] = 0; variables++;
    }
    if (!variables || used >= capacity) return HW_INVALID;
    output[used++] = 0;
    while (used & 3U) { if (used == capacity) return HW_NO_SPACE; output[used++] = 0; }
    *packed_bytes = used;
    return HW_OK;
}
