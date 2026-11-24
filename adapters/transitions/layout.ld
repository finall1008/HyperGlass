SECTIONS {
 .text_color 0x2001dc0 : { *(.text_color) }
 .capture_root 0x2002838 : { *(.capture_root) }
 .widget_helpers 0x2003300 : { *(.widget_helpers) }
 .capture_config 0x2003600 : { *(.capture_config) }
 .capture_meta 0x2004770 : { *(.capture_meta) *(.text*) *(.rodata*) }
 .hooks 0x20050a0 : { *(.limit_hooks) }
 .capture_small 0x2005180 : { *(.capture_small) }
 .limit_test 0x20063a0 : { *(.limit_test) }
 .bridge 0x2008db0 : { *(.bridge) }
 .badge 0x2009000 : { *(.badge) }
 .widget_spans 0x2009040 : { *(.widget_spans) }
 .widget_wrap 0x2009080 : { *(.widget_wrap) }
 .widget_image_filter 0x2009100 : { *(.widget_image_filter) }
 .maml_repaint 0x2009140 : { *(.maml_repaint) }
 .material_scene 0x2009180 : { *(.material_scene) }
 .material_scroll 0x20091c0 : { *(.material_scroll) }
 .detached_capture 0x2009200 : { *(.detached_capture) }
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
async_scene_return = 0x1a7efa0;
scope_widget = 0x1b25bec;
scope_precreate = 0x1b481ac;
scope_close = 0x1c887e4;
scope_widget_snapshot = 0x1b18ac0;
scene_built = 0x9e2754;
first_child_continue = 0x16a3880;
next_child_continue = 0x16a3934;
draw_children = 0x16a3864;
image_filter_continue = 0x16a3624;
color_filter_continue = 0x16a4c70;
shader_mask_continue = 0x16a4e78;
opacity_continue = 0x16a3ec4;
opacity_offset = 0x16a3f40;
push_transform_call = 0x9e2744;
mark_floor_continue = 0x1a7f0d8;
mark_ceil_continue = 0x1a7f248;
result_continue = 0x1b493c4;
bytes_continue = 0x1c8a794;
clone_pack_continue = 0xa10560;
clone_unpack_continue = 0xa1058c;
android_text_continue = 0x105ca24;
android_spanned_continue = 0xd0ebb4;
span_direct_continue = 0xd0fae8;
wrap_factory_filter_call = 0x11f8fa4;
wrap_factory_resume = 0x11f8fac;
wrap_factory_child = 0x11f8fc0;
wrap_factory_set_state = 0xb113c0;
wrap_grayscale_matrix = 0x18a95d8;
android_hint_continue = 0x11b8550;
maml_text_continue = 0x180b6ec;
maml_filter_continue = 0x18072ac;
android_image_continue = 0x11971e8;
android_background_continue = 0x18885a4;
android_paint_begin = 0x1184418;
center_crop_alpha_return = 0x1199524;
fit_scaled_alpha_return = 0x1197fc4;
bitmap_size_alpha_return = 0x11a2c40;
drawable_alpha_return = 0x11a5f0c;
future_drawable_return = 0x11a0854;
image_opacity_value = 0x1196d10;
image_opacity_none = 0x1196d00;
image_opacity_wrap = 0x1196d74;
android_paint_end = 0x118526c;


transform_continue = 0x16a3c30;

scope_current_icon = 0x1b48494;
scope_window_icon = 0x1c89e44;
scope_maml_icon = 0x1b49f2c;
scope_window_maml = 0x1c8ae94;
scene_signal_send = 0x9ea88c;
source_tag_return = 0x1c89bcc;
source_tag_tail = 0x942184;

badge_text_continue = 0x1592d5c;
badge_background_continue = 0x1592e98;
