#pragma once

#include <vantage/kernel/parallel.h>

/* Architecture seam. Control fields use acquire/release, not volatile polling. */
uint32_t parallel_load(const uint32_t* value);
void parallel_store(uint32_t* value, uint32_t next);
unsigned int parallel_core_id(void);
uint32_t parallel_time_us(void);
void parallel_idle(void);
void parallel_wake(void);
void parallel_completion_barrier(void);
void parallel_port_halt(const char* message);

/* Single owner per worker slot. poll is also used by host regression threads. */
void parallel_worker_online(unsigned int core);
uint32_t parallel_worker_poll(unsigned int core);
void parallel_worker_entry(unsigned int core);

/* Immutable hooks installed by core 0 before scheduling starts. reserve(1)
 * claims idle workers atomically against task dispatch; reserve(0) releases. */
int parallel_task_install(uint32_t (*poll)(unsigned int core), int (*reserve)(int claim));
uint32_t parallel_task_mask(void);
int parallel_task_start(void);
void parallel_task_stop(void);
