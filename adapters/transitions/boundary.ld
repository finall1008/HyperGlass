SECTIONS {
 .message 0x1868e9c : { *(.message) }
 .selector 0x2006320 : { *(.selector) }
 .initialize 0x2006360 : { *(.initialize) }
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
message_return = 0x1868ef4;
barrier = 0x1cea348;
wrap_message = 0x1867c88;
boundary_build = 0x15f14f0;
boundary_return = 0x15f1514;

hg_capture_config = 0x2003600;
