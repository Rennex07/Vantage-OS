# Built-in terminal

The simulator and Pi run the same terminal app with a graphical QWERTY keyboard.
It supports built-in commands, line editing and scrolling.

## Usage

Type with your PC keyboard or click the on-screen keys. On Pi, use the touchscreen
or serial at **115200 baud, 8N1**, with local echo disabled.

| Command | Action |
|---|---|
| `help` | List commands |
| `clear` | Clear the terminal |
| `mem` | Show memory information |
| `echo text` | Print text |
| `about` | About the terminal |
| `ls` | List stored files |
| `cat name` | Read a file |
| `edit name` | Edit a text file |
| `rm name` | Delete a file |

Backspace edits, Ctrl+C cancels a line, and Ctrl+L clears the display while keeping
your input. On-screen Shift applies to the next character. Hold Del to repeat
deletion; release or drag off to stop.

`mem` shows host-managed display memory in the simulator and kernel allocator
statistics on Pi.

## Scope

Input is printable ASCII, limited to 127 characters per command. Overlong input is
discarded. The terminal does not yet run programs, and has no command history or
physical USB keyboard driver.

Physical Pi touch, serial and display validation is pending. Desktop and mocked
ARM tests are described in the [developer guide](DEVELOPMENT.md#terminal).

## Font

The bitmap font uses [JetBrains Mono v2.304](https://github.com/JetBrains/JetBrainsMono/tree/v2.304)
under the [SIL Open Font License](../drivers/video/OFL.txt). Normal builds use the
included bitmap; regeneration is documented in the developer guide.

## Persistent text files

With storage mounted, use `ls`, `cat notes`, `rm notes`, or `edit notes`.
Short names refer to `/ram/`; full `/ram/name` paths also work. `edit notes` opens
a compact, full-screen, touch-first ASCII editor above the keyboard. Existing
files load into memory; missing files start empty and are created only on Save.

Tap a line where you want to insert text, then type with the on-screen keyboard
or desktop keyboard. The selected line is highlighted; there is no pointer or
blinking text cursor. The header shows the insertion line/column, byte count and
an asterisk for unsaved changes. Backspace removes the preceding character;
Enter splits the line. Text that looks like a shell or dot command is literal
document content while the editor is open.

| Touch control | Action |
|---|---|
| Save | Write the whole buffer to the current filename |
| Exit | Return to the shell; with unsaved changes, tap Discard again to confirm |
| Up / Down | Select the adjacent line; scroll when it leaves the view |
| Left / Right | Move the insertion point one character, including across lines |
| New | Insert a newline at the end of the selected line |
| Drop | Remove the selected line |

To cancel a pending discard, select text, edit, or use another control. Saving
also cancels the discard confirmation. The on-screen Cancel key (or Ctrl+C) acts
like Exit. Dragging away from a control before release cancels that touch.
Long lines scroll horizontally to keep the insertion position in view; `<` at
the left of the text means its beginning is off-screen. Each line has a two-row
touch target. The editor needs at least 40 columns and 12 text rows.

Files still hold at most 4096 bytes, independently of SD/FAT cluster sizes.
Editor lines are not subject to the shell's 127-character command limit. An edit
that would exceed capacity is rejected without discarding the buffer. Binary
files are rejected; existing tabs and carriage returns display as spaces and
are preserved unless edited. Use `rm name` at the shell to delete a saved file.
The graphical display is required for editor navigation and Save; the serial
console can supply text but does not mirror the full-screen editor view.

Saving truncates the file and writes it in 256-byte chunks through `SYS_FILE_*`.
Each chunk is durable separately: a power loss during save can leave an empty or
partially saved file. Failed saves retain the in-memory buffer but may already
have changed the disk file. There is no atomic whole-document replacement yet.

On Pi, storage is available only in the explicitly enabled storage-validation
image described in [STORAGE.md](STORAGE.md); normal SD boot remains disabled.
In the simulator, provision a regular image with `tools/storage_image.py` and
set `VANTAGE_STORAGE_IMAGE` to its path before launch. An unmounted filesystem
reports `Storage unavailable` rather than silently saving to RAM.
