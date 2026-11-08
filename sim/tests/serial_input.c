#include "../../includes/input/key.h"
#include "../../includes/io.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define UART_IO 0xfe215040UL
#define UART_LSR 0xfe215054UL

static struct { uint32_t status, byte; } input[256];
static size_t count, consumed, reads;
static unsigned char output[64];
static size_t output_count;

unsigned int mmio_read(long reg) {
    reads++;
    if ((unsigned long)reg == UART_LSR) {
        uint32_t status = consumed < count ? input[consumed].status : 0;
        if (status == 2) consumed++; /* LSR read clears overrun without data. */
        return status | 0x20; /* Transmitter is ready independently of RX. */
    }
    CHECK((unsigned long)reg == UART_IO && consumed < count);
    return input[consumed++].byte;
}
void mmio_write(long reg, unsigned int value) {
    CHECK((unsigned long)reg == UART_IO && output_count < sizeof(output));
    output[output_count++] = (unsigned char)value;
}

static void add(uint32_t status, uint32_t byte) {
    CHECK(count < 256);
    input[count].status = status;
    input[count++].byte = byte;
}
static void text(const char* string) { while (*string) add(1, (unsigned char)*string++); }
static void expect(uint32_t type, uint32_t character) {
    KeyEvent event;
    CHECK(key_read(&event) && event.type == type && event.character == character);
}

int main(void) {
    KeyEvent event;
    uint8_t byte;
    CHECK(!key_read(NULL) && !uart_read_byte(NULL) && !reads);
    CHECK(!key_read(&event) && event.type == KEY_NONE);
    text("a\r\nb\n\b\x7f\x0c\x03\x1b[A\x1b[12~Z");
    expect(KEY_TEXT, 'a'); expect(KEY_ENTER, 0); expect(KEY_TEXT, 'b'); expect(KEY_ENTER, 0);
    expect(KEY_BACKSPACE, 0); expect(KEY_BACKSPACE, 0); expect(KEY_CLEAR, 0); expect(KEY_CANCEL, 0);
    expect(KEY_TEXT, 'Z'); CHECK(!key_read(&event));
    text("before");
    add(3, 'x'); /* An overrun with readable data must drain and cancel the line. */
    text("truncated\r\nnext");
    expect(KEY_TEXT, 'b'); expect(KEY_TEXT, 'e'); expect(KEY_TEXT, 'f');
    expect(KEY_TEXT, 'o'); expect(KEY_TEXT, 'r'); expect(KEY_TEXT, 'e');
    expect(KEY_CANCEL, 0);
    expect(KEY_TEXT, 'n'); expect(KEY_TEXT, 'e'); expect(KEY_TEXT, 'x'); expect(KEY_TEXT, 't');
    CHECK(!key_read(&event));
    add(2, 0); text("bad\n"); expect(KEY_CANCEL, 0);
    add(1, 0xff); add(1, 0); text("Q"); expect(KEY_TEXT, 'Q');
    CHECK(!uart_read_byte(&byte));
    uart_write_bytes("a\0b\n", 4);
    CHECK(output_count == 5 && output[0] == 'a' && !output[1] && output[2] == 'b' &&
          output[3] == '\r' && output[4] == '\n');
    puts("PASS: nonblocking UART input, CRLF, control keys, ANSI filtering and overrun cancellation");
    return 0;
}
