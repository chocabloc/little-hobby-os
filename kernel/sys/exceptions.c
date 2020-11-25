#include "drivers/fbcon/fbcon.h"
#include <stdbool.h>
#include <stdint.h>

static char* exceptions[] = {
    [0] = "Division by Zero",
    [1] = "Debug",
    [2] = "Non Maskable Interrupt",
    [3] = "Breakpoint",
    [4] = "Overflow",
    [5] = "Bound Range Exceeded",
    [6] = "Invalid opcode",
    [7] = "Device not available",
    [8] = "Double Fault",
    [10] = "Invalid TSS",
    [11] = "Segment not present",
    [12] = "Stack Exception",
    [13] = "General Protection fault",
    [14] = "Page fault",
    [16] = "x87 Floating Point Exception",
    [17] = "Alignment check",
    [18] = "Machine check",
    [19] = "SIMD floating point Exception",
    [20] = "Virtualization Exception",
    [30] = "Security Exception"
};

void exc_handler(uint64_t errcode, uint64_t excno)
{
    fbcon_setfgcolor(0x00ff6666);
    fbcon_puts("\nFatal exception occured: ");
    fbcon_setfgcolor(FBCON_COLOR_GRAY);
    fbcon_puts(exceptions[excno]);
    fbcon_puts(", Error Code: ");
    fbcon_puthex(errcode);
    fbcon_puts(". Halting...");

    while (true)
        ;
}