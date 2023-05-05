#include "mold.h"

namespace mold::macho {

// .debug_info contains variable-length fields. This class reads them.
template <typename E>
class DebugInfoReader {
public:
  DebugInfoReader(Context<E> &ctx, ObjectFile<E> &file, u8 *cu)
    : ctx(ctx), file(file), cu(cu) {}

  u64 read(u64 form);

  Context<E> &ctx;
  ObjectFile<E> &file;
  u8 *cu;
};

// Read value of the given DW_FORM_* form. If a value is not scalar,
// returns a dummy value 0.
template <typename E>
u64 DebugInfoReader<E>::read(u64 form) {
  switch (form) {
  case DW_FORM_flag_present:
    return 0;
  case DW_FORM_block1:
  case DW_FORM_data1:
  case DW_FORM_flag:
  case DW_FORM_strx1:
  case DW_FORM_addrx1:
  case DW_FORM_ref1:
    return *cu++;
  case DW_FORM_block2:
  case DW_FORM_data2:
  case DW_FORM_strx2:
  case DW_FORM_addrx2:
  case DW_FORM_ref2: {
    u64 val = *(ul16 *)cu;
    cu += 2;
    return val;
  }
  case DW_FORM_strx3:
  case DW_FORM_addrx3: {
    u64 val = *(ul24 *)cu;
    cu += 3;
    return val;
  }
  case DW_FORM_block4:
  case DW_FORM_data4:
  case DW_FORM_strp:
  case DW_FORM_sec_offset:
  case DW_FORM_line_strp:
  case DW_FORM_strx4:
  case DW_FORM_addrx4:
  case DW_FORM_ref4: {
    u64 val = *(ul32 *)cu;
    cu += 4;
    return val;
  }
  case DW_FORM_data8:
  case DW_FORM_ref8: {
    u64 val = *(ul64 *)cu;
    cu += 8;
    return val;
  }
  case DW_FORM_addr:
  case DW_FORM_ref_addr: {
    u64 val = *(Word<E> *)cu;
    cu += sizeof(Word<E>);
    return val;
  }
  case DW_FORM_block:
  case DW_FORM_strx:
  case DW_FORM_addrx:
  case DW_FORM_udata:
  case DW_FORM_ref_udata:
  case DW_FORM_loclistx:
  case DW_FORM_rnglistx:
    return read_uleb(cu);
  case DW_FORM_string:
    cu += strlen((char *)cu) + 1;
    return 0;
  default:
    Fatal(ctx) << file << ": unhandled debug info form: 0x" << std::hex << form;
    return 0;
  }
}

// Try to find a compilation unit from .debug_info and its
// corresponding record from .debug_abbrev and returns them.
template <typename E>
static std::tuple<u8 *, u8 *, u32>
find_compunit(Context<E> &ctx, ObjectFile<E> &file) {
  // Read .debug_info to find the record at a given offset.
  u8 *cu = file.debug_info;
  u32 dwarf_version = *(ul16 *)(cu + 4);
  u32 abbrev_offset;

  // Skip a header.
  switch (dwarf_version) {
  case 2:
  case 3:
  case 4:
    abbrev_offset = *(ul32 *)(cu + 6);
    if (u32 address_size = cu[10]; address_size != sizeof(Word<E>))
      Fatal(ctx) << file << ": unsupported DWARF address size " << address_size;
    cu += 11;
    break;
  case 5: {
    abbrev_offset = *(ul32 *)(cu + 8);
    if (u32 address_size = cu[7]; address_size != sizeof(Word<E>))
      Fatal(ctx) << file << ": unsupported DWARF address size " << address_size;

    switch (u32 unit_type = cu[6]; unit_type) {
    case DW_UT_compile:
    case DW_UT_partial:
      cu += 12;
      break;
    case DW_UT_skeleton:
    case DW_UT_split_compile:
      cu += 20;
      break;
    default:
      Fatal(ctx) << file << ": unknown DWARF DW_UT_* value: 0x"
                 << std::hex << unit_type;
    }
    break;
  }
  default:
    Fatal(ctx) << file << ": unknown DWARF version: " << dwarf_version;
  }

  u32 abbrev_code = read_uleb(cu);

  // Find a .debug_abbrev record corresponding to the .debug_info record.
  // We assume the .debug_info record at a given offset is of
  // DW_TAG_compile_unit which describes a compunit.
  u8 *abbrev = file.debug_abbrev + abbrev_offset;

  for (;;) {
    u32 code = read_uleb(abbrev);
    if (code == 0)
      Fatal(ctx) << file << ": .debug_abbrev does not contain"
                 << " a record for the first .debug_info record";

    if (code == abbrev_code) {
      // Found a record
      u64 abbrev_tag = read_uleb(abbrev);
      if (abbrev_tag != DW_TAG_compile_unit && abbrev_tag != DW_TAG_skeleton_unit)
        Fatal(ctx) << file << ": the first entry's tag is not"
                   << " DW_TAG_compile_unit/DW_TAG_skeleton_unit but 0x"
                   << std::hex << abbrev_tag;
      break;
    }

    // Skip an uninteresting record
    read_uleb(abbrev); // tag
    abbrev++; // has_children byte
    for (;;) {
      u64 name = read_uleb(abbrev);
      u64 form = read_uleb(abbrev);
      if (name == 0 && form == 0)
        break;
      if (form == DW_FORM_implicit_const)
        read_uleb(abbrev);
    }
  }

  abbrev++; // skip has_children byte
  return {cu, abbrev, dwarf_version};
}

template <typename E>
std::string_view get_source_filename(Context<E> &ctx, ObjectFile<E> &file) {
  assert(file.debug_info);

  u8 *cu;
  u8 *abbrev;
  u32 dwarf_version;
  std::tie(cu, abbrev, dwarf_version) = find_compunit(ctx, file);

  DebugInfoReader<E> reader{ctx, file, cu};

 for (;;) {
    u64 name = read_uleb(abbrev);
    u64 form = read_uleb(abbrev);
    if (name == 0 && form == 0)
      break;

    u64 val = reader.read(form);

    if (name == DW_AT_name) {
      switch (form) {
      case DW_FORM_strp:
        return (char *)(file.debug_str + val);
      case DW_FORM_line_strp:
        return (char *)(file.debug_line + val);
      default:
        Fatal(ctx) << file << ": unknown DWARF form for DW_AT_name: 0x"
                   << std::hex << form;
      }
    }
  }

  return "";
}

using E = MOLD_TARGET;

template std::string_view get_source_filename(Context<E> &, ObjectFile<E> &);

} // namespace mold::macho
