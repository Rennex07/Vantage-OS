#include <vantage/drivers/hardware.h>

#define MBOX_READ 0xfe00b880ULL
#define MBOX_STATUS 0xfe00b898ULL
#define MBOX_WRITE 0xfe00b8a0ULL
#define MBOX_FULL 0x80000000U
#define MBOX_EMPTY 0x40000000U

int property_call(PropertyMailbox* mailbox, const HwOps* io, uint32_t tag,
                  uint32_t* values, unsigned int count) {
    if (!mailbox || !hw_ops_valid(io) || !io->cache || !io->physical ||
        !values || !count || count > 16 || !tag || !io->owned(io->context)) return HW_INVALID;
    if (mailbox->state) return mailbox->state < 0 ? HW_OFF : HW_BUSY;
    uint64_t physical = io->physical(io->context, mailbox->words);
    if (!physical || physical >= 0x40000000ULL || (physical & 15U)) return HW_INVALID;
    mailbox->state = 1;
    memset(mailbox->words, 0, sizeof(mailbox->words));
    mailbox->words[0] = (count + 6) * 4;
    mailbox->words[2] = tag;
    mailbox->words[3] = count * 4;
    for (unsigned int i = 0; i < count; i++) mailbox->words[5 + i] = values[i];
    uint32_t token = (uint32_t)physical | 8U;
    io->cache(io->context, mailbox->words, sizeof(mailbox->words), HW_CLEAN_INVALIDATE);
    if (hw_wait(io, MBOX_STATUS, MBOX_FULL, 0, 1000000) != HW_OK) {
        /* No request was submitted, so the buffer is still ours. */
        mailbox->state = 0;
        return HW_TIMEOUT;
    }
    io->write(io->context, MBOX_WRITE, token);
    uint32_t start = io->time_us(io->context);
    for (unsigned int polls = 0; polls < 8000000U; polls++) {
        if (!(io->read(io->context, MBOX_STATUS) & MBOX_EMPTY) &&
            io->read(io->context, MBOX_READ) == token) {
            io->cache(io->context, mailbox->words, sizeof(mailbox->words), HW_INVALIDATE);
            mailbox->state = 0;
            if (mailbox->words[0] != (count + 6) * 4 || mailbox->words[1] != 0x80000000U ||
                mailbox->words[2] != tag || mailbox->words[3] != count * 4 ||
                mailbox->words[4] != (0x80000000U | count * 4) ||
                mailbox->words[5 + count] != 0) return HW_PROTOCOL;
            for (unsigned int i = 0; i < count; i++) values[i] = mailbox->words[5 + i];
            return HW_OK;
        }
        if (io->time_us(io->context) - start >= 1000000U) break;
    }
    mailbox->state = -1;
    return HW_TIMEOUT;
}
