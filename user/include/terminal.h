#pragma once
#include "../../includes/console.h"
#include "../../includes/files.h"
#include "../../includes/input/touch.h"

#define TERMINAL_MAX_ROWS 60U
#define TERMINAL_COMMAND_SIZE 128U

typedef uint32_t (*TerminalInfoFn)(void* context, ConsoleInfo* info);
typedef void (*TerminalOutputFn)(void* context, char character);

typedef struct {
    uint32_t columns, rows, column, row;
    uint32_t length, input_limit, overflow;
    char cells[TERMINAL_MAX_ROWS][TEXT_MAX_COLUMNS];
    char command[TERMINAL_COMMAND_SIZE];
    uint8_t dirty[TERMINAL_MAX_ROWS];
    uint32_t editing, document_size;
    uint32_t edit_position, edit_top, edit_left, edit_modified, edit_confirm;
    uint32_t edit_touch;
    const char* edit_status;
    char document_path[FILE_PATH_MAX];
    unsigned char document[FILE_CAPACITY];
    TerminalInfoFn info;
    TerminalOutputFn output;
    void* context;
} Terminal;

uint32_t terminal_init(Terminal* terminal, uint32_t columns, uint32_t rows,
                       TerminalInfoFn info, TerminalOutputFn output, void* context);
void terminal_key(Terminal* terminal, const KeyEvent* event);

enum { EDIT_SAVE, EDIT_EXIT, EDIT_UP, EDIT_DOWN, EDIT_LEFT, EDIT_RIGHT, EDIT_NEW, EDIT_DROP };
void terminal_editor_action(Terminal* terminal, uint32_t action);
void terminal_touch(Terminal* terminal, const TouchEvent* event);
uint32_t terminal_editor_selected(const Terminal* terminal, uint32_t row);

void terminal_app_init(void);
void terminal_app_step(void);
