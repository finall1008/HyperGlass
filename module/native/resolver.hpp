#pragma once
#include "elf_image.hpp"
#include "runtime.h"
#include <array>
#include <string>
#include <vector>
namespace hg {
struct RegisterRelation { uint8_t left,left_shift,right,right_shift; };
struct Anchor {
 enum Mode : uint8_t { NamedCall=6, OperandSequence, BranchDestination, RelativeSequence, SceneFloor, SceneCeil };
 const char *owner;
 uint32_t source;
 uint8_t before,count,mode;
 int32_t dependency,qualifier;
 const char *callee;
 int32_t delta;
 const uint32_t *values,*masks;
 const RegisterRelation *relations=nullptr;
 uint8_t relation_count=0;
 bool follow_helpers=false;
};
struct Target { uint32_t source; int32_t anchor; uint8_t kind; };
struct Relocation { uint16_t offset; uint8_t kind; Target target; uint32_t original; };
struct Unit {
 const char *label;uint32_t source,size;int32_t anchor;uint8_t group,shadow;
 const uint8_t *bytes;const Relocation *relocations;uint16_t relocation_count;
 int32_t guard=-1;bool when_guard_matches=true;
};
struct Config { uint8_t slot,kind;int16_t subtract_slot;int32_t anchor;uint32_t source_tag; };
// VirtualSlot decodes a validated vtable load; delta selects Resolved::virtual_slots.
enum class ConstraintKind : uint8_t { Equal, Minimum, VirtualSlot };
struct Constraint { uint32_t group;int32_t left,right;int32_t delta;ConstraintKind kind=ConstraintKind::Equal; };
struct WidgetWrap {
 size_t unit;
 int32_t call_anchor,filter_anchor,image_filter_anchor;
 uint32_t compound,dispatch,resume;
 uint32_t image_filter;
};
struct Program {
 const char *image;const Anchor *anchors;size_t anchor_count;
 const Unit *units;size_t unit_count;
 const Config *config;size_t config_count;uint32_t config_source;
 const uint32_t *groups;size_t group_count;
 const Constraint *constraints;size_t constraint_count;
 const WidgetWrap *widget_wrap=nullptr;
};
using Patch = hg_patch;
struct MamlRepaintLayout {
 uint32_t field_table=0, tag_map=0, binding=0, build_owner=0, element_map=0, string_class=0;
};
// Element._retakeInactiveElement guard. A Hero flight inflates copies of an
// icon's subtree outside the attached ShortcutIconWidget; offsets and the
// build selector come from the running ELF.
struct IconRetakeLayout {
 uint32_t parent=0, widget=0, key=0, controller=0, sentinel=0, capture_key=0, build_selector=0;
 uintptr_t icon_build=0;
 uint32_t owner=0;
};
struct Resolved {
 std::vector<Patch> patches;
 uintptr_t shadow=0;size_t shadow_size=0;uint32_t ready=0;
 uint32_t adapted_widget_paths=0;
 std::vector<uint32_t> addresses;
 std::vector<std::pair<uint32_t,uintptr_t>> adapter_pages;
 MamlRepaintLayout maml_repaint;
 IconRetakeLayout icon_retake;
 // Engine vtable byte offsets read from the running ELF's call sites.
 std::array<uint32_t,2> virtual_slots{};
 // Live Dart_CreateIsolateGroup entry when the image defines it.
 uintptr_t isolate_group_entry=0;
 std::string diagnostic;
};
class Locator {
 public:
  Locator(const ElfImage &image,const Program &program,const std::array<uint64_t,2>&scene_marks):image_(image),program_(program),scene_marks_(scene_marks),resolved_(program.anchor_count,-1),candidates_(program.anchor_count,0){}
  uint32_t resolve(size_t index);
  unsigned candidates(size_t index)const{return candidates_[index];}
 private:
  const ElfImage &image_;const Program &program_;std::array<uint64_t,2> scene_marks_;
  std::vector<int64_t> resolved_;std::vector<unsigned>candidates_;
  void scan(const Anchor &anchor,uint64_t begin,uint64_t end,std::vector<uint32_t>&exact,std::vector<uint32_t>&elastic);
  void append_helpers(const char*owner,std::vector<Symbol>&functions) const;
  bool matches_sequence(const Anchor&anchor,uint64_t start) const;
};
uintptr_t adapter_address(const Resolved &result,uint32_t source);
bool resolve_program(const char *path,uintptr_t live_base,const Program &program,Resolved &out,bool allocate_shadow=true);
}
