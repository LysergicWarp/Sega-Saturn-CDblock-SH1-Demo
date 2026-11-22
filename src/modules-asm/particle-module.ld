/* particle-module.s in the card firmware's module slot, where its load copies the image. The
   particles live in its BSS, after the image, which must fit the slot too. */
ENTRY(module_start)
SECTIONS
{
    .text 0x0907c000 : { *(.text) }
    /DISCARD/ : { *(.data) *(.bss) *(.comment) }
}
ASSERT(module_end + module_bss_bytes <= 0x09080000, "module image and BSS exceed the module slot")
