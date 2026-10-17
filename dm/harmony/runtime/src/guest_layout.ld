/* Embedded guest-relative layout: three different permission regions, no RWX. */
SECTIONS {
  .xblr_guest_text ALIGN(0x4000) : {
    HIDDEN(__xblr_guest_base = .);
    KEEP(*(.xblr.guest.text))
    HIDDEN(__xblr_guest_text_end = .);
  }
  .xblr_guest_ro (__xblr_guest_base + 0x4000) : {
    KEEP(*(.xblr.guest.ro))
    HIDDEN(__xblr_guest_ro_end = .);
  }
  .xblr_guest_data (__xblr_guest_base + 0x8000) : {
    KEEP(*(.xblr.guest.data))
    HIDDEN(__xblr_guest_data_end = .);
  }
  ASSERT(SIZEOF(.xblr_guest_text) <= 0x4000, "guest text exceeds fixed region")
  ASSERT(SIZEOF(.xblr_guest_ro) == 32, "guest RO identity/layout mismatch")
  ASSERT(SIZEOF(.xblr_guest_data) == 32, "guest data layout mismatch")
}
INSERT AFTER .text;
