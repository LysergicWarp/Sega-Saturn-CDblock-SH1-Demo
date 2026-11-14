/* sanity-firmware.s at the start of the expansion area, where the expansion loader decodes it.
   The whole image is decoded there, so it must fit the area. */
ENTRY(resident_start)
SECTIONS
{
    .text 0x0907b000 : { *(.text) }
    /DISCARD/ : { *(.data) *(.bss) *(.comment) }
}
ASSERT(resident_end <= 0x0907fff8, "resident image exceeds the expansion area")
