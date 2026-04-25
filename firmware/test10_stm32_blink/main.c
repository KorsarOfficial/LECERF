/* STM32F103 Blue Pill blink LED on PC13 (active low).
   Uses RCC + GPIOC + USART1 — same code that would run on real Blue Pill. */

#define RCC_APB2ENR  (*(volatile unsigned*)0x40021018)
#define GPIOC_CRH    (*(volatile unsigned*)0x40011004)
#define GPIOC_BSRR   (*(volatile unsigned*)0x40011010)

#define USART1_DR    (*(volatile unsigned*)0x40013804)
#define USART1_SR    (*(volatile unsigned*)0x40013800)

static void putch(char c) {
    while (!(USART1_SR & (1 << 7))) {}
    USART1_DR = (unsigned)c;
}
static void puts_(const char* s) { while (*s) putch(*s++); }

static volatile unsigned blinks = 0;

int main(void) {
    /* Enable IOPC + USART1 */
    RCC_APB2ENR |= (1 << 4) | (1 << 14);
    /* PC13 as 2 MHz output, push-pull: CNF=00, MODE=10 → bits[23:20]=0010 */
    GPIOC_CRH = (GPIOC_CRH & ~(0xF << 20)) | (0x2 << 20);

    puts_("STM32 blink demo\n");

    for (int i = 0; i < 5; ++i) {
        GPIOC_BSRR = (1 << (13 + 16));   /* reset (LED on, active low) */
        for (volatile int d = 0; d < 100; ++d) {}
        GPIOC_BSRR = (1 << 13);          /* set (LED off) */
        for (volatile int d = 0; d < 100; ++d) {}
        blinks++;
    }
    puts_("done\n");

    __asm__ volatile (
        "mov r0, %0\n"
        ".short 0xDEFE\n"
        :: "r"(blinks) : "r0"
    );
    return 0;
}
