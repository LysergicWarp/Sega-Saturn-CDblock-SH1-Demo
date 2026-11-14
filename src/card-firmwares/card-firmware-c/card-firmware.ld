/* The image at the start of the expansion area, where the expansion loader decodes it: the
   header first, then the code and every variable, the zeroed ones included, so that each load
   starts them afresh. The stacks follow, outside the image. All of it ends before the module slot
   at 0x0907c000. sh-elf names C symbols with a leading underscore. */
ENTRY(_header)
SECTIONS
{
    .image 0x0907b000 :
    {
        KEEP(*(.header))
        _image_code = .;
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.* COMMON)
        . = ALIGN(8);
        _image_end = .;     /* the encoder takes whole eight-byte blocks */
    }
    .stacks (NOLOAD) : { *(.stacks) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(ADDR(.stacks) + SIZEOF(.stacks) <= 0x0907c000, "the image and stacks reach the module slot")
