/* Disc extension probe at the start of the disc extension area, where the loader decodes it. Its
   counters and vector table sit above the decoded image, so the code must end well below them. */
ENTRY(probe_start)
SECTIONS
{
    .text 0x09075800 : { *(.text) }
    /DISCARD/ : { *(.data) *(.bss) *(.comment) }
}
ASSERT(probe_entry == 0x09075848, "entry code must follow the 0x48-byte loader header")
ASSERT(probe_end <= 0x09076000, "probe too large")
