/* Firmware bootstrap at the start of the disc extension area, where the loader decodes it. The
   card firmware's image it carries must be whole longwords and end before the module slot once
   copied to 0x0907b000, and the whole decoded payload must stay well inside the area, which ends at
   0x09077bff. */
ENTRY(boot_start)
SECTIONS
{
    .text 0x09075800 : { *(.text) }
    /DISCARD/ : { *(.data) *(.bss) *(.comment) }
}
ASSERT(boot_entry == 0x09075848, "entry code must follow the 0x48-byte loader header")
ASSERT((boot_image_end - boot_image) % 4 == 0, "firmware image not whole longwords")
ASSERT(boot_image_end - boot_image <= 0x1000, "firmware image reaches the module slot")
ASSERT(boot_end <= 0x09077800, "bootstrap too large for the disc extension area")
