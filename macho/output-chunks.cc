#include "mold.h"
#include "../common/sha.h"

#include <shared_mutex>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_sort.h>

#ifndef _WIN32
# include <sys/mman.h>
#endif

namespace mold::macho {

template <typename E>
std::ostream &operator<<(std::ostream &out, const Chunk<E> &chunk) {
  out << chunk.hdr.get_segname() << "," << chunk.hdr.get_sectname();
  return out;
}

template <typename T>
static std::vector<u8> to_u8vec(T &data) {
  std::vector<u8> buf(sizeof(T));
  memcpy(buf.data(), &data, sizeof(T));
  return buf;
}

template <typename E>
static std::vector<u8> create_pagezero_cmd(Context<E> &ctx) {
  SegmentCommand<E> cmd = {};
  cmd.cmd = LC_SEGMENT_64;
  cmd.cmdsize = sizeof(cmd);
  strcpy(cmd.segname, "__PAGEZERO");
  cmd.vmsize = ctx.arg.pagezero_size;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_segment_cmd(Context<E> &ctx, OutputSegment<E> &seg) {
  i64 nsects = 0;
  for (Chunk<E> *sec : seg.chunks)
    if (!sec->is_hidden)
      nsects++;

  SegmentCommand<E> cmd = seg.cmd;
  cmd.cmdsize = sizeof(SegmentCommand<E>) + sizeof(MachSection<E>) * nsects;
  cmd.nsects = nsects;

  std::vector<u8> buf = to_u8vec(cmd);
  for (Chunk<E> *sec : seg.chunks)
    if (!sec->is_hidden)
      append(buf, to_u8vec(sec->hdr));
  return buf;
}

template <typename E>
static std::vector<u8> create_dyld_info_only_cmd(Context<E> &ctx) {
  DyldInfoCommand cmd = {};
  cmd.cmd = LC_DYLD_INFO_ONLY;
  cmd.cmdsize = sizeof(cmd);

  if (ctx.rebase && ctx.rebase->hdr.size) {
    cmd.rebase_off = ctx.rebase->hdr.offset;
    cmd.rebase_size = ctx.rebase->hdr.size;
  }

  if (ctx.bind && ctx.bind->hdr.size) {
    cmd.bind_off = ctx.bind->hdr.offset;
    cmd.bind_size = ctx.bind->hdr.size;
  }

  if (ctx.lazy_bind && ctx.lazy_bind->hdr.size) {
    cmd.lazy_bind_off = ctx.lazy_bind->hdr.offset;
    cmd.lazy_bind_size = ctx.lazy_bind->hdr.size;
  }

  if (ctx.export_.hdr.size) {
    cmd.export_off = ctx.export_.hdr.offset;
    cmd.export_size = ctx.export_.hdr.size;
  }
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_symtab_cmd(Context<E> &ctx) {
  SymtabCommand cmd = {};
  cmd.cmd = LC_SYMTAB;
  cmd.cmdsize = sizeof(cmd);
  cmd.symoff = ctx.symtab.hdr.offset;
  cmd.nsyms = ctx.symtab.hdr.size / sizeof(MachSym<E>);
  cmd.stroff = ctx.strtab.hdr.offset;
  cmd.strsize = ctx.strtab.hdr.size;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_dysymtab_cmd(Context<E> &ctx) {
  DysymtabCommand cmd = {};
  cmd.cmd = LC_DYSYMTAB;
  cmd.cmdsize = sizeof(cmd);
  cmd.ilocalsym = 0;
  cmd.nlocalsym = ctx.symtab.globals_offset;
  cmd.iextdefsym = ctx.symtab.globals_offset;
  cmd.nextdefsym = ctx.symtab.undefs_offset - ctx.symtab.globals_offset;
  cmd.iundefsym = ctx.symtab.undefs_offset;
  cmd.nundefsym = ctx.symtab.hdr.size / sizeof(MachSym<E>) - ctx.symtab.undefs_offset;
  cmd.indirectsymoff = ctx.indir_symtab.hdr.offset;
  cmd.nindirectsyms  = ctx.indir_symtab.hdr.size / ctx.indir_symtab.ENTRY_SIZE;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_dylinker_cmd(Context<E> &ctx) {
  static constexpr char path[] = "/usr/lib/dyld";

  i64 size = sizeof(DylinkerCommand) + sizeof(path);
  std::vector<u8> buf(align_to(size, 8));

  DylinkerCommand &cmd = *(DylinkerCommand *)buf.data();
  cmd.cmd = LC_LOAD_DYLINKER;
  cmd.cmdsize = buf.size();
  cmd.nameoff = sizeof(cmd);
  memcpy(buf.data() + sizeof(cmd), path, sizeof(path));
  return buf;
}

template <typename E>
static std::vector<u8> create_uuid_cmd(Context<E> &ctx) {
  UUIDCommand cmd = {};
  cmd.cmd = LC_UUID;
  cmd.cmdsize = sizeof(cmd);

  assert(sizeof(cmd.uuid) == sizeof(ctx.uuid));
  memcpy(cmd.uuid, ctx.uuid, sizeof(cmd.uuid));
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_build_version_cmd(Context<E> &ctx) {
  i64 size = sizeof(BuildVersionCommand) + sizeof(BuildToolVersion);
  std::vector<u8> buf(align_to(size, 8));

  BuildVersionCommand &cmd = *(BuildVersionCommand *)buf.data();
  cmd.cmd = LC_BUILD_VERSION;
  cmd.cmdsize = buf.size();
  cmd.platform = ctx.arg.platform;
  cmd.minos = ctx.arg.platform_min_version.encode();
  cmd.sdk = ctx.arg.platform_sdk_version.encode();
  cmd.ntools = 1;

  BuildToolVersion &tool = *(BuildToolVersion *)(buf.data() + sizeof(cmd));
  tool.tool = TOOL_MOLD;
  tool.version = parse_version(ctx, mold_version_string).encode();
  return buf;
}

template <typename E>
static std::vector<u8> create_source_version_cmd(Context<E> &ctx) {
  SourceVersionCommand cmd = {};
  cmd.cmd = LC_SOURCE_VERSION;
  cmd.cmdsize = sizeof(cmd);
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_main_cmd(Context<E> &ctx) {
  EntryPointCommand cmd = {};
  cmd.cmd = LC_MAIN;
  cmd.cmdsize = sizeof(cmd);
  cmd.entryoff = ctx.arg.entry->get_addr(ctx) - ctx.mach_hdr.hdr.addr;
  cmd.stacksize = ctx.arg.stack_size;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8>
create_load_dylib_cmd(Context<E> &ctx, DylibFile<E> &dylib) {
  i64 size = sizeof(DylibCommand) + dylib.install_name.size() + 1; // +1 for NUL
  std::vector<u8> buf(align_to(size, 8));

  auto get_type = [&] {
    if (dylib.is_reexported)
      return LC_REEXPORT_DYLIB;
    if (dylib.is_weak)
      return LC_LOAD_WEAK_DYLIB;
    return LC_LOAD_DYLIB;
  };

  DylibCommand &cmd = *(DylibCommand *)buf.data();
  cmd.cmd = get_type();
  cmd.cmdsize = buf.size();
  cmd.nameoff = sizeof(cmd);
  cmd.timestamp = 2;
  cmd.current_version = ctx.arg.current_version.encode();
  cmd.compatibility_version = ctx.arg.compatibility_version.encode();
  write_string(buf.data() + sizeof(cmd), dylib.install_name);
  return buf;
}

template <typename E>
static std::vector<u8> create_rpath_cmd(Context<E> &ctx, std::string_view name) {
  i64 size = sizeof(RpathCommand) + name.size() + 1; // +1 for NUL
  std::vector<u8> buf(align_to(size, 8));

  RpathCommand &cmd = *(RpathCommand *)buf.data();
  cmd.cmd = LC_RPATH;
  cmd.cmdsize = buf.size();
  cmd.path_off = sizeof(cmd);
  write_string(buf.data() + sizeof(cmd), name);
  return buf;
}

template <typename E>
static std::vector<u8> create_function_starts_cmd(Context<E> &ctx) {
  LinkEditDataCommand cmd = {};
  cmd.cmd = LC_FUNCTION_STARTS;
  cmd.cmdsize = sizeof(cmd);
  cmd.dataoff = ctx.function_starts->hdr.offset;
  cmd.datasize = ctx.function_starts->hdr.size;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_data_in_code_cmd(Context<E> &ctx) {
  LinkEditDataCommand cmd = {};
  cmd.cmd = LC_DATA_IN_CODE;
  cmd.cmdsize = sizeof(cmd);
  cmd.dataoff = ctx.data_in_code->hdr.offset;
  cmd.datasize = ctx.data_in_code->hdr.size;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_dyld_chained_fixups(Context<E> &ctx) {
  LinkEditDataCommand cmd = {};
  cmd.cmd = LC_DYLD_CHAINED_FIXUPS;
  cmd.cmdsize = sizeof(cmd);
  cmd.dataoff = ctx.chained_fixups->hdr.offset;
  cmd.datasize = ctx.chained_fixups->hdr.size;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_dyld_exports_trie(Context<E> &ctx) {
  LinkEditDataCommand cmd = {};
  cmd.cmd = LC_DYLD_EXPORTS_TRIE;
  cmd.cmdsize = sizeof(cmd);
  cmd.dataoff = ctx.export_.hdr.offset;
  cmd.datasize = ctx.export_.hdr.size;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<u8> create_sub_framework_cmd(Context<E> &ctx) {
  i64 size = sizeof(UmbrellaCommand) + ctx.arg.umbrella.size() + 1;
  std::vector<u8> buf(align_to(size, 8));

  UmbrellaCommand &cmd = *(UmbrellaCommand *)buf.data();
  cmd.cmd = LC_SUB_FRAMEWORK;
  cmd.cmdsize = buf.size();
  cmd.umbrella_off = sizeof(cmd);
  write_string(buf.data() + sizeof(cmd), ctx.arg.umbrella);
  return buf;
}

template <typename E>
static std::vector<u8> create_id_dylib_cmd(Context<E> &ctx) {
  i64 size = sizeof(DylibCommand) + ctx.arg.final_output.size() + 1;
  std::vector<u8> buf(align_to(size, 8));

  DylibCommand &cmd = *(DylibCommand *)buf.data();
  cmd.cmd = LC_ID_DYLIB;
  cmd.cmdsize = buf.size();
  cmd.nameoff = sizeof(cmd);
  write_string(buf.data() + sizeof(cmd), ctx.arg.final_output);
  return buf;
}

template <typename E>
static std::vector<u8> create_code_signature_cmd(Context<E> &ctx) {
  LinkEditDataCommand cmd = {};
  cmd.cmd = LC_CODE_SIGNATURE;
  cmd.cmdsize = sizeof(cmd);
  cmd.dataoff = ctx.code_sig->hdr.offset;
  cmd.datasize = ctx.code_sig->hdr.size;
  return to_u8vec(cmd);
}

template <typename E>
static std::vector<std::vector<u8>> create_load_commands(Context<E> &ctx) {
  std::vector<std::vector<u8>> vec;

  if (ctx.arg.pagezero_size)
    vec.push_back(create_pagezero_cmd(ctx));

  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
    vec.push_back(create_segment_cmd(ctx, *seg));

  if (ctx.chained_fixups && ctx.chained_fixups->hdr.size) {
    vec.push_back(create_dyld_chained_fixups(ctx));
    if (ctx.export_.hdr.size)
      vec.push_back(create_dyld_exports_trie(ctx));
  } else {
    vec.push_back(create_dyld_info_only_cmd(ctx));
  }

  vec.push_back(create_symtab_cmd(ctx));
  vec.push_back(create_dysymtab_cmd(ctx));

  if (ctx.arg.uuid != UUID_NONE)
    vec.push_back(create_uuid_cmd(ctx));

  vec.push_back(create_build_version_cmd(ctx));
  vec.push_back(create_source_version_cmd(ctx));

  if (ctx.arg.function_starts)
    vec.push_back(create_function_starts_cmd(ctx));

  for (DylibFile<E> *file : ctx.dylibs)
    if (file->dylib_idx != BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE)
      vec.push_back(create_load_dylib_cmd(ctx, *file));

  for (std::string_view rpath : ctx.arg.rpaths)
    vec.push_back(create_rpath_cmd(ctx, rpath));

  if (ctx.data_in_code)
    vec.push_back(create_data_in_code_cmd(ctx));

  if (!ctx.arg.umbrella.empty())
    vec.push_back(create_sub_framework_cmd(ctx));

  switch (ctx.output_type) {
  case MH_EXECUTE:
    vec.push_back(create_dylinker_cmd(ctx));
    vec.push_back(create_main_cmd(ctx));
    break;
  case MH_DYLIB:
    vec.push_back(create_id_dylib_cmd(ctx));
    break;
  case MH_BUNDLE:
    break;
  default:
    unreachable();
  }

  if (ctx.code_sig)
    vec.push_back(create_code_signature_cmd(ctx));
  return vec;
}

template <typename E>
void OutputMachHeader<E>::compute_size(Context<E> &ctx) {
  std::vector<std::vector<u8>> cmds = create_load_commands(ctx);
  this->hdr.size = sizeof(MachHeader) + flatten(cmds).size() + ctx.arg.headerpad;
}

template <typename E>
static bool has_tlv(Context<E> &ctx) {
  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
    for (Chunk<E> *chunk : seg->chunks)
      if (chunk->hdr.type == S_THREAD_LOCAL_VARIABLES)
        return true;
  return false;
}

template <typename E>
static bool has_reexported_lib(Context<E> &ctx) {
  for (DylibFile<E> *file : ctx.dylibs)
    if (file->is_reexported)
      return true;
  return false;
}

template <typename E>
void OutputMachHeader<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + this->hdr.offset;

  std::vector<std::vector<u8>> cmds = create_load_commands(ctx);

  MachHeader &mhdr = *(MachHeader *)buf;
  mhdr.magic = 0xfeedfacf;
  mhdr.cputype = E::cputype;
  mhdr.cpusubtype = E::cpusubtype;
  mhdr.filetype = ctx.output_type;
  mhdr.ncmds = cmds.size();
  mhdr.sizeofcmds = flatten(cmds).size();
  mhdr.flags = MH_TWOLEVEL | MH_NOUNDEFS | MH_DYLDLINK | MH_PIE;

  if (has_tlv(ctx))
    mhdr.flags |= MH_HAS_TLV_DESCRIPTORS;

  if (ctx.output_type == MH_DYLIB && !has_reexported_lib(ctx))
    mhdr.flags |= MH_NO_REEXPORTED_DYLIBS;

  if (ctx.arg.mark_dead_strippable_dylib)
    mhdr.flags |= MH_DEAD_STRIPPABLE_DYLIB;

  if (ctx.arg.application_extension)
    mhdr.flags |= MH_APP_EXTENSION_SAFE;

  write_vector(buf + sizeof(mhdr), flatten(cmds));
}

template <typename E>
OutputSection<E> *
OutputSection<E>::get_instance(Context<E> &ctx, std::string_view segname,
                               std::string_view sectname) {
  static std::shared_mutex mu;

  auto find = [&]() -> OutputSection<E> * {
    for (Chunk<E> *chunk : ctx.chunks) {
      if (chunk->hdr.match(segname, sectname)) {
        if (OutputSection<E> *osec = chunk->to_osec())
          return osec;
        Fatal(ctx) << "reserved name is used: " << segname << "," << sectname;
      }
    }
    return nullptr;
  };

  {
    std::shared_lock lock(mu);
    if (OutputSection<E> *osec = find())
      return osec;
  }

  std::unique_lock lock(mu);
  if (OutputSection<E> *osec = find())
    return osec;

  OutputSection<E> *osec = new OutputSection<E>(ctx, segname, sectname);
  ctx.chunk_pool.emplace_back(osec);
  return osec;
}

template <typename E>
void OutputSection<E>::compute_size(Context<E> &ctx) {
  if constexpr (is_arm<E>) {
    if (this->hdr.attr & S_ATTR_SOME_INSTRUCTIONS ||
        this->hdr.attr & S_ATTR_PURE_INSTRUCTIONS) {
      create_range_extension_thunks(ctx, *this);
      return;
    }
  }

  // As a special case, we need a word-size padding at the beginning
  // of __data for dyld. It is located by __dyld_private symbol.
  u64 offset = (this == ctx.data) ? sizeof(Word<E>) : 0;

  for (Subsection<E> *subsec : members) {
    offset = align_to(offset, 1 << subsec->p2align);
    subsec->output_offset = offset;
    offset += subsec->input_size;
  }
  this->hdr.size = offset;
}

template <typename E>
void OutputSection<E>::copy_buf(Context<E> &ctx) {
  assert(this->hdr.type != S_ZEROFILL);

  tbb::parallel_for_each(members, [&](Subsection<E> *subsec) {
    std::string_view data = subsec->get_contents();
    u8 *loc = ctx.buf + this->hdr.offset + subsec->output_offset;
    memcpy(loc, data.data(), data.size());
    subsec->apply_reloc(ctx, loc);
  });

  if constexpr (is_arm<E>) {
    tbb::parallel_for_each(thunks,
                           [&](std::unique_ptr<RangeExtensionThunk<E>> &thunk) {
      thunk->copy_buf(ctx);
    });
  }
}

template <typename E>
OutputSegment<E> *
OutputSegment<E>::get_instance(Context<E> &ctx, std::string_view name) {
  static std::shared_mutex mu;

  auto find = [&]() -> OutputSegment<E> *{
    for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
      if (seg->cmd.get_segname() == name)
        return seg.get();
    return nullptr;
  };

  {
    std::shared_lock lock(mu);
    if (OutputSegment<E> *seg = find())
      return seg;
  }

  std::unique_lock lock(mu);
  if (OutputSegment<E> *seg = find())
    return seg;

  OutputSegment<E> *seg = new OutputSegment<E>(name);
  ctx.segments.emplace_back(seg);
  return seg;
}

template <typename E>
OutputSegment<E>::OutputSegment(std::string_view name) {
  cmd.cmd = LC_SEGMENT_64;
  memcpy(cmd.segname, name.data(), name.size());

  if (name == "__PAGEZERO")
    cmd.initprot = 0;
  else if (name == "__TEXT")
    cmd.initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  else if (name == "__LINKEDIT")
    cmd.initprot = VM_PROT_READ;
  else
    cmd.initprot = VM_PROT_READ | VM_PROT_WRITE;

  cmd.maxprot = cmd.initprot;

  if (name == "__DATA_CONST")
    cmd.flags = SG_READ_ONLY;
}

template <typename E>
void OutputSegment<E>::set_offset(Context<E> &ctx, i64 fileoff, u64 vmaddr) {
  cmd.fileoff = fileoff;
  cmd.vmaddr = vmaddr;

  if (cmd.get_segname() == "__LINKEDIT")
    set_offset_linkedit(ctx, fileoff, vmaddr);
  else
    set_offset_regular(ctx, fileoff, vmaddr);
}

template <typename E>
void OutputSegment<E>::set_offset_regular(Context<E> &ctx, i64 fileoff,
                                          u64 vmaddr) {
  Timer t(ctx, std::string(cmd.get_segname()));
  i64 i = 0;

  auto is_bss = [](Chunk<E> &x) {
    return x.hdr.type == S_ZEROFILL || x.hdr.type == S_THREAD_LOCAL_ZEROFILL;
  };

  auto get_tls_alignment = [&] {
    i64 val = 1;
    for (Chunk<E> *chunk : chunks)
      if (chunk->hdr.type == S_THREAD_LOCAL_REGULAR ||
          chunk->hdr.type == S_THREAD_LOCAL_ZEROFILL)
        val = std::max<i64>(val, 1 << chunk->hdr.p2align);
    return val;
  };

  auto get_alignment = [&](Chunk<E> &chunk) -> u32 {
    switch (chunk.hdr.type) {
    case S_THREAD_LOCAL_REGULAR:
    case S_THREAD_LOCAL_ZEROFILL:
      // A TLS initialization image is copied as a contiguous block, so
      // the alignment of it is the largest of __thread_data and
      // __thread_bss.  This function returns an alignment value of a TLS
      // initialization image.
      return get_tls_alignment();
    case S_THREAD_LOCAL_VARIABLES:
      // __thread_vars needs to be aligned to word size because it
      // contains pointers. For some reason, Apple's clang creates it with
      // an alignment of 1. So we need to override.
      return sizeof(Word<E>);
    default:
      return 1 << chunk.hdr.p2align;
    }
  };

  // Assign offsets to non-BSS sections
  while (i < chunks.size() && !is_bss(*chunks[i])) {
    Timer t2(ctx, std::string(chunks[i]->hdr.get_sectname()), &t);
    Chunk<E> &sec = *chunks[i++];

    fileoff = align_to(fileoff, get_alignment(sec));
    vmaddr = align_to(vmaddr, get_alignment(sec));

    sec.hdr.offset = fileoff;
    sec.hdr.addr = vmaddr;

    sec.compute_size(ctx);
    fileoff += sec.hdr.size;
    vmaddr += sec.hdr.size;
  }

  // Assign offsets to BSS sections
  while (i < chunks.size()) {
    Chunk<E> &sec = *chunks[i++];
    assert(is_bss(sec));

    vmaddr = align_to(vmaddr, get_alignment(sec));
    sec.hdr.addr = vmaddr;
    sec.compute_size(ctx);
    vmaddr += sec.hdr.size;
  }

  cmd.vmsize = align_to(vmaddr - cmd.vmaddr, E::page_size);
  cmd.filesize = align_to(fileoff - cmd.fileoff, E::page_size);
}

template <typename E>
void OutputSegment<E>::set_offset_linkedit(Context<E> &ctx, i64 fileoff,
                                           u64 vmaddr) {
  Timer t(ctx, "__LINKEDIT");

  // Unlike regular segments, __LINKEDIT member sizes can be computed in
  // parallel except __string_table and __code_signature sections.
  auto skip = [&](Chunk<E> *c) {
    return c == &ctx.strtab || c == ctx.code_sig.get();
  };

  tbb::parallel_for_each(chunks, [&](Chunk<E> *chunk) {
    if (!skip(chunk)) {
      Timer t2(ctx, std::string(chunk->hdr.get_sectname()), &t);
      chunk->compute_size(ctx);
    }
  });

  for (Chunk<E> *chunk : chunks) {
    fileoff = align_to(fileoff, 1 << chunk->hdr.p2align);
    vmaddr = align_to(vmaddr, 1 << chunk->hdr.p2align);

    chunk->hdr.offset = fileoff;
    chunk->hdr.addr = vmaddr;

    if (skip(chunk)) {
      Timer t2(ctx, std::string(chunk->hdr.get_sectname()), &t);
      chunk->compute_size(ctx);
    }

    fileoff += chunk->hdr.size;
    vmaddr += chunk->hdr.size;
  }

  cmd.vmsize = align_to(vmaddr - cmd.vmaddr, E::page_size);
  cmd.filesize = fileoff - cmd.fileoff;
}

struct RebaseEntry {
  RebaseEntry(const RebaseEntry &) = default;
  RebaseEntry(i32 seg_idx, i32 offset) : seg_idx(seg_idx), offset(offset) {}
  auto operator<=>(const RebaseEntry &) const = default;

  i32 seg_idx;
  i32 offset;
};

static std::vector<u8> encode_rebase_entries(std::vector<RebaseEntry> &rebases) {
  std::vector<u8> buf;
  buf.push_back(REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER);

  // Sort rebase entries to reduce the size of the output
  tbb::parallel_sort(rebases);

  // Emit rebase records
  for (i64 i = 0; i < rebases.size();) {
    RebaseEntry &cur = rebases[i];
    RebaseEntry *last = (i == 0) ? nullptr : &rebases[i - 1];

    // Write a segment index and an offset
    if (!last || last->seg_idx != cur.seg_idx ||
        cur.offset - last->offset - 8 < 0) {
      buf.push_back(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | cur.seg_idx);
      encode_uleb(buf, cur.offset);
    } else {
      i64 dist = cur.offset - last->offset - 8;
      assert(dist >= 0);

      if (dist % 8 == 0 && dist < 128) {
        buf.push_back(REBASE_OPCODE_ADD_ADDR_IMM_SCALED | (dist >> 3));
      } else {
        buf.push_back(REBASE_OPCODE_ADD_ADDR_ULEB);
        encode_uleb(buf, dist);
      }
    }

    // Advance j so that j refers to past of the end of consecutive relocs
    i64 j = i + 1;
    while (j < rebases.size() &&
           rebases[j - 1].seg_idx == rebases[j].seg_idx &&
           rebases[j - 1].offset + 8 == rebases[j].offset)
      j++;

    // Write the consecutive relocs
    if (j - i < 16) {
      buf.push_back(REBASE_OPCODE_DO_REBASE_IMM_TIMES | (j - i));
    } else {
      buf.push_back(REBASE_OPCODE_DO_REBASE_ULEB_TIMES);
      encode_uleb(buf, j - i);
    }

    i = j;
  }

  buf.push_back(REBASE_OPCODE_DONE);
  buf.resize(align_to(buf.size(), 8));
  return buf;
}

template <typename E>
static bool needs_rebasing(const Relocation<E> &r) {
  // Rebase only ARM64_RELOC_UNSIGNED or X86_64_RELOC_UNSIGNED relocs.
  if (r.type != E::abs_rel)
    return false;

  // If the reloc specifies the relative address between two relocations,
  // we don't need a rebase reloc.
  if (r.is_subtracted)
    return false;

  // If we have a dynamic reloc, we don't need to rebase it.
  if (r.sym() && r.sym()->is_imported)
    return false;

  // If it refers a TLS block, it's already relative to the thread
  // pointer, so it doesn't have to be adjusted to the loaded address.
  if (r.refers_to_tls())
    return false;

  return true;
}

template <typename E>
inline void RebaseSection<E>::compute_size(Context<E> &ctx) {
  std::vector<std::vector<RebaseEntry>> vec(ctx.objs.size());

  tbb::parallel_for((i64)0, (i64)ctx.objs.size(), [&](i64 i) {
    for (Subsection<E> *subsec : ctx.objs[i]->subsections) {
      if (!subsec->is_alive)
        continue;

      std::span<Relocation<E>> rels = subsec->get_rels();
      OutputSegment<E> &seg = *subsec->isec->osec.seg;
      i64 base = subsec->get_addr(ctx) - seg.cmd.vmaddr;

      for (Relocation<E> &rel : rels)
        if (needs_rebasing(rel))
          vec[i].emplace_back(seg.seg_idx, base + rel.offset);
    }
  });

  std::vector<RebaseEntry> rebases = flatten(vec);

  for (i64 i = 0; Symbol<E> *sym : ctx.stubs.syms)
    if (!sym->has_got())
      rebases.emplace_back(ctx.data_seg->seg_idx,
                           ctx.lazy_symbol_ptr->hdr.addr + i++ * sizeof(Word<E>) -
                           ctx.data_seg->cmd.vmaddr);

  for (Symbol<E> *sym : ctx.got.syms)
    if (!sym->is_imported)
      rebases.emplace_back(ctx.data_const_seg->seg_idx,
                           sym->get_got_addr(ctx) - ctx.data_const_seg->cmd.vmaddr);

  for (Symbol<E> *sym : ctx.thread_ptrs.syms)
    if (!sym->is_imported)
      rebases.emplace_back(ctx.data_seg->seg_idx,
                           sym->get_tlv_addr(ctx) - ctx.data_seg->cmd.vmaddr);

  contents = encode_rebase_entries(rebases);
  this->hdr.size = contents.size();
}

template <typename E>
inline void RebaseSection<E>::copy_buf(Context<E> &ctx) {
  write_vector(ctx.buf + this->hdr.offset, contents);
}

template <typename E>
struct BindEntry {
  BindEntry(const BindEntry &) = default;
  BindEntry(Symbol<E> *sym, i32 seg_idx, i32 offset, i64 addend)
    : sym(sym), seg_idx(seg_idx), offset(offset), addend(addend) {}

  Symbol<E> *sym;
  i32 seg_idx;
  i32 offset;
  i64 addend;
};

template <typename E>
static i32 get_dylib_idx(Context<E> &ctx, Symbol<E> &sym) {
  assert(sym.is_imported);

  if (ctx.arg.flat_namespace)
    return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

  if (sym.file->is_dylib)
    return ((DylibFile<E> *)sym.file)->dylib_idx;

  assert(!ctx.arg.U.empty());
  return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;
}

template <typename E>
std::vector<u8>
encode_bind_entries(Context<E> &ctx, std::vector<BindEntry<E>> &bindings) {
  std::vector<u8> buf;
  buf.push_back(BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER);

  // Sort the vector to minimize the encoded binding info size.
  sort(bindings, [](const BindEntry<E> &a, const BindEntry<E> &b) {
    return std::tuple(a.sym->name, a.seg_idx, a.offset, a.addend) <
           std::tuple(b.sym->name, b.seg_idx, b.offset, b.addend);
  });

  // Encode bindings
  for (i64 i = 0; i < bindings.size(); i++) {
    BindEntry<E> &b = bindings[i];
    BindEntry<E> *last = (i == 0) ? nullptr : &bindings[i - 1];

    if (!last || b.sym->file != last->sym->file) {
      i64 idx = get_dylib_idx(ctx, *b.sym);
      if (idx < 0) {
        buf.push_back(BIND_OPCODE_SET_DYLIB_SPECIAL_IMM |
                      (idx & BIND_IMMEDIATE_MASK));
      } else if (idx < 16) {
        buf.push_back(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | idx);
      } else {
        buf.push_back(BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB);
        encode_uleb(buf, idx);
      }
    }

    if (!last || last->sym->name != b.sym->name ||
        last->sym->is_weak != b.sym->is_weak) {
      i64 flags = (b.sym->is_weak ? BIND_SYMBOL_FLAGS_WEAK_IMPORT : 0);
      buf.push_back(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | flags);

      std::string_view name = b.sym->name;
      buf.insert(buf.end(), (u8 *)name.data(), (u8 *)(name.data() + name.size()));
      buf.push_back('\0');
    }

    if (!last || last->seg_idx != b.seg_idx || last->offset != b.offset) {
      assert(b.seg_idx < 16);
      buf.push_back(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | b.seg_idx);
      encode_uleb(buf, b.offset);
    }

    if (!last || last->addend != b.addend) {
      buf.push_back(BIND_OPCODE_SET_ADDEND_SLEB);
      encode_sleb(buf, b.addend);
    }

    buf.push_back(BIND_OPCODE_DO_BIND);
  }

  buf.push_back(BIND_OPCODE_DONE);
  buf.resize(align_to(buf.size(), 8));
  return buf;
}

template <typename E>
void BindSection<E>::compute_size(Context<E> &ctx) {
  std::vector<std::vector<BindEntry<E>>> vec(ctx.objs.size());

  tbb::parallel_for((i64)0, (i64)ctx.objs.size(), [&](i64 i) {
    for (Subsection<E> *subsec : ctx.objs[i]->subsections) {
      if (subsec->is_alive) {
        for (Relocation<E> &r : subsec->get_rels()) {
          if (r.type == E::abs_rel && r.sym() && r.sym()->is_imported) {
            OutputSegment<E> &seg = *subsec->isec->osec.seg;
            vec[i].emplace_back(r.sym(), seg.seg_idx,
                                subsec->get_addr(ctx) + r.offset - seg.cmd.vmaddr,
                                r.addend);
          }
        }
      }
    }
  });

  std::vector<BindEntry<E>> bindings = flatten(vec);

  for (Symbol<E> *sym : ctx.got.syms)
    if (sym->is_imported)
      bindings.emplace_back(sym, ctx.data_const_seg->seg_idx,
                            sym->get_got_addr(ctx) - ctx.data_const_seg->cmd.vmaddr,
                            0);

  for (Symbol<E> *sym : ctx.thread_ptrs.syms)
    if (sym->is_imported)
      bindings.emplace_back(sym, ctx.data_seg->seg_idx,
                            sym->get_tlv_addr(ctx) - ctx.data_seg->cmd.vmaddr, 0);

  contents = encode_bind_entries(ctx, bindings);
  this->hdr.size = contents.size();
}

template <typename E>
void BindSection<E>::copy_buf(Context<E> &ctx) {
  write_vector(ctx.buf + this->hdr.offset, contents);
}

template <typename E>
void LazyBindSection<E>::add(Context<E> &ctx, Symbol<E> &sym, i64 idx) {
  auto emit = [&](u8 byte) {
    contents.push_back(byte);
  };

  i64 dylib_idx = get_dylib_idx(ctx, sym);

  if (dylib_idx < 0) {
    emit(BIND_OPCODE_SET_DYLIB_SPECIAL_IMM | (dylib_idx & BIND_IMMEDIATE_MASK));
  } else if (dylib_idx < 16) {
    emit(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | dylib_idx);
  } else {
    emit(BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB);
    encode_uleb(contents, dylib_idx);
  }

  i64 flags = (sym.is_weak ? BIND_SYMBOL_FLAGS_WEAK_IMPORT : 0);
  assert(flags < 16);

  emit(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | flags);
  contents.insert(contents.end(), (u8 *)sym.name.data(),
                  (u8 *)(sym.name.data() + sym.name.size()));
  emit('\0');

  i64 seg_idx = ctx.data_seg->seg_idx;
  emit(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | seg_idx);

  i64 offset = ctx.lazy_symbol_ptr->hdr.addr + idx * sizeof(Word<E>) -
               ctx.data_seg->cmd.vmaddr;
  encode_uleb(contents, offset);

  emit(BIND_OPCODE_DO_BIND);
  emit(BIND_OPCODE_DONE);
}

template <typename E>
void LazyBindSection<E>::compute_size(Context<E> &ctx) {
  bind_offsets.clear();

  for (i64 i = 0; i < ctx.stubs.syms.size(); i++) {
    bind_offsets.push_back(contents.size());
    add(ctx, *ctx.stubs.syms[i], i);
  }

  contents.resize(align_to(contents.size(), 1 << this->hdr.p2align));
  this->hdr.size = contents.size();
}

template <typename E>
void LazyBindSection<E>::copy_buf(Context<E> &ctx) {
  write_vector(ctx.buf + this->hdr.offset, contents);
}

inline i64 ExportEncoder::finish() {
  tbb::parallel_sort(entries, [](const Entry &a, const Entry &b) {
    return a.name < b.name;
  });

  // Construct a trie
  TrieNode node;
  tbb::task_group tg;
  construct_trie(node, entries, 0, &tg, entries.size() / 32, true);
  tg.wait();

  if (node.prefix.empty())
    root = std::move(node);
  else
    root.children.emplace_back(new TrieNode(std::move(node)));

  // Set output offsets to trie nodes. Since a serialized trie node
  // contains output offsets of other nodes in the variable-length
  // ULEB format, it unfortunately needs more than one iteration.
  // We need to repeat until the total size of the serialized trie
  // converges to obtain the optimized output. However, in reality,
  // repeating this step twice is enough. Size reduction on third and
  // further iterations is negligible.
  set_offset(root, 0);
  return set_offset(root, 0);
}

static i64 common_prefix_len(std::string_view x, std::string_view y) {
  i64 i = 0;
  while (i < x.size() && i < y.size() && x[i] == y[i])
    i++;
  return i;
}

void
inline ExportEncoder::construct_trie(TrieNode &node, std::span<Entry> entries,
                                     i64 len, tbb::task_group *tg,
                                     i64 grain_size, bool divide) {
  i64 new_len = common_prefix_len(entries[0].name, entries.back().name);

  if (new_len > len) {
    node.prefix = entries[0].name.substr(len, new_len - len);
    if (entries[0].name.size() == new_len) {
      node.is_leaf = true;
      node.flags = entries[0].flags;
      node.addr = entries[0].addr;
      entries = entries.subspan(1);
    }
  }

  for (i64 i = 0; i < entries.size();) {
    auto it = std::partition_point(entries.begin() + i + 1, entries.end(),
                                   [&](const Entry &ent) {
      return entries[i].name[new_len] == ent.name[new_len];
    });
    i64 j = it - entries.begin();

    TrieNode *child = new TrieNode;
    std::span<Entry> subspan = entries.subspan(i, j - i);

    if (divide && j - i < grain_size) {
      tg->run([=, this] {
        construct_trie(*child, subspan, new_len, tg, grain_size, false);
      });
    } else {
      construct_trie(*child, subspan, new_len, tg, grain_size, divide);
    }

    node.children.emplace_back(child);
    i = j;
  }
}

inline i64 ExportEncoder::set_offset(TrieNode &node, i64 offset) {
  node.offset = offset;

  i64 size = 0;
  if (node.is_leaf) {
    size = uleb_size(node.flags) + uleb_size(node.addr);
    size += uleb_size(size);
  } else {
    size = 1;
  }

  size++; // # of children

  for (std::unique_ptr<TrieNode> &child : node.children) {
    // +1 for NUL byte
    size += child->prefix.size() + 1 + uleb_size(child->offset);
  }

  for (std::unique_ptr<TrieNode> &child : node.children)
    size += set_offset(*child, offset + size);
  return size;
}

inline void ExportEncoder::write_trie(u8 *start, TrieNode &node) {
  u8 *buf = start + node.offset;

  if (node.is_leaf) {
    buf += write_uleb(buf, uleb_size(node.flags) + uleb_size(node.addr));
    buf += write_uleb(buf, node.flags);
    buf += write_uleb(buf, node.addr);
  } else {
    *buf++ = 0;
  }

  *buf++ = node.children.size();

  for (std::unique_ptr<TrieNode> &child : node.children) {
    buf += write_string(buf, child->prefix);
    buf += write_uleb(buf, child->offset);
  }

  for (std::unique_ptr<TrieNode> &child : node.children)
    write_trie(start, *child);
}

template <typename E>
void ExportSection<E>::compute_size(Context<E> &ctx) {
  auto get_flags = [](Symbol<E> &sym) {
    u32 flags = 0;
    if (sym.is_weak)
      flags |= EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION;
    if (sym.is_tlv)
      flags |= EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL;
    return flags;
  };

  for (ObjectFile<E> *file : ctx.objs)
    for (Symbol<E> *sym : file->syms)
      if (sym && sym->file == file && sym->visibility == SCOPE_GLOBAL)
        enc.entries.push_back({sym->name, get_flags(*sym),
                               sym->get_addr(ctx) - ctx.mach_hdr.hdr.addr});

  if (enc.entries.empty())
    return;

  this->hdr.size = align_to(enc.finish(), 8);
}

template <typename E>
void ExportSection<E>::copy_buf(Context<E> &ctx) {
  if (this->hdr.size == 0)
    return;

  u8 *buf = ctx.buf + this->hdr.offset;
  memset(buf, 0, this->hdr.size);
  enc.write_trie(buf, enc.root);
}

// LC_FUNCTION_STARTS contains function start addresses encoded in
// ULEB128. I don't know what tools consume this table, but we create
// it anyway by default for the sake of compatibility.
template <typename E>
void FunctionStartsSection<E>::compute_size(Context<E> &ctx) {
  std::vector<std::vector<u64>> vec(ctx.objs.size());

  tbb::parallel_for((i64)0, (i64)ctx.objs.size(), [&](i64 i) {
    ObjectFile<E> &file = *ctx.objs[i];
    for (Symbol<E> *sym : file.syms)
      if (sym && sym->file == &file && sym->subsec && sym->subsec->is_alive &&
          &sym->subsec->isec->osec == ctx.text)
        vec[i].push_back(sym->get_addr(ctx));
  });

  std::vector<u64> addrs = flatten(vec);
  tbb::parallel_sort(addrs.begin(), addrs.end());

  // We need a NUL terminator at the end. We also want to make sure that
  // the size is a multiple of 8 because the `strip` command assumes that
  // there's no gap between __func_starts and the following __data_in_code.
  contents.resize(align_to(addrs.size() * 5 + 1, 8));

  u8 *p = contents.data();
  u64 last = ctx.mach_hdr.hdr.addr;

  for (u64 val : addrs) {
    p += write_uleb(p, val - last);
    last = val;
  }

  // Write the terminator
  p += write_uleb(p, 0);

  this->hdr.size = align_to(p - contents.data(), 8);
  contents.resize(this->hdr.size);
}

template <typename E>
void FunctionStartsSection<E>::copy_buf(Context<E> &ctx) {
  write_vector(ctx.buf + this->hdr.offset, contents);
}

// The symbol table in an output file is sorted by symbol type (local,
// global or undef).
template <typename E>
void SymtabSection<E>::compute_size(Context<E> &ctx) {
  std::string cwd = std::filesystem::current_path().string();

  std::vector<InputFile<E> *> vec;
  append(vec, ctx.objs);
  append(vec, ctx.dylibs);

  // Compute the number of symbols for each symbol type
  tbb::parallel_for_each(vec, [&](InputFile<E> *file) {
    file->compute_symtab_size(ctx);
  });

  // Compute the indices in the symbol table
  InputFile<E> &first = *vec.front();
  InputFile<E> &last = *vec.back();

  // Add -add_ast_path symbols first
  first.stabs_offset = ctx.arg.add_ast_path.size();
  first.strtab_offset = strtab_init_image.size();
  for (std::string_view s : ctx.arg.add_ast_path)
    first.strtab_offset += s.size() + 1;

  // Add input file symbols
  for (i64 i = 1; i < vec.size(); i++)
    vec[i]->stabs_offset = vec[i - 1]->stabs_offset + vec[i - 1]->num_stabs;

  first.locals_offset = last.stabs_offset + last.num_stabs;

  for (i64 i = 1; i < vec.size(); i++)
    vec[i]->locals_offset = vec[i - 1]->locals_offset + vec[i - 1]->num_locals;

  globals_offset = last.locals_offset + last.num_locals;
  first.globals_offset = globals_offset;

  for (i64 i = 1; i < vec.size(); i++)
    vec[i]->globals_offset = vec[i - 1]->globals_offset + vec[i - 1]->num_globals;

  undefs_offset = last.globals_offset + last.num_globals;
  first.undefs_offset = undefs_offset;

  for (i64 i = 1; i < vec.size(); i++)
    vec[i]->undefs_offset = vec[i - 1]->undefs_offset + vec[i - 1]->num_undefs;

  for (i64 i = 1; i < vec.size(); i++)
    vec[i]->strtab_offset = vec[i - 1]->strtab_offset + vec[i - 1]->strtab_size;

  i64 num_symbols = last.undefs_offset + last.num_undefs;
  this->hdr.size = num_symbols * sizeof(MachSym<E>);
  ctx.strtab.hdr.size = last.strtab_offset + last.strtab_size;

  // Update symbol's output_symtab_idx
  tbb::parallel_for_each(vec, [&](InputFile<E> *file) {
    i64 locals = file->locals_offset;
    i64 globals = file->globals_offset;
    i64 undefs = file->undefs_offset;

    for (Symbol<E> *sym : file->syms) {
      if (sym && sym->file == file && sym->output_symtab_idx == -2) {
        if (sym->is_imported)
          sym->output_symtab_idx = undefs++;
        else if (sym->visibility == SCOPE_GLOBAL)
          sym->output_symtab_idx = globals++;
        else
          sym->output_symtab_idx = locals++;
      }
    }
  });
}

template <typename E>
void SymtabSection<E>::copy_buf(Context<E> &ctx) {
  // Create symbols for -add_ast_path
  MachSym<E> *buf = (MachSym<E> *)(ctx.buf + this->hdr.offset);
  u8 *strtab = ctx.buf + ctx.strtab.hdr.offset;
  i64 stroff = strtab_init_image.size();

  memcpy(strtab, strtab_init_image.data(), strtab_init_image.size());

  for (std::string_view path : ctx.arg.add_ast_path) {
    MachSym<E> &msym = *buf++;
    msym.stroff = stroff;
    msym.n_type = N_AST;
    stroff += write_string(strtab + stroff, path);
  }

  // Copy symbols from input files to an output file
  std::vector<InputFile<E> *> files;
  append(files, ctx.objs);
  append(files, ctx.dylibs);

  tbb::parallel_for_each(files, [&](InputFile<E> *file) {
    file->populate_symtab(ctx);
  });
}

template <typename E>
void IndirectSymtabSection<E>::compute_size(Context<E> &ctx) {
  ctx.got.hdr.reserved1 = 0;
  i64 n = ctx.got.syms.size();

  ctx.thread_ptrs.hdr.reserved1 = n;
  n += ctx.thread_ptrs.syms.size();

  ctx.stubs.hdr.reserved1 = n;
  n += ctx.stubs.syms.size();

  if (ctx.lazy_symbol_ptr) {
    ctx.lazy_symbol_ptr->hdr.reserved1 = n;
    n += ctx.stubs.syms.size();
  }

  this->hdr.size = n * ENTRY_SIZE;
}

template <typename E>
void IndirectSymtabSection<E>::copy_buf(Context<E> &ctx) {
  ul32 *buf = (ul32 *)(ctx.buf + this->hdr.offset);

  auto get_idx = [&](Symbol<E> &sym) -> u32 {
    if (sym.is_abs && sym.visibility != SCOPE_GLOBAL)
      return INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL;
    if (sym.is_abs)
      return INDIRECT_SYMBOL_ABS;
    if (sym.visibility != SCOPE_GLOBAL)
      return INDIRECT_SYMBOL_LOCAL;
    return sym.output_symtab_idx;
  };

  for (Symbol<E> *sym : ctx.got.syms)
    *buf++ = get_idx(*sym);

  for (Symbol<E> *sym : ctx.thread_ptrs.syms)
    *buf++ = get_idx(*sym);

  for (Symbol<E> *sym : ctx.stubs.syms)
    *buf++ = get_idx(*sym);

  if (ctx.lazy_symbol_ptr)
    for (Symbol<E> *sym : ctx.stubs.syms)
      *buf++ = get_idx(*sym);
}

// Create __DATA,__objc_imageinfo section contents by merging input
// __objc_imageinfo sections.
template <typename E>
std::unique_ptr<ObjcImageInfoSection<E>>
ObjcImageInfoSection<E>::create(Context<E> &ctx) {
  ObjcImageInfo info = {};

  for (ObjectFile<E> *file : ctx.objs) {
    if (!file->objc_image_info)
      continue;

    // Make sure that all object files have the same Swift version.
    ObjcImageInfo &info2 = *file->objc_image_info;
    if (info.swift_version == 0)
      info.swift_version = info2.swift_version;

    if (info.swift_version != info2.swift_version && info2.swift_version != 0)
      Error(ctx) << *file << ": incompatible __objc_imageinfo swift version"
                 << (u32)info.swift_version << " " << (u32)info2.swift_version;

    // swift_lang_version is set to the newest.
    info.swift_lang_version =
      std::max<u32>(info.swift_lang_version, info2.swift_lang_version);
  }

  // This property is on if it is on in all input object files
  auto has_category_class = [](ObjectFile<E> *file) -> bool {
    if (ObjcImageInfo *info = file->objc_image_info)
      return info->flags & OBJC_IMAGE_HAS_CATEGORY_CLASS_PROPERTIES;
    return false;
  };

  if (std::all_of(ctx.objs.begin(), ctx.objs.end(), has_category_class))
    info.flags |= OBJC_IMAGE_HAS_CATEGORY_CLASS_PROPERTIES;

  return std::make_unique<ObjcImageInfoSection<E>>(ctx, info);
}

template <typename E>
void ObjcImageInfoSection<E>::copy_buf(Context<E> &ctx) {
  memcpy(ctx.buf + this->hdr.offset, &contents, sizeof(contents));
}

// Input __mod_init_func sections contain pointers to global initializer
// functions. Since the addresses in the section are absolute, they need
// base relocations, so the scheme is not efficient in PIC (position-
// independent code).
//
// __init_offset is a new section to make it more efficient in PIC.
// The section contains 32-bit offsets from the beginning of the image
// to initializer functions. The section doesn't need base relocations.
//
// If `-init_offsets` is given, the linker converts input __mod_init_func
// sections into an __init_offset section.
template <typename E>
void InitOffsetsSection<E>::compute_size(Context<E> &ctx) {
  this->hdr.size = 0;
  for (ObjectFile<E> *file : ctx.objs)
    this->hdr.size += file->init_functions.size() * 4;
}

template <typename E>
void InitOffsetsSection<E>::copy_buf(Context<E> &ctx) {
  ul32 *buf = (ul32 *)(ctx.buf + this->hdr.offset);

  for (ObjectFile<E> *file : ctx.objs)
    for (Symbol<E> *sym : file->init_functions)
      *buf++ = sym->get_addr(ctx) - ctx.mach_hdr.hdr.addr;
}

template <typename E>
void CodeSignatureSection<E>::compute_size(Context<E> &ctx) {
  std::string filename = filepath(ctx.arg.final_output).filename().string();
  i64 filename_size = align_to(filename.size() + 1, 16);
  i64 num_blocks = align_to(this->hdr.offset, E::page_size) / E::page_size;
  this->hdr.size = sizeof(CodeSignatureHeader) + sizeof(CodeSignatureBlobIndex) +
                   sizeof(CodeSignatureDirectory) + filename_size +
                   num_blocks * SHA256_SIZE;
}

// A __code_signature section contains a digital signature for an output
// file so that the system can identify who created it.
//
// On ARM macOS, __code_signature is mandatory even if we don't have a key
// to sign. The signature we append in the following function is just
// SHA256 hashes of each page. Such signature is called the "ad-hoc"
// signature. Although mandating the ad-hoc signature doesn't make much
// sense because anyone can compute it, we need to always create it
// because otherwise the loader will simply rejects our output.
//
// On x86-64 macOS, __code_signature is optional. The loader doesn't reject
// an executable with no signature section.
template <typename E>
void CodeSignatureSection<E>::write_signature(Context<E> &ctx) {
  Timer t(ctx, "write_signature");

  u8 *buf = ctx.buf + this->hdr.offset;
  memset(buf, 0, this->hdr.size);

  std::string filename = filepath(ctx.arg.final_output).filename().string();
  i64 filename_size = align_to(filename.size() + 1, 16);
  i64 num_blocks = align_to(this->hdr.offset, E::page_size) / E::page_size;

  // Fill code-sign header fields
  CodeSignatureHeader &sighdr = *(CodeSignatureHeader *)buf;
  buf += sizeof(sighdr);

  sighdr.magic = CSMAGIC_EMBEDDED_SIGNATURE;
  sighdr.length = this->hdr.size;
  sighdr.count = 1;

  CodeSignatureBlobIndex &idx = *(CodeSignatureBlobIndex *)buf;
  buf += sizeof(idx);

  idx.type = CSSLOT_CODEDIRECTORY;
  idx.offset = sizeof(sighdr) + sizeof(idx);

  CodeSignatureDirectory &dir = *(CodeSignatureDirectory *)buf;
  buf += sizeof(dir);

  dir.magic = CSMAGIC_CODEDIRECTORY;
  dir.length = sizeof(dir) + filename_size + num_blocks * SHA256_SIZE;
  dir.version = CS_SUPPORTSEXECSEG;
  dir.flags = CS_ADHOC | CS_LINKER_SIGNED;
  dir.hash_offset = sizeof(dir) + filename_size;
  dir.ident_offset = sizeof(dir);
  dir.n_code_slots = num_blocks;
  dir.code_limit = this->hdr.offset;
  dir.hash_size = SHA256_SIZE;
  dir.hash_type = CS_HASHTYPE_SHA256;
  dir.page_size = std::countr_zero(E::page_size);
  dir.exec_seg_base = ctx.text_seg->cmd.fileoff;
  dir.exec_seg_limit = ctx.text_seg->cmd.filesize;
  if (ctx.output_type == MH_EXECUTE)
    dir.exec_seg_flags = CS_EXECSEG_MAIN_BINARY;

  memcpy(buf, filename.data(), filename.size());
  buf += filename_size;

  // Compute a hash value for each block.
  auto compute_hash = [&](i64 i) {
    u8 *start = ctx.buf + i * E::page_size;
    u8 *end = ctx.buf + std::min<i64>((i + 1) * E::page_size, this->hdr.offset);
    sha256_hash(start, end - start, buf + i * SHA256_SIZE);
  };

  for (i64 i = 0; i < num_blocks; i += 1024) {
    i64 j = std::min(num_blocks, i + 1024);
    tbb::parallel_for(i, j, compute_hash);

#if __APPLE__
    // Calling msync() with MS_ASYNC speeds up the following msync()
    // with MS_INVALIDATE.
    if (ctx.output_file->is_mmapped)
      msync(ctx.buf + i * E::page_size, 1024 * E::page_size, MS_ASYNC);
#endif
  }

  // A LC_UUID load command may also contain a crypto hash of the
  // entire file. We compute its value as a tree hash.
  if (ctx.arg.uuid == UUID_HASH) {
    u8 uuid[SHA256_SIZE];
    sha256_hash(ctx.buf + this->hdr.offset, this->hdr.size, uuid);

    // Indicate that this is UUIDv4 as defined by RFC4122.
    uuid[6] = (uuid[6] & 0b00001111) | 0b01010000;
    uuid[8] = (uuid[8] & 0b00111111) | 0b10000000;

    memcpy(ctx.uuid, uuid, 16);

    // Rewrite the load commands to write the updated UUID and
    // recompute code signatures for the updated blocks.
    ctx.mach_hdr.copy_buf(ctx);

    for (i64 i = 0; i * E::page_size < ctx.mach_hdr.hdr.size; i++)
      compute_hash(i);
  }

#if __APPLE__
  // If an executable's pages have been created via an mmap(), the output
  // file will fail for the code signature verification because the macOS
  // kernel wrongly assume that the pages may be mutable after the code
  // verification, though it is actually impossible after munmap().
  //
  // In order to workaround the issue, we call msync() to invalidate all
  // mmapped pages.
  //
  // https://openradar.appspot.com/FB8914231
  if (ctx.output_file->is_mmapped) {
    Timer t2(ctx, "msync", &t);
    msync(ctx.buf, ctx.output_file->filesize, MS_INVALIDATE);
  }
#endif
}

template <typename E>
void DataInCodeSection<E>::compute_size(Context<E> &ctx) {
  assert(contents.empty());

  for (ObjectFile<E> *file : ctx.objs) {
    LinkEditDataCommand *cmd =
      (LinkEditDataCommand *)file->find_load_command(ctx, LC_DATA_IN_CODE);
    if (!cmd)
      continue;

    std::span<DataInCodeEntry> entries = {
      (DataInCodeEntry *)(file->mf->data + cmd->dataoff),
      cmd->datasize / sizeof(DataInCodeEntry),
    };

    for (Subsection<E> *subsec : file->subsections) {
      if (entries.empty())
        break;

      DataInCodeEntry &ent = entries[0];
      if (subsec->input_addr + subsec->input_size < ent.offset)
        continue;

      if (ent.offset < subsec->input_addr + subsec->input_size) {
        u32 offset = subsec->get_addr(ctx) + subsec->input_addr - ent.offset -
                     ctx.text_seg->cmd.vmaddr;
        contents.push_back({offset, ent.length, ent.kind});
      }

      entries = entries.subspan(1);
    }
  }

  this->hdr.size = contents.size() * sizeof(contents[0]);
}

template <typename E>
void DataInCodeSection<E>::copy_buf(Context<E> &ctx) {
  write_vector(ctx.buf + this->hdr.offset, contents);
}

// Collect all locations that needs fixing on page-in
template <typename E>
std::vector<Fixup<E>> get_fixups(Context<E> &ctx) {
  std::vector<std::vector<Fixup<E>>> vec(ctx.objs.size());

  tbb::parallel_for((i64)0, (i64)ctx.objs.size(), [&](i64 i) {
    for (Subsection<E> *subsec : ctx.objs[i]->subsections) {
      if (!subsec->is_alive)
        continue;

      for (Relocation<E> &r : subsec->get_rels()) {
        if (r.type == E::abs_rel && r.sym() && r.sym()->is_imported)
          vec[i].push_back({subsec->get_addr(ctx) + r.offset, r.sym(),
                            (u64)r.addend});
        else if (needs_rebasing(r))
          vec[i].push_back({subsec->get_addr(ctx) + r.offset});
      }
    }
  });

  std::vector<Fixup<E>> fixups = flatten(vec);

  for (Symbol<E> *sym : ctx.got.syms)
    fixups.push_back({sym->get_got_addr(ctx), sym->is_imported ? sym : nullptr});

  for (Symbol<E> *sym : ctx.thread_ptrs.syms)
    fixups.push_back({sym->get_tlv_addr(ctx), sym->is_imported ? sym : nullptr});

  tbb::parallel_sort(fixups, [](const Fixup<E> &a, const Fixup<E> &b) {
    return a.addr < b.addr;
  });
  return fixups;
}

// Returns fixups for a given segment
template <typename E>
static std::span<Fixup<E>>
get_segment_fixups(std::vector<Fixup<E>> &fixups, OutputSegment<E> &seg) {
  auto begin = std::partition_point(fixups.begin(), fixups.end(),
                                    [&](const Fixup<E> &x) {
    return x.addr < seg.cmd.vmaddr;
  });

  if (begin == fixups.end())
    return {};

  auto end = std::partition_point(begin, fixups.end(), [&](const Fixup<E> &x) {
    return x.addr < seg.cmd.vmaddr + seg.cmd.vmsize;
  });

  return {&*begin, (size_t)(end - begin)};
}

template <typename E>
bool operator<(const SymbolAddend<E> &a, const SymbolAddend<E> &b) {
  if (a.sym != b.sym)
    return *a.sym < *b.sym;
  return a.addend < b.addend;
}

// A chained fixup can contain an addend if its value is equal to or
// smaller than 255.
static constexpr i64 MAX_INLINE_ADDEND = 255;

template <typename E>
static std::tuple<std::vector<SymbolAddend<E>>, u32>
get_dynsyms(std::vector<Fixup<E>> &fixups) {
  // Collect all dynamic relocations and sort them by addend
  std::vector<SymbolAddend<E>> syms;
  for (Fixup<E> &x : fixups)
    if (x.sym)
      syms.push_back({x.sym, x.addend <= MAX_INLINE_ADDEND ? 0 : x.addend});

  sort(syms);
  remove_duplicates(syms);

  // Set symbol ordinal
  for (i64 i = syms.size() - 1; i >= 0; i--)
    syms[i].sym->fixup_ordinal = i;

  // Dynamic relocations can have an arbitrary large addend. For example,
  // if you initialize a global variable pointer as `int *p = foo + (1<<31)`
  // and `foo` is an imported symbol, it generates a dynamic relocation with
  // an addend of 1<<31. Such large addend don't fit in a 64-bit in-place
  // chained relocation, as its `addend` bitfield is only 8 bit wide.
  //
  // A dynamic relocation with large addend is represented as a pair of
  // symbol name and an addend in the import table. We use an import table
  // structure with an addend field only if it's necessary.
  u64 max = 0;
  for (SymbolAddend<E> &x : syms)
    max = std::max(max, x.addend);

  u32 format;
  if (max == 0)
    format = DYLD_CHAINED_IMPORT;
  else if ((u32)max == max)
    format = DYLD_CHAINED_IMPORT_ADDEND;
  else
    format = DYLD_CHAINED_IMPORT_ADDEND64;

  return {std::move(syms), format};
}

template <typename E>
u8 *ChainedFixupsSection<E>::allocate(i64 size) {
  i64 off = contents.size();
  contents.resize(contents.size() + size);
  return contents.data() + off;
}

// macOS 13 or later supports a new mechanism to apply dynamic relocations.
// In this comment, I'll explain what it is and how it works.
//
// In the traditional dynamic linking mechanism, data relocations are
// applied eagerly on process startup; only function symbols are resolved
// lazily through PLT/stubs. The new mechanism called the "page-in linking"
// changes that; with the page-in linking, the kernel now applies data
// relocations as it loads a new page from disk to memory. The page-in
// linking has the following benefits compared to the ttraditional model:
//
//  1. Data relocations are no longer applied eagerly on startup, shotening
//     the process startup time.
//
//  2. It reduces the number of dirty pages because until a process actually
//     access a data page, no page is loaded to memory. Moreover, the kernel
//     can now discard (instead of page out) an read-only page with
//     relocations under memory pressure because it knows how to reconstruct
//     the same page by applying the same dynamic relocations.
//
// `__chainfixups` section contains data needed for the page-in linking.
// The section contains the first-level page table. Specifically, we have
// one DyldChainedStartsInSegment data structure for each segment, and the
// data structure contains a u16 array of the same length as the number of
// pages in the segment. Each u16 value represents an in-page offset within
// the corresponding page of the first location that needs fixing. With that
// information, the kernel is able to know the first dynamically-linked
// location in a page when it pages in a new page from disk.
//
// Unlike the traiditional dynamic linking model, there's no separate
// relocation table. Relocation record is represented in a compact 64-bit
// encoding and directly embedded to the place where the relocation is
// applied to. In addition to that, each in-place relocation record contains
// an offset to the next location in the same page that needs fixing. With
// that, the kernel is able to follow that chain to apply all relocations
// for a given page.
//
// This mechanism is so-called the "chained fixups", as the in-place
// relocations form a linked list.
template <typename E>
void ChainedFixupsSection<E>::compute_size(Context<E> &ctx) {
  fixups = get_fixups(ctx);
  if (fixups.empty())
    return;

  // Section header
  i64 hdr_size = align_to(sizeof(DyldChainedFixupsHeader), 8);
  auto *h = (DyldChainedFixupsHeader *)allocate(hdr_size);
  h->fixups_version = 0;
  h->starts_offset = contents.size();

  // Segment header
  i64 seg_count = ctx.segments.back()->seg_idx + 1;
  i64 starts_offset = contents.size();
  i64 starts_size = align_to(sizeof(DyldChainedStartsInImage) +
                             seg_count * 4, 8);
  auto *starts = (DyldChainedStartsInImage *)allocate(starts_size);
  starts->seg_count = seg_count;

  // Write the first-level page table for each segment
  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments) {
    std::span<Fixup<E>> fx = get_segment_fixups(fixups, *seg);
    if (fx.empty())
      continue;

    starts = (DyldChainedStartsInImage *)(contents.data() + starts_offset);
    starts->seg_info_offset[seg->seg_idx] = contents.size() - starts_offset;

    i64 npages =
      align_to(fx.back().addr + 1 - seg->cmd.vmaddr, E::page_size) / E::page_size;
    i64 size = align_to(sizeof(DyldChainedStartsInSegment) + npages * 2, 8);

    auto *rec = (DyldChainedStartsInSegment *)allocate(size);
    rec->size = size;
    rec->page_size = E::page_size;
    rec->pointer_format = DYLD_CHAINED_PTR_64;
    rec->segment_offset = seg->cmd.vmaddr - ctx.mach_hdr.hdr.addr;
    rec->max_valid_pointer = 0;
    rec->page_count = npages;

    for (i64 i = 0, j = 0; i < npages; i++) {
      u64 addr = seg->cmd.vmaddr + i * E::page_size;
      while (j < fixups.size() && fixups[j].addr < addr)
        j++;

      if (j < fixups.size() && fixups[j].addr < addr + E::page_size)
        rec->page_start[i] = fixups[j].addr & (E::page_size - 1);
      else
        rec->page_start[i] = DYLD_CHAINED_PTR_START_NONE;
    }
  }

  // Write symbol import table
  u32 import_format;
  std::tie(dynsyms, import_format) = get_dynsyms(fixups);

  h = (DyldChainedFixupsHeader *)contents.data();
  h->imports_count = dynsyms.size();
  h->imports_format = import_format;
  h->imports_offset = contents.size();

  if (import_format == DYLD_CHAINED_IMPORT)
    write_imports<DyldChainedImport>(ctx);
  else if (import_format == DYLD_CHAINED_IMPORT_ADDEND)
    write_imports<DyldChainedImportAddend>(ctx);
  else
    write_imports<DyldChainedImportAddend64>(ctx);

  // Write symbol names
  h = (DyldChainedFixupsHeader *)contents.data();
  h->symbols_offset = contents.size();
  h->symbols_format = 0;

  for (i64 i = 0; i < dynsyms.size(); i++)
    if (Symbol<E> *sym = dynsyms[i].sym;
        i == 0 || dynsyms[i - 1].sym != sym)
      write_string(allocate(sym->name.size() + 1), sym->name);

  contents.resize(align_to(contents.size(), 8));
  this->hdr.size = contents.size();
}

// This function is a part of ChainedFixupsSection<E>::compute_size(),
// but it's factored out as a separate function so that it can take
// a type parameter.
template <typename E>
template <typename T>
void ChainedFixupsSection<E>::write_imports(Context<E> &ctx) {
  T *imports = (T *)allocate(sizeof(T) * dynsyms.size());
  i64 nameoff = 0;

  for (i64 i = 0; i < dynsyms.size(); i++) {
    Symbol<E> &sym = *dynsyms[i].sym;

    if (ctx.arg.flat_namespace)
      imports[i].lib_ordinal = BIND_SPECIAL_DYLIB_FLAT_LOOKUP;
    else if (sym.file->is_dylib)
      imports[i].lib_ordinal = ((DylibFile<E> *)sym.file)->dylib_idx;
    else
      imports[i].lib_ordinal = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;

    imports[i].weak_import = sym.is_weak;
    imports[i].name_offset = nameoff;

    if constexpr (requires (T x) { x.addend; })
      imports[i].addend = dynsyms[i].addend;

    if (i + 1 == dynsyms.size() || dynsyms[i + 1].sym != &sym)
      nameoff += sym.name.size() + 1;
  }
}

template <typename E>
void ChainedFixupsSection<E>::copy_buf(Context<E> &ctx) {
  memcpy(ctx.buf + this->hdr.offset, contents.data(), contents.size());
}

template <typename E>
static InputSection<E> &
find_input_section(Context<E> &ctx, OutputSegment<E> &seg, u64 addr) {
  auto it = std::partition_point(seg.chunks.begin(), seg.chunks.end(),
                                 [&](Chunk<E> *chunk) {
    return chunk->hdr.addr < addr;
  });

  assert(it != seg.chunks.begin());

  OutputSection<E> *osec = it[-1]->to_osec();
  assert(osec);

  auto it2 = std::partition_point(osec->members.begin(), osec->members.end(),
                                  [&](Subsection<E> *subsec) {
    return subsec->get_addr(ctx) < addr;
  });

  assert(it2 != osec->members.begin());
  return *it2[-1]->isec;
}

// This function is called after copy_sections_to_output_file().
template <typename E>
void ChainedFixupsSection<E>::write_fixup_chains(Context<E> &ctx) {
  Timer t(ctx, "write_fixup_chains");

  auto page = [](u64 addr) { return addr & ~((u64)E::page_size - 1); };

  auto get_ordinal = [&](i64 i, u64 addend) {
    for (; i < dynsyms.size(); i++)
      if (dynsyms[i].addend == addend)
        return i;
    unreachable();
  };

  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments) {
    std::span<Fixup<E>> fx = get_segment_fixups(fixups, *seg);

    for (i64 i = 0; i < fx.size(); i++) {
      constexpr u32 stride = 4;

      u32 next = 0;
      if (i + 1 < fx.size() && page(fx[i + 1].addr) == page(fx[i].addr))
        next = (fx[i + 1].addr - fx[i].addr) / stride;

      u8 *loc = ctx.buf + seg->cmd.fileoff + (fx[i].addr - seg->cmd.vmaddr);

      if (Symbol<E> *sym = fx[i].sym) {
        if (fx[i].addr % stride)
          Error(ctx) << find_input_section(ctx, *seg, fx[i].addr)
                     << ": unaligned relocation against `" << *sym
                     << "; re-link with -no_fixup_chains";

        DyldChainedPtr64Bind *rec = (DyldChainedPtr64Bind *)loc;

        if (fx[i].addend <= MAX_INLINE_ADDEND) {
          rec->ordinal = sym->fixup_ordinal;
          rec->addend = fx[i].addend;
        } else {
          rec->ordinal = get_ordinal(sym->fixup_ordinal, fx[i].addend);
          rec->addend = 0;
        }

        rec->reserved = 0;
        rec->next = next;
        rec->bind = 1;
      } else {
        if (fx[i].addr % stride)
          Error(ctx) << find_input_section(ctx, *seg, fx[i].addr)
                     << ": unaligned base relocation; "
                     << "re-link with -no_fixup_chains";

        u64 val = *(ul64 *)loc;
        if (val & 0x00ff'fff0'0000'0000)
          Error(ctx) << seg->cmd.get_segname()
                     << ": rebase addend too large; re-link with -no_fixup_chains";

        DyldChainedPtr64Rebase *rec = (DyldChainedPtr64Rebase *)loc;
        rec->target = val;
        rec->high8 = val >> 56;
        rec->reserved = 0;
        rec->next = next;
        rec->bind = 0;
      }
    }
  }
}

template <typename E>
void StubsSection<E>::add(Context<E> &ctx, Symbol<E> *sym) {
  assert(sym->stub_idx == -1);
  sym->stub_idx = syms.size();

  syms.push_back(sym);
  this->hdr.size = syms.size() * E::stub_size;

  if (ctx.stub_helper) {
    if (ctx.stub_helper->hdr.size == 0)
      ctx.stub_helper->hdr.size = E::stub_helper_hdr_size;

    ctx.stub_helper->hdr.size += E::stub_helper_size;
    ctx.lazy_symbol_ptr->hdr.size += sizeof(Word<E>);
  }
}

template <typename E>
std::vector<u8>
encode_unwind_info(Context<E> &ctx, std::vector<Symbol<E> *> personalities,
                   std::vector<std::vector<UnwindRecord<E> *>> &pages,
                   i64 num_lsda) {
  // Compute the size of the buffer.
  i64 size = sizeof(UnwindSectionHeader) +
             personalities.size() * 4 +
             sizeof(UnwindFirstLevelPage) * (pages.size() + 1) +
             sizeof(UnwindSecondLevelPage) * pages.size() +
             sizeof(UnwindLsdaEntry) * num_lsda;

  for (std::span<UnwindRecord<E> *> span : pages)
    size += (sizeof(UnwindPageEntry) + 4) * span.size();

  // Allocate an output buffer.
  std::vector<u8> buf(size);

  // Write the section header.
  UnwindSectionHeader &uhdr = *(UnwindSectionHeader *)buf.data();
  uhdr.version = UNWIND_SECTION_VERSION;
  uhdr.encoding_offset = sizeof(uhdr);
  uhdr.encoding_count = 0;
  uhdr.personality_offset = sizeof(uhdr);
  uhdr.personality_count = personalities.size();
  uhdr.page_offset = sizeof(uhdr) + personalities.size() * 4;
  uhdr.page_count = pages.size() + 1;

  // Write the personalities
  ul32 *per = (ul32 *)(buf.data() + sizeof(uhdr));
  for (Symbol<E> *sym : personalities)
    *per++ = sym->get_got_addr(ctx);

  // Write first level pages, LSDA and second level pages
  UnwindFirstLevelPage *page1 = (UnwindFirstLevelPage *)per;
  UnwindLsdaEntry *lsda = (UnwindLsdaEntry *)(page1 + (pages.size() + 1));
  UnwindSecondLevelPage *page2 = (UnwindSecondLevelPage *)(lsda + num_lsda);

  for (std::span<UnwindRecord<E> *> span : pages) {
    page1->func_addr = span[0]->get_func_addr(ctx);
    page1->page_offset = (u8 *)page2 - buf.data();
    page1->lsda_offset = (u8 *)lsda - buf.data();

    for (UnwindRecord<E> *rec : span) {
      if (rec->lsda) {
        lsda->func_addr = rec->get_func_addr(ctx) - ctx.mach_hdr.hdr.addr;
        lsda->lsda_addr = rec->lsda->get_addr(ctx) + rec->lsda_offset -
                          ctx.mach_hdr.hdr.addr;
        lsda++;
      }
    }

    std::unordered_map<u32, u32> map;
    for (UnwindRecord<E> *rec : span)
      map.insert({rec->encoding, map.size()});

    page2->kind = UNWIND_SECOND_LEVEL_COMPRESSED;
    page2->page_offset = sizeof(UnwindSecondLevelPage);
    page2->page_count = span.size();

    UnwindPageEntry *entry = (UnwindPageEntry *)(page2 + 1);
    for (UnwindRecord<E> *rec : span) {
      entry->func_addr = rec->get_func_addr(ctx) - page1->func_addr;
      entry->encoding = map[rec->encoding];
      entry++;
    }

    page2->encoding_offset = (u8 *)entry - (u8 *)page2;
    page2->encoding_count = map.size();

    ul32 *encoding = (ul32 *)entry;
    for (std::pair<u32, u32> kv : map)
      encoding[kv.second] = kv.first;

    page1++;
    page2 = (UnwindSecondLevelPage *)(encoding + map.size());
  }

  // Write a terminator
  UnwindRecord<E> &last = *pages.back().back();
  page1->func_addr = last.subsec->get_addr(ctx) + last.subsec->input_size + 1;
  page1->page_offset = 0;
  page1->lsda_offset = (u8 *)lsda - buf.data();

  assert((u8 *)page2 <= buf.data() + buf.size());
  buf.resize((u8 *)page2 - buf.data());
  return buf;
}

// If two unwind records covers adjascent functions and have identical
// contents (i.e. have the same encoding, the same personality function
// and don't have LSDA), we can merge the two.
template <typename E>
static std::span<UnwindRecord<E> *>
merge_unwind_records(Context<E> &ctx, std::vector<UnwindRecord<E> *> &records) {
  auto can_merge = [&](UnwindRecord<E> &a, UnwindRecord<E> &b) {
    // As a special case, we don't merge unwind records with STACK_IND
    // encoding even if their encodings look the same. It is because the
    // real encoding for that record type is encoded in the instruction
    // stream and therefore the real encodings might be different.
    if constexpr (is_x86<E>)
      if ((a.encoding & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_STACK_IND ||
          (b.encoding & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_STACK_IND)
        return false;

    return a.get_func_addr(ctx) + a.code_len == b.get_func_addr(ctx) &&
           a.encoding == b.encoding &&
           a.personality == b.personality &&
           !a.lsda && !b.lsda;
  };

  assert(!records.empty());

  i64 i = 0;
  for (i64 j = 1; j < records.size(); j++) {
    if (can_merge(*records[i], *records[j]))
      records[i]->code_len += records[j]->code_len;
    else
      records[++i] = records[j];
  }
  return {&records[0], (size_t)(i + 1)};
}

// __unwind_info stores unwind records in two-level tables. The first-level
// table specifies the upper 12 bits of function addresses. The second-level
// page entries are 32-bits long and specifies the lower 24 bits of fucntion
// addresses along with indices to personality functions.
//
// This function splits a vector of unwind records into groups so that
// records in the same group share the first-level page table.
template <typename E>
static std::vector<std::vector<UnwindRecord<E> *>>
split_records(Context<E> &ctx, std::span<UnwindRecord<E> *> records) {
  constexpr i64 max_group_size = 200;
  std::vector<std::vector<UnwindRecord<E> *>> vec;

  while (!records.empty()) {
    u64 end_addr = records[0]->get_func_addr(ctx) + (1 << 24);
    i64 i = 1;
    while (i < records.size() && i < max_group_size &&
           records[i]->get_func_addr(ctx) < end_addr)
      i++;
    vec.push_back({records.begin(), records.begin() + i});
    records = records.subspan(i);
  }
  return vec;
}

template <typename E>
void UnwindInfoSection<E>::compute_size(Context<E> &ctx) {
  std::vector<UnwindRecord<E> *> records;

  for (std::unique_ptr<OutputSegment<E>> &seg : ctx.segments)
    for (Chunk<E> *chunk : seg->chunks)
      if (OutputSection<E> *osec = chunk->to_osec())
        for (Subsection<E> *subsec : osec->members)
          for (UnwindRecord<E> &rec : subsec->get_unwind_records())
            records.push_back(&rec);

  if (records.empty())
    return;

  auto encode_personality = [&](Symbol<E> *sym) -> u32 {
    for (i64 i = 0; i < personalities.size(); i++)
      if (personalities[i] == sym)
        return (i + 1) << std::countr_zero((u32)UNWIND_PERSONALITY_MASK);

    if (personalities.size() == 3)
      Fatal(ctx) << "too many personality functions";

    personalities.push_back(sym);
    return personalities.size() << std::countr_zero((u32)UNWIND_PERSONALITY_MASK);
  };

  for (UnwindRecord<E> *rec : records) {
    if (rec->personality)
      rec->encoding |= encode_personality(rec->personality);
    if (rec->lsda)
      num_lsda++;
  }

  tbb::parallel_sort(records,
                     [&](const UnwindRecord<E> *a, const UnwindRecord<E> *b) {
    return a->get_func_addr(ctx) < b->get_func_addr(ctx);
  });

  std::span<UnwindRecord<E> *> records2 = merge_unwind_records(ctx, records);
  pages = split_records(ctx, records2);

  this->hdr.size = encode_unwind_info(ctx, personalities, pages, num_lsda).size();
}

template <typename E>
void UnwindInfoSection<E>::copy_buf(Context<E> &ctx) {
  if (this->hdr.size == 0)
    return;

  std::vector<u8> vec = encode_unwind_info(ctx, personalities, pages, num_lsda);
  assert(this->hdr.size == vec.size());
  write_vector(ctx.buf + this->hdr.offset, vec);
}

template <typename E>
void GotSection<E>::add(Context<E> &ctx, Symbol<E> *sym) {
  if (sym->got_idx != -1)
    return;

  sym->got_idx = syms.size();
  syms.push_back(sym);
  this->hdr.size = syms.size() * sizeof(Word<E>);
}

template <typename E>
void GotSection<E>::copy_buf(Context<E> &ctx) {
  Word<E> *buf = (Word<E> *)(ctx.buf + this->hdr.offset);

  for (i64 i = 0; i < syms.size(); i++)
    if (!syms[i]->is_imported)
      buf[i] = syms[i]->get_addr(ctx);
}

template <typename E>
void LazySymbolPtrSection<E>::copy_buf(Context<E> &ctx) {
  Word<E> *buf = (Word<E> *)(ctx.buf + this->hdr.offset);

  for (i64 i = 0; i < ctx.stubs.syms.size(); i++)
    *buf++ = ctx.stub_helper->hdr.addr + E::stub_helper_hdr_size +
             E::stub_helper_size * i;
}

template <typename E>
void ThreadPtrsSection<E>::add(Context<E> &ctx, Symbol<E> *sym) {
  assert(sym->tlv_idx == -1);
  sym->tlv_idx = syms.size();
  syms.push_back(sym);
  this->hdr.size = syms.size() * sizeof(Word<E>);
}

template <typename E>
void ThreadPtrsSection<E>::copy_buf(Context<E> &ctx) {
  ul64 *buf = (ul64 *)(ctx.buf + this->hdr.offset);
  memset(buf, 0, this->hdr.size);

  for (i64 i = 0; i < syms.size(); i++)
    if (Symbol<E> &sym = *syms[i]; !sym.is_imported)
      buf[i] = sym.get_addr(ctx);
}

// Parse CIE augmented string
template <typename E>
void CieRecord<E>::parse(Context<E> &ctx) {
  std::string_view aug = get_contents().data() + 9;

  for (char c : aug) {
    switch (c) {
    case 'L':
      has_lsda = true;
      break;
    case 'z':
    case 'P':
    case 'R':
      break;
    default:
      Fatal(ctx) << *file << ": __eh_frame: unknown augmented string: " << aug;
    }
  }
}

template <typename E>
std::string_view CieRecord<E>::get_contents() const {
  const char *data = file->mf->get_contents().data() + file->eh_frame_sec->offset +
                     input_addr - file->eh_frame_sec->addr;
  return {data, (size_t)*(ul32 *)data + 4};
}

template <typename E>
void CieRecord<E>::copy_to(Context<E> &ctx) {
  u8 *buf = ctx.buf + ctx.eh_frame.hdr.offset;

  std::string_view data = get_contents();
  memcpy(buf + output_offset, data.data(), data.size());

  if (personality) {
    i64 offset = output_offset + personality_offset;
    *(ul32 *)(buf + offset) =
      personality->get_got_addr(ctx) - (ctx.eh_frame.hdr.addr + offset);
  }
}

template <typename E>
std::string_view FdeRecord<E>::get_contents(ObjectFile<E> &file) const {
  const char *data = file.mf->get_contents().data() + file.eh_frame_sec->offset +
                     input_addr - file.eh_frame_sec->addr;
  return {data, (size_t)*(ul32 *)data + 4};
}

template <typename E>
void FdeRecord<E>::copy_to(Context<E> &ctx, ObjectFile<E> &file) {
  u8 *buf = ctx.buf + ctx.eh_frame.hdr.offset + output_offset;

  // Copy FDE contents
  std::string_view data = get_contents(file);
  memcpy(buf, data.data(), data.size());

  // Relocate CIE offset
  auto find_cie = [&](i64 addr) {
    for (CieRecord<E> &cie : file.cies)
      if (cie.input_addr == addr)
        return &cie;
    Fatal(ctx) << file << ": cannot find a CIE for a FDE at address 0x"
               << std::hex << input_addr;
  };

  i64 cie_offset = *(ul32 *)(data.data() + 4);
  CieRecord<E> *cie = find_cie(input_addr + 4 - cie_offset);

  *(ul32 *)(buf + 4) = output_offset + 4 - cie->output_offset;

  // Relocate function start address
  u64 output_addr = ctx.eh_frame.hdr.addr + output_offset;
  *(ul64 *)(buf + 8) = (i32)(func->get_addr(ctx) - output_addr - 8);

  if (cie->has_lsda) {
    u8 *aug = buf + 24;
    read_uleb(aug); // skip Augmentation Data Length

    i64 offset = aug - buf;
    u64 addr = *(ul32 *)aug + input_addr + offset;

    Subsection<E> *lsda = file.find_subsection(ctx, addr);
    if (!lsda)
      Fatal(ctx) << file << ": cannot find a LSDA for a FDE at address 0x"
                 << std::hex << input_addr;

    *(ul32 *)aug = lsda->get_addr(ctx) - output_addr - offset +
                   addr - lsda->input_addr;
  }
}

template <typename E>
void EhFrameSection<E>::compute_size(Context<E> &ctx) {
  // Remove FDEs pointing to dead functions or functions that already
  // have compact unwind info.
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    std::erase_if(file->fdes, [](FdeRecord<E> &fde) {
      return !fde.func->is_alive || fde.func->nunwind;
    });
  });

  i64 offset = 0;

  for (ObjectFile<E> *file : ctx.objs) {
    if (file->fdes.empty())
      continue;

    for (CieRecord<E> &cie : file->cies) {
      cie.output_offset = offset;
      offset += cie.size();
    }

    for (FdeRecord<E> &fde : file->fdes) {
      fde.output_offset = offset;
      offset += fde.size(*file);
    }
  }

  this->hdr.size = offset;
}

template <typename E>
void EhFrameSection<E>::copy_buf(Context<E> &ctx) {
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    if (file->fdes.empty())
      return;

    for (CieRecord<E> &cie : file->cies)
      cie.copy_to(ctx);

    for (FdeRecord<E> &fde : file->fdes)
      fde.copy_to(ctx, *file);
  });
}

template <typename E>
SectCreateSection<E>::SectCreateSection(Context<E> &ctx, std::string_view seg,
                                        std::string_view sect,
                                        std::string_view contents)
  : Chunk<E>(ctx, seg, sect), contents(contents) {
  this->hdr.size = contents.size();
  ctx.chunk_pool.emplace_back(this);
}

template <typename E>
void SectCreateSection<E>::copy_buf(Context<E> &ctx) {
  write_string(ctx.buf + this->hdr.offset, contents);
}

using E = MOLD_TARGET;

template class OutputSegment<E>;
template class OutputMachHeader<E>;
template class OutputSection<E>;
template class RebaseSection<E>;
template class BindSection<E>;
template class LazyBindSection<E>;
template class ExportSection<E>;
template class FunctionStartsSection<E>;
template class SymtabSection<E>;
template class StrtabSection<E>;
template class IndirectSymtabSection<E>;
template class ObjcStubsSection<E>;
template class InitOffsetsSection<E>;
template class CodeSignatureSection<E>;
template class ObjcImageInfoSection<E>;
template class DataInCodeSection<E>;
template class ChainedFixupsSection<E>;
template class StubsSection<E>;
template class StubHelperSection<E>;
template class UnwindInfoSection<E>;
template class GotSection<E>;
template class LazySymbolPtrSection<E>;
template class ThreadPtrsSection<E>;
template class CieRecord<E>;
template class FdeRecord<E>;
template class EhFrameSection<E>;
template class SectCreateSection<E>;

} // namespace mold::macho
