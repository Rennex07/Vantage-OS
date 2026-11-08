#include "../../includes/files_internal.h"
#include "../platform/storage_host.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(int argc, char** argv) {
    CHECK(argc == 3);
    CHECK(sim_storage_open(argv[2], 0) == 0);
    int write = !strcmp(argv[1], "write");
    int64_t h = files_open(1, "/ram/reboot", write ? FILE_WRITE | FILE_CREATE : FILE_READ);
    CHECK(h > 0);
    if (write) CHECK(files_write(1, (uint64_t)h, "survives process exit", 21) == 21);
    else {
        char data[32] = {0};
        CHECK(files_read(1, (uint64_t)h, data, sizeof(data)) == 21);
        CHECK(!memcmp(data, "survives process exit", 21));
    }
    CHECK(files_close(1, (uint64_t)h) == 0);
    CHECK(sim_storage_close() == 0);
    return 0;
}
