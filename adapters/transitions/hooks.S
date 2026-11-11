.section .limit_test,"ax"
.global limit_test
limit_test:
 mov x3,x29
 mov x8,#80
 adrp x9,scene_built
 add x9,x9,:lo12:scene_built
frame_loop:
 ldr x4,[x3,#8]
 ldr x5,[x3]
 cmp x4,x9
 b.eq found
 cmp x5,x3
 b.ls no
 sub x4,x5,x3
 cmp x4,#16,lsl #12
 b.hi no
 mov x3,x5
 subs x8,x8,#1
 b.ne frame_loop
 b no
found:
 ldr x4,[x5,#8]
 adrp x9,async_scene_return
 add x9,x9,:lo12:async_scene_return
 cmp x4,x9
 b.ne no
 ldr x5,[x5]
 // Only full_scene's marked SF export may alter ancestor rendering.
 ldur x4,[x5,#-0x88]
 ldur x4,[x4,#7]
 mov x9,#0x4130000000000000 // META_BASE as a positive double.
 cmp x4,x9
 b.lo no
 ldur x6,[x5,#-0x80]
 lsl x7,x28,#32
 mov x8,#80
layer_loop:
 cmp w6,w22
 b.eq no
 cmp x6,x0
 b.eq yes
 ldur w6,[x6,#0x1f]
 add x6,x6,x7
 subs x8,x8,#1
 b.ne layer_loop
no:
 mov x16,#0
 ret
yes:
 // Depth above the captured layer plus one; zero means off the capture path.
 mov x16,#81
 sub x16,x16,x8
 ret
.section .limit_hooks,"ax"
.global force_path,stop_after_path,refresh_path
force_path:
 bl limit_test
 cbz x16,force_replay
 add x17,x22,#0x20
 stur w17,[x0,#0x23]
force_replay:
 stur x0,[x29,#-0x18]
 b first_child_continue
stop_after_path:
 bl limit_test
 cbz x16,next_replay
 mov x2,x22
 b next_child_continue
next_replay:
 add x2,x2,x28,lsl #32
 b next_child_continue
refresh_path:
 mov x16,#6
 bl capture_bridge
 mov x15,x29
 ldp x29,x30,[x15],#16
 ret
.global capture_image_filter
capture_image_filter:
 mov x12,x30
 mov x0,x1
 bl limit_test
 mov x30,x12
 cbz x16,image_replay
 b draw_children
image_replay:
 stp x29,x30,[x15,#-16]!
 b image_filter_continue
.global root_gateway,mark_floor,mark_ceil,result_gateway,bytes_gateway,clone_pack,clone_unpack
root_gateway:
 mov x16,#0
 bl capture_bridge
 ldr x4,[x27,#0x70]
 b push_transform_call
mark_floor:
 mov x16,#1
 bl capture_bridge
 stur x0,[x29,#-0x90]
 b mark_floor_continue
mark_ceil:
 mov x16,#1
 bl capture_bridge
 stur x0,[x29,#-0x88]
 b mark_ceil_continue
result_gateway:
 mov x16,#2
 bl capture_bridge
 b result_continue
bytes_gateway:
 mov x16,#3
 bl capture_bridge
 mov x17,#-0x168
 b bytes_continue
clone_pack:
 mov x16,#4
 bl capture_bridge
 stur x2,[x29,#-0x10]
 b clone_pack_continue
clone_unpack:
 mov x16,#5
 bl capture_bridge
 stur x1,[x3,#0xf]
 b clone_unpack_continue
.section .text_color,"ax"
.global android_image_plate
android_image_plate:
 mov x12,x30
 mov x16,#8
 bl capture_bridge
 mov x30,x12
 cbz x16,android_image_original
 // Reuse the launcher's canonical empty placeholder. The resolver replays
 // both current pool-load instructions; no allocation or nullable result.
.global android_empty_widget
android_empty_widget:
 add x0,x27,#0x46,lsl #12
 ldr x0,[x0,#0xf88]
 ret
android_image_original:
 stp x29,x30,[x15,#-16]!
 b android_image_continue
.global android_text_inline,android_spanned_text,android_hint_text,maml_text_color,maml_filter_color
android_text_inline:
 // The existing frame boxes the Color at its original allocation PC.
 // Retain provider alpha, and normalize the foreground to white.
 orr w4,w4,#0xffffff
 stur x4,[x29,#-0x30]
 b android_text_continue
android_spanned_text:
 orr w3,w3,#0xffffff
 stur x3,[x29,#-0x28]
 b android_spanned_continue
android_hint_text:
 orr w0,w0,#0xffffff
 stur x0,[x29,#-0x58]
 b android_hint_continue
maml_text_color:
 fmov d0,#1.0
 stur d0,[x0,#0xf]
 stur d0,[x0,#0x17]
 stur d0,[x0,#0x1f]
 mov x4,x0
 b maml_text_continue
maml_filter_color:
 fmov d0,#1.0
 stur d0,[x0,#0xf]
 stur d0,[x0,#0x17]
 stur d0,[x0,#0x1f]
 mov x3,x0
 b maml_filter_continue
.section .widget_helpers,"ax"
.global android_image_opacity
android_image_opacity:
 // _applyImageAlphaIfNeeded. Provider pictures (bitmap builders and Uri
 // drawables) show the glass through them at 0.6 of their own alpha via the
 // function's Opacity wrapper; the allocation and its return PC stay in the
 // original Dart frame. Resource drawables keep their alpha.
 ldr x17,[x29,#8]
 adrp x16,center_crop_alpha_return
 add x16,x16,:lo12:center_crop_alpha_return
 cmp x17,x16
 b.eq opacity_photo
 adrp x16,fit_scaled_alpha_return
 add x16,x16,:lo12:fit_scaled_alpha_return
 cmp x17,x16
 b.eq opacity_photo
 adrp x16,bitmap_size_alpha_return
 add x16,x16,:lo12:bitmap_size_alpha_return
 cmp x17,x16
 b.eq opacity_photo
 adrp x16,drawable_alpha_return
 add x16,x16,:lo12:drawable_alpha_return
 cmp x17,x16
 b.ne opacity_plain
 ldr x17,[x29]
 ldr x17,[x17,#8]
 adrp x16,future_drawable_return
 add x16,x16,:lo12:future_drawable_return
 cmp x17,x16
 b.eq opacity_photo
opacity_plain:
 cmp w0,w22
 b.ne opacity_value_replay
 b image_opacity_none
opacity_photo:
 mov x16,#0x3333
 movk x16,#0x3333,lsl #16
 movk x16,#0x3333,lsl #32
 movk x16,#0x3fe3,lsl #48
 fmov d1,x16
 cmp w0,w22
 b.eq opacity_store
 ldur d2,[x0,#7]
 fcmp d2,#0.0
 b.le opacity_value_replay
 b.vs opacity_value_replay
 fmul d1,d1,d2
opacity_store:
 stur d1,[x29,#-0x10]
 b image_opacity_wrap
opacity_value_replay:
 b image_opacity_value
.section .text_color,"ax"
.global android_background
android_background:
 // Hide container backgrounds only from the shared paint-building phase.
 // Padding and outline queries keep the original provider drawable.
 adrp x16,android_paint_begin
 add x16,x16,:lo12:android_paint_begin
 cmp x30,x16
 b.lo android_background_original
 adrp x16,android_paint_end
 add x16,x16,:lo12:android_paint_end
 cmp x30,x16
 b.hs android_background_original
 ldur x16,[x1,#-1]
 ubfx x16,x16,#12,#20
#ifdef HG_ADAPTIVE
 adrp x17,hg_capture_config
 ldr w17,[x17,:lo12:hg_capture_config+28]
#else
 mov x17,#0x21fa
#endif
 sub x16,x16,x17
#ifdef HG_ADAPTIVE
 adrp x17,hg_capture_config
 ldr w17,[x17,:lo12:hg_capture_config+32]
 cmp x16,x17
#else
 cmp x16,#13
#endif
 b.hi android_background_original
 mov x0,x22
 ret
android_background_original:
 ldur w2,[x1,#0xb]
 b android_background_continue
.section .capture_root,"ax"
.global source_controller_tag,source_tag_hero
source_controller_tag:
 // Resolve recommendation copies by their flight identity. Other containers
 // retain their canonical controller tags, which can omit the Hero prefix.
 b source_tag_hero
source_tag_canonical:
 mov x16,x30
 adrp x30,source_tag_return
 add x30,x30,:lo12:source_tag_return
 br x16
source_tag_hero:
 mov x17,#-0x128
 ldr x3,[x29,x17]
 mov x10,x0
.global source_tag_prefix
source_tag_prefix:
 // Replay the recommendation prefix selected by HeroTagResolver.getPrefix.
 add x0,x27,#0xb,lsl #12
 ldr x0,[x0,#0x158]
 mov x16,x0
 mov x0,x10
 ldur w17,[x3,#7]
 add x17,x17,x28,lsl #32
 ldur w12,[x16,#7]
 ldur w13,[x17,#7]
 cmp w13,w12
 b.lo source_tag_canonical
 lsr w12,w12,#1
 cbz w12,source_tag_canonical
 add x16,x16,#15
 add x17,x17,#15
source_tag_compare:
 ldrb w13,[x16],#1
 ldrb w14,[x17],#1
 cmp w13,w14
 b.ne source_tag_canonical
 subs w12,w12,#1
 b.ne source_tag_compare
 adrp x30,source_tag_return
 add x30,x30,:lo12:source_tag_return
 // Match getcomponentNameString's frame and preserve its model/component roots.
 // Every allocating call and its return PC stays in the original Dart code.
 stp x29,x30,[x15,#-16]!
 mov x29,x15
 sub x15,x15,#0x20
 stur x1,[x29,#-0x18]
 stur x3,[x29,#-0x10]
 b source_tag_tail
// Offscreen layers on the capture path would hide the wallpaper from the
// target's backdrop sampling (gesture return fades and tints the workspace).
.global capture_color_filter,capture_shader_mask,capture_opacity
capture_color_filter:
 adrp x17,color_filter_continue
 add x17,x17,:lo12:color_filter_continue
 b skip_on_path
capture_shader_mask:
 adrp x17,shader_mask_continue
 add x17,x17,:lo12:shader_mask_continue
skip_on_path:
 mov x12,x30
 mov x0,x1
 bl limit_test
 mov x30,x12
 cbz x16,skip_replay
 b draw_children
skip_replay:
 stp x29,x30,[x15,#-16]!
 br x17
capture_opacity:
 // Flags hold alpha compared with 255; on the path the offset-only branch is used.
 b.lt opacity_translucent
 b opacity_offset
opacity_translucent:
 mov x0,x3
 bl limit_test
 ldur x3,[x29,#-8]
 cbz x16,opacity_replay
 b opacity_offset
opacity_replay:
 b opacity_continue

.section .capture_root,"ax"
.global capture_transform
capture_transform:
 mov x12,x30
 mov x0,x1
 bl limit_test
 mov x30,x12
 cbz x16,transform_replay
 mov x16,#7
 bl capture_bridge
 mov x30,x12
 cbz x16,transform_replay
 b draw_children
transform_replay:
 stp x29,x30,[x15,#-16]!
 b transform_continue
