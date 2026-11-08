#pragma once
/* Mount an existing regular disk image; never creates or formats it. */
int sim_storage_open(const char* path, unsigned int partition);
int sim_storage_close(void);
