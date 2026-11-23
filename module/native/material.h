#pragma once
#include "resolver.hpp"
#include "../../adapters/transitions/capture_layout.h"
namespace hg {
void set_material_density(float density);
void set_home_material_ready(bool ready);
void set_workspace_scrolling(bool scrolling);
unsigned queue_material_target(unsigned kind,unsigned x,unsigned y,const HgCaptureTransform&transform);
bool install_material_hooks(uintptr_t base,const Program&program,const Resolved&result);
bool install_backdrop_stream_hooks();
bool crop_export_image(uintptr_t peer,unsigned x,unsigned y,unsigned width,unsigned height);
uintptr_t image_peer(uintptr_t image,uintptr_t heap);
}
