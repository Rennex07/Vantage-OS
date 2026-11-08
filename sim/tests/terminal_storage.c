#include "../../user/include/terminal.h"
#include "../../user/include/user_lib.h"
#include "../../includes/files_internal.h"
#include "../platform/storage_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static struct { unsigned char before[32]; Terminal terminal; unsigned char after[32]; } guarded;
static char transcript[65536];
static size_t written;

static void output(void* context, char c) {
    (void)context;
    if (written + 1 < sizeof(transcript)) {
        transcript[written++] = c; transcript[written] = 0;
    }
}
static void command(const char* line) {
    while (*line) {
        KeyEvent event = {KEY_TEXT, (unsigned char)*line++};
        terminal_key(&guarded.terminal, &event);
    }
    KeyEvent enter = {KEY_ENTER, 0};
    terminal_key(&guarded.terminal, &enter);
    for (unsigned int i = 0; i < 32; i++) CHECK(guarded.before[i] == 0xa7 && guarded.after[i] == 0xa7);
}
static void contents(const char* expected) {
    unsigned char data[FILE_IO_MAX];
    int64_t handle = sys_file_open("/ram/notes", FILE_READ);
    CHECK(handle > 0);
    int count = sys_file_read((uint64_t)handle, data, sizeof(data));
    CHECK(count == (int)strlen(expected) && !memcmp(data, expected, (size_t)count));
    CHECK(sys_file_close((uint64_t)handle) == 0);
}

static void action(uint32_t which) { terminal_editor_action(&guarded.terminal, which); }
static void close_editor(void) {
    action(EDIT_EXIT);
    if (guarded.terminal.editing) action(EDIT_EXIT);
}
static void clear_document(void) {
    while (guarded.terminal.document_size) action(EDIT_DROP);
}
static void text(const char* line) {
    while (*line) {
        KeyEvent event = {KEY_TEXT, (unsigned char)*line++};
        terminal_key(&guarded.terminal, &event);
    }
}
static void tap(int x, int y) {
    TouchEvent e = {0};
    e.type = TOUCH_EVENT_DOWN; e.x = x; e.y = y;
    terminal_touch(&guarded.terminal, &e);
    e.type = TOUCH_EVENT_UP;
    terminal_touch(&guarded.terminal, &e);
}

int main(int argc, char** argv) {
    CHECK(argc == 3);
    memset(&guarded, 0xa7, sizeof(guarded));
    Terminal* t = &guarded.terminal;
    CHECK(terminal_init(t, 60, 33, NULL, output, NULL));
    CHECK(sim_storage_open(argv[2], 0) >= 0);
    if (!strcmp(argv[1], "write")) {
        command("edit notes"); CHECK(t->editing && !t->document_size);
        command("hello storage"); command("second line"); command(".save");
        action(EDIT_SAVE); CHECK(strstr(transcript, "Saved."));
        contents("hello storage\nsecond line\n.save\n");
        close_editor(); CHECK(!t->editing);
    } else if (!strcmp(argv[1], "read")) {
        contents("hello storage\nsecond line\n.save\n");
        command("ls"); CHECK(strstr(transcript, "/ram/notes"));
        command("cat notes"); CHECK(strstr(transcript, "hello storage\nsecond line\n.save\n"));
        command("edit /ram/notes"); CHECK(t->editing);
        /* Tap the second line and insert in its middle; no pointer is drawn. */
        tap(8 * TEXT_CELL_WIDTH, 4 * TEXT_CELL_HEIGHT);
        CHECK(t->edit_position == 16 && terminal_editor_selected(t, 4));
        text("!"); CHECK(!memcmp(t->document + 14, "se!cond", 7));
        KeyEvent backspace = {KEY_BACKSPACE, 0}; terminal_key(t, &backspace);
        action(EDIT_UP); CHECK(t->edit_position == 2);
        action(EDIT_LEFT); action(EDIT_RIGHT); CHECK(t->edit_position == 2);
        action(EDIT_DOWN); action(EDIT_NEW); text("new line");
        CHECK(!memcmp(t->document + 26, "new line\n.save", 14));
        action(EDIT_DROP);
        clear_document(); CHECK(!t->document_size);
        command("replacement"); action(EDIT_SAVE); contents("replacement\n");
        clear_document(); command("unsaved");
        action(EDIT_EXIT); CHECK(t->editing && t->edit_confirm);
        action(EDIT_EXIT); CHECK(!t->editing); contents("replacement\n");
        command("edit notes"); clear_document();
        /* Fill across syscall chunks and reach the exact per-file capacity. */
        char line[128]; memset(line, 'a', 127); line[127] = 0;
        for (unsigned int i = 0; i < 32; i++) command(line);
        CHECK(t->document_size == FILE_CAPACITY);
        command("overflow"); CHECK(t->document_size == FILE_CAPACITY);
        action(EDIT_SAVE); close_editor(); command("edit notes");
        CHECK(t->document_size == FILE_CAPACITY);
        for (unsigned int i = 0; i < FILE_CAPACITY; i++) CHECK(t->document[i] == (i % 128 == 127 ? '\n' : 'a'));
        CHECK(t->edit_top > 0 || t->edit_position == 0);
        /* A single full-length line is editable too; shell limits do not apply. */
        clear_document();
        for (unsigned int i = 0; i < 32; i++) text(line);
        text("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        CHECK(t->document_size == FILE_CAPACITY && t->edit_left > 0);
        action(EDIT_NEW); CHECK(t->document_size == FILE_CAPACITY);
        terminal_key(t, &backspace); text("z");
        action(EDIT_SAVE); close_editor(); command("edit notes");
        CHECK(t->document_size == FILE_CAPACITY && t->document[4095] == 'z');
        for (unsigned int i = 0; i < FILE_CAPACITY - 1; i++) CHECK(t->document[i] == 'a');
        clear_document(); action(EDIT_SAVE); contents("");
        close_editor(); command("rm notes");
        CHECK(sys_file_open("/ram/notes", FILE_READ) == FILE_NOT_FOUND);
        command("edit temporary"); command("discard"); close_editor();
        CHECK(sys_file_open("/ram/temporary", FILE_READ) == FILE_NOT_FOUND);
        command("edit delete-me"); command("data"); action(EDIT_SAVE); close_editor(); command("rm delete-me");
        CHECK(!t->editing && sys_file_open("/ram/delete-me", FILE_READ) == FILE_NOT_FOUND);
        command("edit ../invalid");
        CHECK(!t->editing); /* Invalid existing-file open is rejected. */
        int64_t binary = sys_file_open("/ram/binary", FILE_WRITE | FILE_CREATE);
        CHECK(binary > 0 && sys_file_write((uint64_t)binary, "\x1b[2J", 4) == 4);
        CHECK(sys_file_close((uint64_t)binary) == 0);
        command("edit binary"); CHECK(!t->editing && strstr(transcript, "Not an ASCII text file"));
        command("cat binary"); CHECK(strstr(transcript, "?[2J"));
        command("rm binary");
    } else {
        CHECK(!strcmp(argv[1], "deleted"));
        CHECK(sys_file_open("/ram/notes", FILE_READ) == FILE_NOT_FOUND);
        command("cat notes"); CHECK(strstr(transcript, "File not found"));
        command("edit notes"); command("keep this buffer");
        CHECK(t->editing && t->document_size == 17);
        CHECK(files_unmount() == 0);
        action(EDIT_SAVE); CHECK(t->editing && t->document_size == 17);
        CHECK(strstr(t->edit_status, "Save failed"));
        CHECK(!memcmp(t->document, "keep this buffer\n", 17));
        close_editor();
        command("edit notes"); CHECK(!t->editing && strstr(transcript, "Storage unavailable"));
        puts("PASS: editor create/read/replace/delete persists across fresh process mounts");
        return 0;
    }
    CHECK(files_unmount() == 0);
    return 0;
}
