#pragma once
#include <vantage/base.h>

/* Called by the architecture exception entry or the simulator transport. */
uint64_t handle_sys_call(uint64_t syscall_id, uint64_t arg0);
