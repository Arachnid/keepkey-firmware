MEMORY
{
    /* Bootloader */
    rom (rx) : ORIGIN = 0x08020000, LENGTH = 0x00040000 

    /* RAM */
    ram (rwx)    : ORIGIN = 0x20000000, LENGTH = 0x00020000
}



INCLUDE libopencm3_stm32f2.ld
