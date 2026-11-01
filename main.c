/* main.c - titik masuk kernel 6S: inisialisasi semua komponen lalu jalankan shell */
#include "kernel.h"

void shell_objects_init(void);

/* Logo ASCII: huruf "O" pada "OS" diganti angka 6  =>  6S  (Operating Six) */
static const char *logo[] = {
    "  __     ____  ",
    " / /_   / ___| ",
    "| '_ \\  \\___ \\ ",
    "| (_) |  ___) |",
    " \\___/  |____/ ",
};

void kmain(void) {
    cons_init();
    for (int i = 0; i < 5; i++) { kputs(logo[i]); kputc('\n'); }
    kprintf("\n  6S - Operating Six  |  kernel x86  |  Round Robin preemptive\n\n");
    gdt_init();   kprintf("[ OK ] GDT dimuat\n");
    idt_init();   kprintf("[ OK ] IDT dimuat\n");
    pic_init();   kprintf("[ OK ] PIC diatur ulang\n");
    mem_init();   kprintf("[ OK ] Alokator memori (heap %u KB)\n", HEAP_SIZE / 1024);
    sched_init(); shell_objects_init(); kprintf("[ OK ] Thread manager + scheduler (quantum %d ms)\n", quantum_ticks);
    fs_init();    kprintf("[ OK ] RAMDisk\n");
    pit_init(1000); kprintf("[ OK ] Timer 1000 Hz\n");
    __asm__ volatile("sti");                       /* aktifkan interupsi: preemption dimulai */
    kprintf("\nLogin sebagai root. Ketik 'masuk guest' untuk jadi user biasa.\n");
    kprintf("Ketik 'bantuan' atau 'skenario' untuk memulai.\n\n");
    shell_main();
}
