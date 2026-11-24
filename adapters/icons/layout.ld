SECTIONS {
 .text.helper 0x2000120 : { *(.text.color) *(.text.blend) *(.text.distance) *(.text.finish) *(.text.OUTLINED_FUNCTION*) }
 .text 0x20078c4 : { *(.text) *(.text.clear_content) }
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
