#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/bits.h>
#include <sys/compiler.h>
#include <sys/error.h>
#include <sys/io.h>
#include <sys/message.h>

#define VERSATILE_UART0_BASE    0x101F1000
#define VERSATILE_UART0_IRQ     12
#define PL011_MMAP_SIZE         4096

typedef struct
{
    uint32_t        DR;         /* Offset 0x000 */
    uint32_t        SR;         /* Offset 0x004 */
    uint32_t        reserved1;  /* Offset 0x008 */
    uint32_t        reserved2;  /* Offset 0x00c */
    uint32_t        reserved3;  /* Offset 0x010 */
    uint32_t        reserved4;  /* Offset 0x014 */
    uint32_t const  FR;         /* Offset 0x018 */
    uint32_t        reserved5;  /* Offset 0x01c */
    uint32_t        ILPR;       /* Offset 0x020 */
    uint32_t        IBRD;       /* Offset 0x024 */
    uint32_t        FBRD;       /* Offset 0x028 */
    uint32_t        LCR_H;      /* Offset 0x02C */
    uint32_t        CR;         /* Offset 0x030 */
    uint32_t        IFLS;       /* Offset 0x034 */
    uint32_t        IMSC;       /* Offset 0x038 */
    uint32_t const  RIS;        /* Offset 0x03C */
    uint32_t const  MIS;        /* Offset 0x040 */
    uint32_t        ICR;        /* Offset 0x044 */
    uint32_t        DMACR;      /* Offset 0x048 */
} pl011_t;

COMPILER_ASSERT(sizeof(pl011_t) == (0x048 + 4));

enum
{
    /**
     * \brief   Receive Fifo Empty
     *
     * Bit in FR register which, when set, says that there are not
     * bytes available to read.
     */
    FR_RXFE = SETBIT(4),

    /**
     * \brief   Transmit Fifo Full
     *
     * Bit in FR register which, when set, says that the output
     * pipeline is full.
     */
    FR_TXFF = SETBIT(5),
};

enum
{
    /**
     * \brief   Transmit enabled
     */
    CR_TXE = SETBIT(8),

    /**
     * \brief   Receive enabled
     */
    CR_RXE = SETBIT(9),

    /**
     * \brief   UART enabled
     */
    CR_UARTEN = SETBIT(0),
};

enum
{
    /**
     * \brief   Receive interrupt masked
     */
    IMSC_RX = SETBIT(4),

    /**
     * \brief   Transmit interrupt masked
     */
    IMSC_TX = SETBIT(5),
};

enum
{
    /**
     * \brief   Receive interrupt is high
     */
    MIS_RX = SETBIT(4),

    /**
     * \brief   Transmit interrupt is high
     */
    MIS_TX = SETBIT(5),
};

enum
{
    /**
     * \brief   Clear Receive interrupt
     */
    ICR_RX = SETBIT(4),

    /**
     * \brief   Clear Transmit interrupt
     */
    ICR_TX = SETBIT(5),

    /**
     * \brief   Clear all interrupts
     */
    ICR_ALL = 0x7ff,
};

static bool pl011_read_ready (volatile pl011_t * uart)
{
    return (uart->FR & FR_RXFE) == 0;
}

static uint8_t pl011_blocking_read (volatile pl011_t * uart)
{
    while (!pl011_read_ready(uart));
    return uart->DR;
}

static bool pl011_write_ready (volatile pl011_t * uart)
{
    return (uart->FR & FR_TXFF) == 0;
}

static void pl011_blocking_write (volatile pl011_t * uart, uint8_t c)
{
    while (!pl011_write_ready(uart));
    uart->DR = c;
}

char my_toupper (char c)
{
    if (c >= 'a' && c <= 'z') {
        return c + ('A' - 'a');
    }
    else if (c >= 'A' && c <= 'Z') {
        return c;
    }
    else if (c == '\r' || c == '\n') {
        return c;
    }
    else {
        return '?';
    }
}

void pl011_isr (volatile pl011_t * uart, InterruptHandler_t irq_id)
{
    uint32_t mis = uart->MIS;

    if (mis & MIS_RX) {
        while (pl011_read_ready(uart)) {
            uint8_t payload;

            payload = pl011_blocking_read(uart);
            pl011_blocking_write(uart, payload);
        }
        uart->ICR = ICR_RX;
    }

    if (mis & MIS_TX) {
    }

    InterruptComplete(irq_id);
}

int main (int argc, char *argv[]) {

    volatile pl011_t * uart0;
    InterruptHandler_t irq_id;
    int chid;
    int coid;

    unsigned int i;

    chid = ChannelCreate();
    coid = Connect(SELF_PID, chid);

    uart0 = MapPhysical(VERSATILE_UART0_BASE, PL011_MMAP_SIZE);

    char buf[64];

    for (i = 0; i < 10; i++) {
        snprintf(buf, sizeof(buf), "UART0 IMSC bit %d: %s\n", i, (uart0->IMSC & SETBIT(i)) != 0 ? "set" : "unset");
        unsigned int j;
        for (j = 0 ; j < strlen(buf); j++) {
            pl011_blocking_write(uart0, buf[j]);
        }
    }

    for (i = 0; i < 16; i++) {
        snprintf(buf, sizeof(buf), "UART0 CR bit %d: %s (setbit %d: 0x%x)\n", i, (uart0->CR & SETBIT(i)) != 0 ? "set" : "unset", i, SETBIT(i));
        unsigned int j;
        for (j = 0; j < strlen(buf); j++) {
            pl011_blocking_write(uart0, buf[j]);
        }
    }

    for (i = 0; i < 11; i++) {
        snprintf(buf, sizeof(buf), "UART0 RIS bit %d: %s\n", i, (uart0->RIS & SETBIT(i)) != 0 ? "set" : "unset");
        unsigned int j;
        for (j = 0; j < strlen(buf); j++) {
            pl011_blocking_write(uart0, buf[j]);
        }
    }

    static const char message[] = "Hello, World\n";

    for (i = 0; i < strlen(message); i++) {
        pl011_blocking_write(uart0, message[i]);
    }

    // Clear all pending interrupts
    uart0->ICR = ICR_ALL;

    // Enable interrupts
    uart0->IMSC = (IMSC_RX | IMSC_TX);

    irq_id = InterruptAttach(coid, VERSATILE_UART0_IRQ, NULL);

    // Main interrupt-handling loop
    for (;;) {
        int msgid;
        int num = MessageReceive(chid, &msgid, NULL, 0);

        if (msgid > 0) {
            MessageReply(msgid, ERROR_NO_SYS, NULL, 0);
        }
        else if (msgid < 0) {
            MessageReply(msgid, ERROR_NO_SYS, NULL, 0);
        }
        else {
            // Pulse received
            pl011_isr(uart0, irq_id);
            num = num;
        }
    }

    // Disable interrupts
    uart0->IMSC &= ~(IMSC_RX | IMSC_TX);

    InterruptDetach(irq_id);

    while (true) {
        uint8_t c = pl011_blocking_read(uart0);
        pl011_blocking_write(uart0, my_toupper(c));
    }

    return 0;
}