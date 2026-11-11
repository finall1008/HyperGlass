SECTIONS {
 .text_color 0x98bdc0 : { *(.text_color) }
 .capture_root 0xb32838 : { *(.capture_root) }
 .widget_helpers 0xb33300 : { *(.widget_helpers) }
 .capture_config 0xb33600 : { *(.capture_config) }
 .capture_meta 0xce2770 : { *(.capture_meta) *(.text*) *(.rodata*) }
 .hooks 0x11f30a0 : { *(.limit_hooks) }
 .capture_small 0x11f3180 : { *(.capture_small) }
 .limit_test 0x18113a0 : { *(.limit_test) }
 .bridge 0x19bfdb0 : { *(.bridge) }
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
async_scene_return = 0x191a61c;
scope_widget = 0x19b0dc8;
scope_precreate = 0x19beef0;
scope_close = 0x1b136bc;
scope_widget_snapshot = 0x199c894;
scene_built = 0xa02928;
first_child_continue = 0x1562384;
next_child_continue = 0x1562438;
draw_children = 0x1562368;
image_filter_continue = 0x1562128;
color_filter_continue = 0x156376c;
shader_mask_continue = 0x1563974;
opacity_continue = 0x15629c8;
opacity_offset = 0x1562a44;
push_transform_call = 0xa02918;
mark_floor_continue = 0x191a750;
mark_ceil_continue = 0x191a8bc;
result_continue = 0x19c0058;
bytes_continue = 0x1b1519c;
clone_pack_continue = 0xa1e60c;
clone_unpack_continue = 0xa1e638;
android_text_continue = 0x10e5e04;
android_spanned_continue = 0xc42d88;
android_hint_continue = 0x10e379c;
maml_text_continue = 0x164257c;
maml_filter_continue = 0x163e20c;
android_image_continue = 0x10d4bc4;
android_background_continue = 0x173d858;
android_paint_begin = 0x10b85d0;
center_crop_alpha_return = 0x10c69f0;
fit_scaled_alpha_return = 0x10c5778;
bitmap_size_alpha_return = 0x10cfff0;
drawable_alpha_return = 0x10d367c;
future_drawable_return = 0x10cdac0;
image_opacity_value = 0x10c58dc;
image_opacity_none = 0x10c58cc;
image_opacity_wrap = 0x10c5940;
android_paint_end = 0x10b9534;


transform_continue = 0x1562734;

scope_current_icon = 0x19bf198;
scope_window_icon = 0x1b1495c;
scope_maml_icon = 0x19c08e4;
scope_window_maml = 0x1b15874;
source_tag_return = 0x1b14738;
source_tag_tail = 0x11f8168;
