#include "../include/terminal.h"
#include "../include/user_lib.h"

static void mirror(Terminal* t, char c) {
    if (t->output) t->output(t->context, c);
}

static void blank(Terminal* t) {
    uint32_t row, col;
    for (row = 0; row < t->rows; row++) {
        for (col = 0; col < t->columns; col++) t->cells[row][col] = ' ';
        t->dirty[row] = 1;
    }
    t->row = t->column = 0;
}

static void newline(Terminal* t) {
    uint32_t row, col;
    t->dirty[t->row] = 1;
    t->column = 0;
    if (++t->row == t->rows) {
        for (row = 1; row < t->rows; row++) {
            for (col = 0; col < t->columns; col++) t->cells[row - 1][col] = t->cells[row][col];
        }
        t->row--;
        for (col = 0; col < t->columns; col++) t->cells[t->row][col] = ' ';
        for (row = 0; row < t->rows; row++) t->dirty[row] = 1;
    }
    t->dirty[t->row] = 1;
}

static void put(Terminal* t, char c) {
    mirror(t, c);
    if (c == '\n') {
        newline(t);
        return;
    }
    if (t->column == t->columns) newline(t);
    t->cells[t->row][t->column++] = c;
    t->dirty[t->row] = 1;
}

static void write(Terminal* t, const char* text) {
    while (*text) put(t, *text++);
}

static void number(Terminal* t, uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) put(t, digits[--count]);
}

static uint32_t command_is(const char* command, const char* word) {
    while (*word && *command == *word) { command++; word++; }
    return !*word && (!*command || *command == ' ');
}

static void memory(Terminal* t) {
    ConsoleInfo info = {0};
    if (!t->info || !t->info(t->context, &info)) {
        write(t, "Memory statistics unavailable.\n");
    } else if (info.memory_kind == CONSOLE_MEMORY_HOST) {
        write(t, "Simulator: host-managed memory.\nBack buffer: ");
        number(t, info.host_backbuffer_bytes);
        write(t, " bytes\nKernel MM is tested separately.\n");
    } else if (info.memory_kind == CONSOLE_MEMORY_KERNEL) {
        write(t, "RAM: "); number(t, info.memory.ram_bytes / (1024 * 1024));
        write(t, " MiB\nFree pages: "); number(t, info.memory.free_pages);
        put(t, '/'); number(t, info.memory.allocatable_pages);
        write(t, "\nHeap used: "); number(t, info.memory.heap_used_bytes);
        put(t, '/'); number(t, info.memory.heap_bytes);
        write(t, " bytes\n");
    } else write(t, "Memory statistics unavailable.\n");
}

static void file_error(Terminal* t, int error) {
    const char* message = "File operation failed";
    if (error == FILE_UNAVAILABLE) message = "Storage unavailable (not mounted)";
    else if (error == FILE_NOT_FOUND) message = "File not found";
    else if (error == FILE_BAD) message = "Invalid filename or request";
    else if (error == FILE_NO_SPACE) message = "Storage or file capacity exhausted";
    else if (error == FILE_BUSY) message = "File is busy";
    else if (error == FILE_IO_ERROR) message = "Storage I/O failed; remount required";
    else if (error == FILE_CORRUPT) message = "Storage is corrupt";
    write(t, message);
    write(t, ".\n");
}

static int file_path(const char* argument, char path[FILE_PATH_MAX]) {
    uint32_t length = 0;
    while (*argument == ' ') argument++;
    if (!*argument) return 0;
    if (*argument != '/') {
        const char* prefix = "/ram/";
        while (*prefix) path[length++] = *prefix++;
    }
    while (*argument && *argument != ' ') {
        if (length == FILE_PATH_MAX - 1) return 0;
        path[length++] = *argument++;
    }
    while (*argument == ' ') argument++;
    path[length] = 0;
    return !*argument;
}

static void show_bytes(Terminal* t, const unsigned char* data, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        unsigned char c = data[i];
        /* Never send stored control/escape sequences to the serial terminal. */
        put(t, c == '\n' || (c >= 32 && c <= 126) ? (char)c : '?');
    }
}

static void list_files(Terminal* t) {
    FileEntry entry = {0};
    int result;
    while ((result = sys_file_list(&entry)) > 0) {
        write(t, entry.path); write(t, "  "); number(t, entry.size); write(t, " bytes\n");
    }
    if (result < 0) file_error(t, result);
}

static void editor_draw(Terminal* t);
static const uint32_t editor_controls[] = {
    EDIT_SAVE, EDIT_EXIT, EDIT_NEW, EDIT_DROP,
    EDIT_UP, EDIT_DOWN, EDIT_LEFT, EDIT_RIGHT
};

static void open_text(Terminal* t, const char* path, int edit) {
    if (edit && (t->columns < 40 || t->rows < 12)) {
        write(t, "Editor needs at least 40 columns and 12 rows.\n"); return;
    }
    int64_t handle = sys_file_open(path, FILE_READ);
    uint32_t used = 0;
    int result = 0;
    if (handle < 0 && !(edit && handle == FILE_NOT_FOUND)) {
        file_error(t, (int)handle); return;
    }
    if (handle >= 0) {
        unsigned char chunk[FILE_IO_MAX];
        while ((result = sys_file_read((uint64_t)handle, chunk, sizeof(chunk))) > 0) {
            if (!edit) { show_bytes(t, chunk, (uint32_t)result); continue; }
            if ((uint32_t)result > FILE_CAPACITY - used) { result = FILE_NO_SPACE; break; }
            uint32_t i;
            for (i = 0; i < (uint32_t)result; i++) {
                unsigned char c = chunk[i];
                if (c != '\n' && c != '\r' && c != '\t' && (c < 32 || c > 126)) break;
            }
            if (i != (uint32_t)result) {
                write(t, "Not an ASCII text file; left unchanged.\n");
                (void)sys_file_close((uint64_t)handle); return;
            }
            memcpy(t->document + used, chunk, (uint32_t)result);
            used += (uint32_t)result;
        }
        int closed = sys_file_close((uint64_t)handle);
        if (result < 0 || closed < 0) { file_error(t, result < 0 ? result : closed); return; }
    }
    if (!edit) { put(t, '\n'); return; }
    for (uint32_t i = 0; i < FILE_PATH_MAX; i++) {
        t->document_path[i] = path[i];
        if (!path[i]) break;
    }
    t->document_size = used;
    t->editing = 1;
    t->edit_position = t->edit_top = t->edit_left = t->edit_modified = t->edit_confirm = 0;
    t->edit_touch = ~0U;
    t->edit_status = "Tap text to edit; Save writes to storage.";
    editor_draw(t);
}

static uint32_t line_start(const Terminal* t, uint32_t position) {
    while (position && t->document[position - 1] != '\n') position--;
    return position;
}

static uint32_t line_end(const Terminal* t, uint32_t position) {
    while (position < t->document_size && t->document[position] != '\n') position++;
    return position;
}

static uint32_t line_at(const Terminal* t, uint32_t index) {
    uint32_t position = 0;
    while (index-- && position < t->document_size) {
        position = line_end(t, position);
        if (position < t->document_size) position++;
    }
    return position;
}

static uint32_t line_number(const Terminal* t, uint32_t position) {
    uint32_t number = 0;
    for (uint32_t i = 0; i < position; i++) if (t->document[i] == '\n') number++;
    return number;
}

static void editor_label(Terminal* t, uint32_t row, uint32_t col, const char* text) {
    while (*text && col < t->columns) t->cells[row][col++] = *text++;
}

static void editor_number(Terminal* t, uint32_t row, uint32_t* col, uint32_t value) {
    char digits[10]; uint32_t count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count && *col < t->columns) t->cells[row][(*col)++] = digits[--count];
}

uint32_t terminal_editor_selected(const Terminal* t, uint32_t row) {
    if (!t->editing || row < 2 || row >= t->rows - 8) return 0;
    return t->edit_top + (row - 2) / 2 == line_number(t, t->edit_position);
}

static void editor_draw(Terminal* t) {
    static const char* labels[] = {"Save", "Exit", "New", "Drop", "Up", "Down", "Left", "Right"};
    uint32_t selected = line_number(t, t->edit_position), height = (t->rows - 10) / 2;
    uint32_t column = t->edit_position - line_start(t, t->edit_position), width = t->columns - 6;
    if (selected < t->edit_top) t->edit_top = selected;
    if (selected >= t->edit_top + height) t->edit_top = selected - height + 1;
    if (column < t->edit_left) t->edit_left = column;
    if (column >= t->edit_left + width) t->edit_left = column - width + 1;
    blank(t);
    editor_label(t, 0, 0, t->edit_modified ? "Edit * " : "Edit   ");
    editor_label(t, 0, 7, t->document_path);
    uint32_t col = 0;
    editor_number(t, 1, &col, t->document_size);
    editor_label(t, 1, col, "/4096 B  Ln "); col += 12;
    editor_number(t, 1, &col, selected + 1);
    editor_label(t, 1, col, " Col "); col += 5;
    editor_number(t, 1, &col, column + 1);
    uint32_t position = line_at(t, t->edit_top);
    for (uint32_t i = 0; i < height; i++) {
        uint32_t row = 2 + i * 2, end = line_end(t, position), c = 0;
        editor_number(t, row, &c, t->edit_top + i + 1);
        t->cells[row][5] = t->edit_left ? '<' : '|';
        for (c = 0; c < width && t->edit_left + c < end - position; c++) {
            unsigned char ch = t->document[position + t->edit_left + c];
            t->cells[row][6 + c] = ch >= 32 && ch <= 126 ? (char)ch : ' ';
        }
        if (end == t->document_size) break;
        position = end + 1;
    }
    editor_label(t, t->rows - 8, 0, t->edit_status);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t row = t->rows - 6 + (i / 4) * 3, left = (i % 4) * t->columns / 4;
        editor_label(t, row + 1, left + 1, t->edit_confirm && editor_controls[i] == EDIT_EXIT ? "Discard" : labels[i]);
    }
}

static void editor_close(Terminal* t) {
    t->editing = 0;
    blank(t);
    write(t, "Editor closed.\n> ");
}

static void editor_save(Terminal* t) {
    int64_t handle = sys_file_open(t->document_path, FILE_WRITE | FILE_CREATE | FILE_TRUNCATE);
    int result = 0;
    if (handle < 0) result = (int)handle;
    else {
        uint32_t offset = 0;
        while (offset < t->document_size) {
            uint32_t count = t->document_size - offset;
            if (count > FILE_IO_MAX) count = FILE_IO_MAX;
            result = sys_file_write((uint64_t)handle, t->document + offset, count);
            if (result != (int)count) { if (result >= 0) result = FILE_IO_ERROR; break; }
            offset += count;
        }
        int closed = sys_file_close((uint64_t)handle);
        if (result >= 0 && closed < 0) result = closed;
    }
    if (result < 0) {
        file_error(t, result); /* Retain useful serial diagnostics. */
        t->edit_modified = 1;
        t->edit_status = "Save failed; buffer kept, disk may be partial.";
    } else {
        t->edit_modified = 0;
        t->edit_status = "Saved.";
        write(t, "Saved.\n");
    }
}

static void editor_insert(Terminal* t, unsigned char c) {
    if (t->document_size == FILE_CAPACITY) {
        t->edit_status = "Full (4096 bytes); nothing inserted."; return;
    }
    for (uint32_t i = t->document_size; i > t->edit_position; i--) t->document[i] = t->document[i - 1];
    t->document[t->edit_position++] = c;
    t->document_size++;
    t->edit_modified = 1;
}

void terminal_editor_action(Terminal* t, uint32_t action) {
    if (!t || !t->editing || action > EDIT_DROP) return;
    if (action == EDIT_EXIT) {
        if (!t->edit_modified || t->edit_confirm) { editor_close(t); return; }
        t->edit_confirm = 1;
        t->edit_status = "Unsaved! Tap Discard again, or Save to keep.";
        editor_draw(t); return;
    }
    t->edit_confirm = 0;
    t->edit_status = "Tap text to edit; Save writes to storage.";
    uint32_t start = line_start(t, t->edit_position), end = line_end(t, t->edit_position);
    uint32_t column = t->edit_position - start;
    if (action == EDIT_SAVE) editor_save(t);
    else if (action == EDIT_LEFT && t->edit_position) t->edit_position--;
    else if (action == EDIT_RIGHT && t->edit_position < t->document_size) t->edit_position++;
    else if (action == EDIT_UP && start) {
        uint32_t previous = line_start(t, start - 1);
        t->edit_position = previous + (column < start - 1 - previous ? column : start - 1 - previous);
    } else if (action == EDIT_DOWN && end < t->document_size) {
        uint32_t next = line_end(t, end + 1);
        t->edit_position = end + 1 + (column < next - end - 1 ? column : next - end - 1);
    } else if (action == EDIT_NEW) {
        t->edit_position = end; editor_insert(t, '\n');
    } else if (action == EDIT_DROP) {
        if (end < t->document_size) end++;
        else if (start) { start--; }
        if (end > start) {
            uint32_t count = end - start;
            for (uint32_t i = start; i + count < t->document_size; i++) t->document[i] = t->document[i + count];
            t->document_size -= count; t->edit_position = start; t->edit_modified = 1;
        }
    }
    editor_draw(t);
}

static uint32_t editor_hit(const Terminal* t, int x, int y) {
    if (x < 0 || y < 0 || (uint32_t)x >= t->columns * TEXT_CELL_WIDTH ||
        (uint32_t)y >= t->rows * TEXT_CELL_HEIGHT) return ~0U;
    uint32_t row = (uint32_t)y / TEXT_CELL_HEIGHT;
    if (row >= t->rows - 6) return ((row - (t->rows - 6)) / 3) * 4 +
        (uint32_t)x * 4 / (t->columns * TEXT_CELL_WIDTH);
    if (row >= 2 && row < 2 + ((t->rows - 10) / 2) * 2) return 8 + (row - 2) / 2;
    return ~0U;
}

void terminal_touch(Terminal* t, const TouchEvent* event) {
    if (!t || !event || !t->editing) return;
    uint32_t hit = editor_hit(t, event->x, event->y);
    if (event->type == TOUCH_EVENT_DOWN) t->edit_touch = hit;
    else if (event->type == TOUCH_EVENT_MOVE && hit != t->edit_touch) t->edit_touch = ~0U;
    else if (event->type == TOUCH_EVENT_UP) {
        uint32_t pressed = t->edit_touch;
        t->edit_touch = ~0U;
        if (hit == ~0U || hit != pressed) return;
        if (hit < 8) { terminal_editor_action(t, editor_controls[hit]); return; }
        uint32_t index = t->edit_top + hit - 8;
        if (index > line_number(t, t->document_size)) return;
        uint32_t start = line_at(t, index), end = line_end(t, start);
        uint32_t col = (uint32_t)event->x / TEXT_CELL_WIDTH;
        col = (col > 6 ? col - 6 : 0) + t->edit_left;
        t->edit_position = start + (col < end - start ? col : end - start);
        t->edit_confirm = 0;
        editor_draw(t);
    }
}

static void execute(Terminal* t) {
    const char* command = t->command;
    while (*command == ' ') command++;
    if (!*command) return;
    if (command_is(command, "help")) {
        write(t, "help   Show commands\nclear  Clear the terminal\nmem    Memory statistics\necho   Print text\nls     List files\ncat    Read a file\nedit   Open text editor\nrm     Delete a file\nabout  About this shell\n");
    } else if (command_is(command, "ls")) list_files(t);
    else if (command_is(command, "cat") || command_is(command, "edit") || command_is(command, "rm")) {
        char path[FILE_PATH_MAX];
        int edit = command_is(command, "edit"), remove = command_is(command, "rm");
        if (!file_path(command + (edit ? 4 : remove ? 2 : 3), path)) {
            write(t, "Usage: cat/edit/rm filename (or /ram/filename).\n"); return;
        }
        if (remove) {
            int result = sys_file_remove(path);
            if (result < 0) file_error(t, result); else write(t, "File deleted.\n");
        } else open_text(t, path, edit);
    } else if (command_is(command, "clear")) {
        blank(t);
        mirror(t, '\x1b');
        mirror(t, '['); mirror(t, '2'); mirror(t, 'J');
        mirror(t, '\x1b'); mirror(t, '['); mirror(t, 'H');
    } else if (command_is(command, "mem")) memory(t);
    else if (command_is(command, "echo")) {
        command += 4;
        while (*command == ' ') command++;
        write(t, command);
        put(t, '\n');
    } else if (command_is(command, "about")) {
        write(t, "Vantage-OS built-in shell.\nASCII text editor; files require mounted storage.\n");
    } else write(t, "Unknown command. Type 'help'.\n");
}

uint32_t terminal_init(Terminal* t, uint32_t columns, uint32_t rows,
                       TerminalInfoFn info, TerminalOutputFn output, void* context) {
    if (!t || columns < 16 || columns > TEXT_MAX_COLUMNS || rows < 4 || rows > TERMINAL_MAX_ROWS) return 0;
    t->columns = columns;
    t->rows = rows;
    t->length = t->overflow = 0;
    t->editing = t->document_size = 0;
    t->command[0] = 0;
    t->input_limit = columns * (rows - 1) - 6; /* Reserve the longer edit prompt. */
    if (t->input_limit >= TERMINAL_COMMAND_SIZE) t->input_limit = TERMINAL_COMMAND_SIZE - 1;
    t->info = info;
    t->output = output;
    t->context = context;
    blank(t);
    write(t, "Vantage terminal\nType 'help' for commands.\n\n> ");
    return 1;
}

void terminal_key(Terminal* t, const KeyEvent* event) {
    if (!t || !event || !t->columns || !t->rows) return;
    if (t->editing) {
        if (event->type == KEY_CANCEL) { terminal_editor_action(t, EDIT_EXIT); return; }
        t->edit_confirm = 0;
        t->edit_status = "Tap text to edit; Save writes to storage.";
        if (event->type == KEY_ENTER) editor_insert(t, '\n');
        else if (event->type == KEY_TEXT && event->character >= 32 && event->character <= 126)
            editor_insert(t, (unsigned char)event->character);
        else if (event->type == KEY_BACKSPACE && t->edit_position) {
            for (uint32_t i = t->edit_position; i < t->document_size; i++) t->document[i - 1] = t->document[i];
            t->edit_position--; t->document_size--; t->edit_modified = 1;
        }
        editor_draw(t); return;
    }
    t->dirty[t->row] = 1;
    if (event->type == KEY_TEXT && event->character >= 32 && event->character <= 126) {
        if (t->length < t->input_limit && !t->overflow) {
            char c = (char)event->character;
            t->command[t->length++] = c;
            t->command[t->length] = 0;
            put(t, c);
        } else t->overflow = 1;
    } else if (event->type == KEY_BACKSPACE && t->length && !t->overflow) {
        t->command[--t->length] = 0;
        if (!t->column && t->row) {
            t->row--;
            t->column = t->columns;
        }
        if (t->column) t->cells[t->row][--t->column] = ' ';
        mirror(t, '\b'); mirror(t, ' '); mirror(t, '\b');
    } else if (event->type == KEY_ENTER || event->type == KEY_CANCEL) {
        if (event->type == KEY_CANCEL) write(t, "^C");
        put(t, '\n');
        if (event->type == KEY_ENTER) {
            if (t->overflow) write(t, "Input too long; command discarded.\n");
            else execute(t);
        }
        t->length = t->overflow = 0;
        t->command[0] = 0;
        if (t->editing) editor_draw(t);
        else write(t, "> ");
    } else if (event->type == KEY_CLEAR) {
        uint32_t i;
        blank(t);
        mirror(t, '\x1b'); mirror(t, '['); mirror(t, '2'); mirror(t, 'J');
        mirror(t, '\x1b'); mirror(t, '['); mirror(t, 'H');
        write(t, t->editing ? "edit> " : "> ");
        for (i = 0; i < t->length; i++) put(t, t->command[i]);
    }
    t->dirty[t->row] = 1;
}
