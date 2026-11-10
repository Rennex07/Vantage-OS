#include <vantage/user.h>
#include <vantage/abi/syscall.h>
#include <stddef.h>

/* The layout refactor must not change existing userspace payloads or numbers. */
_Static_assert(SYS_TOUCH_READ == 1 && SYS_TIME_US == 12, "device syscall IDs");
_Static_assert(SYS_TASK_ID == 16 && SYS_TASK_YIELD == 17 && SYS_TASK_EXIT == 18, "task syscall IDs");
_Static_assert(SYS_FILE_OPEN == 32 && SYS_FILE_REMOVE == 38, "file syscall IDs");
_Static_assert(sizeof(FileOpen) == 56 && offsetof(FileOpen, path) == 8, "file open ABI");
_Static_assert(sizeof(FileIo) == 272 && offsetof(FileIo, data) == 16, "file I/O ABI");
_Static_assert(sizeof(FileSeek) == 16 && sizeof(FileEntry) == 56, "file metadata ABI");
_Static_assert(sizeof(TouchEvent) == 12 && sizeof(KeyEvent) == 8, "input ABI");
_Static_assert(sizeof(TextRow) == 116 && sizeof(CanvasButton) == 40, "graphics ABI");
_Static_assert(sizeof(MemoryStats) == 40 && sizeof(ConsoleInfo) == 64, "console info ABI");
_Static_assert(sizeof(ConsoleWrite) == 132, "console write ABI");

int main(void) { return 0; }
