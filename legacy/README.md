# Historical examples

This directory retains earlier filesystem, RAM-disk and runtime experiments.
They are excluded from the production source manifest and supported host suites.
Their original source attribution is retained.

- `fs/fat.c` assumes scratch memory at the kernel end and defines its own
  incompatible `memcmp`; it is not the persistent snapshot filesystem.
- `fs/block.c` and `tests/ramdisk.c` are the independent historical RAM disk.
- `runtime/std.c` duplicates memory routines now provided by `lib/memory.c`.

Do not add these files back to the kernel build as a shortcut. Integrating an
example requires reviewing its memory, API and license contracts and adding
tests without competing production symbols.
