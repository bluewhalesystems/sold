#include "mold.h"
#include "../common/archive-file.h"

#include <regex>

namespace mold::macho {

template <typename E>
std::ostream &operator<<(std::ostream &out, const InputFile<E> &file) {
  if (file.archive_name.empty())
    out << path_clean(file.filename);
  else
    out << path_clean(file.archive_name) << "(" << path_clean(file.filename) + ")";
  return out;
}

template <typename E>
void InputFile<E>::clear_symbols() {
  for (Symbol<E> *sym : syms) {
    if (__atomic_load_n(&sym->file, __ATOMIC_ACQUIRE) == this) {
      sym->visibility = SCOPE_LOCAL;
      sym->is_imported = false;
      sym->is_exported = false;
      sym->is_common = false;
      sym->is_weak = false;
      sym->is_abs = false;
      sym->is_tlv = false;
      sym->no_dead_strip = false;
      sym->subsec = nullptr;
      sym->value = 0;
      __atomic_store_n(&sym->file, nullptr, __ATOMIC_RELEASE);
    }
  }
}

template <typename E>
ObjectFile<E> *
ObjectFile<E>::create(Context<E> &ctx, MappedFile<Context<E>> *mf,
                      std::string archive_name) {
  ObjectFile<E> *obj = new ObjectFile<E>(mf);
  obj->archive_name = archive_name;
  obj->is_alive = archive_name.empty() || ctx.reader.all_load;
  obj->is_hidden = ctx.reader.hidden;
  ctx.obj_pool.emplace_back(obj);
  return obj;
};

template <typename E>
void ObjectFile<E>::parse(Context<E> &ctx) {
  if (get_file_type(ctx, this->mf) == FileType::LLVM_BITCODE) {
    // Open an compiler IR file
    load_lto_plugin(ctx);

    // It looks like module_create_from_memory is not thread-safe,
    // so protect it with a lock.
    {
      static std::mutex mu;
      std::scoped_lock lock(mu);

      this->lto_module =
        ctx.lto.module_create_from_memory(this->mf->data, this->mf->size);
      if (!this->lto_module)
        Fatal(ctx) << *this << ": lto_module_create_from_memory failed";
    }

    // Read a symbol table
    parse_lto_symbols(ctx);
    return;
  }

  if (SymtabCommand *cmd = (SymtabCommand *)find_load_command(ctx, LC_SYMTAB))
    mach_syms = {(MachSym<E> *)(this->mf->data + cmd->symoff), cmd->nsyms};

  parse_sections(ctx);

  if (MachHeader &hdr = *(MachHeader *)this->mf->data;
      hdr.flags & MH_SUBSECTIONS_VIA_SYMBOLS) {
    split_subsections_via_symbols(ctx);
  } else {
    init_subsections(ctx);
  }

  split_cstring_literals(ctx);
  split_fixed_size_literals(ctx);
  split_literal_pointers(ctx);

  sort(subsections, [](Subsection<E> *a, Subsection<E> *b) {
    return a->input_addr < b->input_addr;
  });

  parse_symbols(ctx);

  for (std::unique_ptr<InputSection<E>> &isec : sections)
    if (isec)
      isec->parse_relocations(ctx);

  if (unwind_sec)
    parse_compact_unwind(ctx);

  if (eh_frame_sec)
    parse_eh_frame(ctx);

  associate_compact_unwind(ctx);

  if (mod_init_func)
    parse_mod_init_func(ctx);

  if (debug_info)
    source_name = get_source_filename(ctx, *this);
}

template <typename E>
void ObjectFile<E>::parse_sections(Context<E> &ctx) {
  SegmentCommand<E> *cmd =
    (SegmentCommand<E> *)find_load_command(ctx, LC_SEGMENT_64);
  if (!cmd)
    return;

  MachSection<E> *mach_sec = (MachSection<E> *)((u8 *)cmd + sizeof(*cmd));
  sections.resize(cmd->nsects);

  for (i64 i = 0; i < cmd->nsects; i++) {
    MachSection<E> &msec = mach_sec[i];
    u8 *contents = (u8 *)(this->mf->get_contents().data() + msec.offset);

    if (msec.match("__LD", "__compact_unwind")) {
      unwind_sec = &msec;
      continue;
    }


    if (msec.match("__TEXT", "__eh_frame")) {
      eh_frame_sec = &msec;
      continue;
    }

    if (msec.match("__DATA", "__objc_imageinfo") ||
        msec.match("__DATA_CONST", "__objc_imageinfo")) {
      if (msec.size != sizeof(ObjcImageInfo))
        Fatal(ctx) << *this << ": __objc_imageinfo: invalid size";

      objc_image_info = (ObjcImageInfo *)contents;

      if (objc_image_info->version != 0)
        Fatal(ctx) << *this << ": __objc_imageinfo: unknown version: "
                   << (u32)objc_image_info->version;
      continue;
    }

    if (ctx.arg.init_offsets && msec.type == S_MOD_INIT_FUNC_POINTERS) {
      mod_init_func = &msec;
      continue;
    }

    if (msec.match("__DWARF", "__debug_info"))
      debug_info = contents;
    if (msec.match("__DWARF", "__debug_abbrev"))
      debug_abbrev = contents;
    if (msec.match("__DWARF", "__debug_str"))
      debug_str = contents;
    if (msec.match("__DWARF", "__debug_line"))
      debug_line = contents;

    if (msec.get_segname() == "__LLVM" || (msec.attr & S_ATTR_DEBUG))
      continue;

    sections[i].reset(new InputSection<E>(ctx, *this, msec, i));
  }
}

template <typename E>
static bool always_split(InputSection<E> &isec) {
  if (isec.hdr.match("__TEXT", "__eh_frame"))
    return true;

  u32 ty = isec.hdr.type;
  return ty == S_4BYTE_LITERALS  || ty == S_8BYTE_LITERALS   ||
         ty == S_16BYTE_LITERALS || ty == S_LITERAL_POINTERS ||
         ty == S_CSTRING_LITERALS;
}

template <typename E>
void ObjectFile<E>::split_subsections_via_symbols(Context<E> &ctx) {
  struct MachSymOff {
    MachSym<E> *msym;
    i64 symidx;
  };

  std::vector<MachSymOff> msyms;

  // Find all symbols whose type is N_SECT.
  for (i64 i = 0; i < mach_syms.size(); i++)
    if (MachSym<E> &msym = mach_syms[i];
        !msym.stab && msym.type == N_SECT && sections[msym.sect - 1])
      msyms.push_back({&msym, i});

  // Sort by address
  sort(msyms, [](const MachSymOff &a, const MachSymOff &b) {
    return std::tuple(a.msym->sect, a.msym->value) <
           std::tuple(b.msym->sect, b.msym->value);
  });

  sym_to_subsec.resize(mach_syms.size());

  // Split each input section
  for (i64 i = 0; i < sections.size(); i++) {
    InputSection<E> *isec = sections[i].get();
    if (!isec || always_split(*isec))
      continue;

    // We start with one big subsection and split it as we process symbols
    auto add_subsec = [&](u32 addr) {
      Subsection<E> *subsec = new Subsection<E>{
        .isec = isec,
        .input_addr = addr,
        .input_size = (u32)(isec->hdr.addr + isec->hdr.size - addr),
        .p2align = (u8)isec->hdr.p2align,
        .is_alive = !ctx.arg.dead_strip,
      };
      subsec_pool.emplace_back(subsec);
      subsections.push_back(subsec);
    };

    add_subsec(isec->hdr.addr);

    // Find the symbols in the given section
    struct Less {
      bool operator()(MachSymOff &m, i64 idx) { return m.msym->sect < idx; }
      bool operator()(i64 idx, MachSymOff &m) { return idx < m.msym->sect; }
    };

    auto [it, end] = std::equal_range(msyms.begin(), msyms.end(), i + 1, Less{});

    for (; it != end; it++) {
      // Split the last subsection into two with a symbol without N_ALT_ENTRY
      // as a boundary. We don't want to create an empty subsection if there
      // are two symbols at the same address.
      MachSymOff &m = *it;

      if (!(m.msym->desc & N_ALT_ENTRY)) {
        Subsection<E> &last = *subsections.back();
        i64 size1 = (i64)m.msym->value - (i64)last.input_addr;
        i64 size2 = (i64)isec->hdr.addr + (i64)isec->hdr.size - (i64)m.msym->value;
        if (size1 > 0 && size2 > 0) {
          last.input_size = size1;
          add_subsec(m.msym->value);
        }
      }
      sym_to_subsec[m.symidx] = subsections.back();
    }
  }
}

// If a section is not splittable (i.e. doesn't have the
// MH_SUBSECTIONS_VIA_SYMBOLS bit), we create one subsection for it
// and let it cover the entire section.
template <typename E>
void ObjectFile<E>::init_subsections(Context<E> &ctx) {
  subsections.resize(sections.size());

  for (i64 i = 0; i < sections.size(); i++) {
    InputSection<E> *isec = sections[i].get();
    if (!isec || always_split(*isec))
      continue;

    Subsection<E> *subsec = new Subsection<E>{
      .isec = isec,
      .input_addr = (u32)isec->hdr.addr,
      .input_size = (u32)isec->hdr.size,
      .p2align = (u8)isec->hdr.p2align,
      .is_alive = !ctx.arg.dead_strip,
    };
    subsec_pool.emplace_back(subsec);
    subsections[i] = subsec;
  }

  sym_to_subsec.resize(mach_syms.size());

  for (i64 i = 0; i < mach_syms.size(); i++) {
    MachSym<E> &msym = mach_syms[i];
    if (!msym.stab && msym.type == N_SECT)
      sym_to_subsec[i] = subsections[msym.sect - 1];
  }

  std::erase(subsections, nullptr);
}

// Split __cstring section.
template <typename E>
void ObjectFile<E>::split_cstring_literals(Context<E> &ctx) {
  for (std::unique_ptr<InputSection<E>> &isec : sections) {
    if (!isec || isec->hdr.type != S_CSTRING_LITERALS)
      continue;

    std::string_view str = isec->contents;
    size_t pos = 0;

    while (pos < str.size()) {
      size_t end = str.find('\0', pos);
      if (end == str.npos)
        Fatal(ctx) << *this << " corruupted cstring section: " << *isec;

      end = str.find_first_not_of('\0', end);
      if (end == str.npos)
        end = str.size();

      // A constant string in __cstring has no alignment info, so we
      // need to infer it.
      Subsection<E> *subsec = new Subsection<E>{
        .isec = &*isec,
        .input_addr = (u32)(isec->hdr.addr + pos),
        .input_size = (u32)(end - pos),
        .p2align = std::min<u8>(isec->hdr.p2align, std::countr_zero(pos)),
        .is_alive = !ctx.arg.dead_strip,
      };

      subsec_pool.emplace_back(subsec);
      subsections.push_back(subsec);
      pos = end;
    }
  }
}

// Split S_{4,8,16}BYTE_LITERALS sections
template <typename E>
void ObjectFile<E>::split_fixed_size_literals(Context<E> &ctx) {
  auto split = [&](InputSection<E> &isec, u32 size) {
    if (isec.contents.size() % size)
      Fatal(ctx) << *this << ": invalid literals section";

    for (i64 pos = 0; pos < isec.contents.size(); pos += size) {
      Subsection<E> *subsec = new Subsection<E>{
        .isec = &isec,
        .input_addr = (u32)(isec.hdr.addr + pos),
        .input_size = size,
        .p2align = (u8)std::countr_zero(size),
        .is_alive = !ctx.arg.dead_strip,
      };

      subsec_pool.emplace_back(subsec);
      subsections.push_back(subsec);
    }
  };

  for (std::unique_ptr<InputSection<E>> &isec : sections) {
    if (!isec)
      continue;

    switch (isec->hdr.type) {
    case S_4BYTE_LITERALS:
      split(*isec, 4);
      break;
    case S_8BYTE_LITERALS:
      split(*isec, 8);
      break;
    case S_16BYTE_LITERALS:
      split(*isec, 16);
      break;
    }
  }
}

// Split S_LITERAL_POINTERS sections such as __DATA,__objc_selrefs.
template <typename E>
void ObjectFile<E>::split_literal_pointers(Context<E> &ctx) {
  for (std::unique_ptr<InputSection<E>> &isec : sections) {
    if (!isec || isec->hdr.type != S_LITERAL_POINTERS)
      continue;

    std::string_view str = isec->contents;
    assert(str.size() % sizeof(Word<E>) == 0);

    for (i64 pos = 0; pos < str.size(); pos += sizeof(Word<E>)) {
      Subsection<E> *subsec = new Subsection<E>{
        .isec = &*isec,
        .input_addr = (u32)(isec->hdr.addr + pos),
        .input_size = sizeof(Word<E>),
        .p2align = (u8)std::countr_zero(sizeof(Word<E>)),
        .is_alive = !ctx.arg.dead_strip,
      };

      subsec_pool.emplace_back(subsec);
      subsections.push_back(subsec);
    }
  }
}

template <typename E>
void ObjectFile<E>::parse_symbols(Context<E> &ctx) {
  SymtabCommand *cmd = (SymtabCommand *)find_load_command(ctx, LC_SYMTAB);
  this->syms.reserve(mach_syms.size());

  i64 nlocal = 0;
  for (MachSym<E> &msym : mach_syms)
    if (!msym.is_extern)
      nlocal++;
  local_syms.reserve(nlocal);

  for (i64 i = 0; i < mach_syms.size(); i++) {
    MachSym<E> &msym = mach_syms[i];
    std::string_view name = (char *)(this->mf->data + cmd->stroff + msym.stroff);

    // Global symbol
    if (msym.is_extern) {
      this->syms.push_back(get_symbol(ctx, name));
      continue;
    }

    // Local symbol
    local_syms.emplace_back(name);
    Symbol<E> &sym = local_syms.back();
    this->syms.push_back(&sym);

    sym.file = this;
    sym.visibility = SCOPE_LOCAL;
    sym.no_dead_strip = (msym.desc & N_NO_DEAD_STRIP);

    if (msym.type == N_ABS) {
      sym.value = msym.value;
      sym.is_abs = true;
    } else if (!msym.stab && msym.type == N_SECT) {
      sym.subsec = sym_to_subsec[i];
      if (!sym.subsec)
        sym.subsec = find_subsection(ctx, msym.value);

      // Subsec is null if a symbol is in a __compact_unwind.
      if (sym.subsec) {
        sym.value = msym.value - sym.subsec->input_addr;
        sym.is_tlv = (sym.subsec->isec->hdr.type == S_THREAD_LOCAL_VARIABLES);
      } else {
        sym.value = msym.value;
      }
    }
  }
}

// A Mach-O object file may contain command line option-like directives
// such as "-lfoo" in its LC_LINKER_OPTION command. This function returns
// such directives.
template <typename E>
std::vector<std::string> ObjectFile<E>::get_linker_options(Context<E> &ctx) {
  if (get_file_type(ctx, this->mf) == FileType::LLVM_BITCODE)
    return {};

  MachHeader &hdr = *(MachHeader *)this->mf->data;
  u8 *p = this->mf->data + sizeof(hdr);
  std::vector<std::string> vec;

  for (i64 i = 0; i < hdr.ncmds; i++) {
    LoadCommand &lc = *(LoadCommand *)p;
    p += lc.cmdsize;

    if (lc.cmd == LC_LINKER_OPTION) {
      LinkerOptionCommand *cmd = (LinkerOptionCommand *)&lc;
      char *buf = (char *)cmd + sizeof(*cmd);
      for (i64 i = 0; i < cmd->count; i++) {
        vec.push_back(buf);
        buf += vec.back().size() + 1;
      }
    }
  }
  return vec;
}

template <typename E>
LoadCommand *ObjectFile<E>::find_load_command(Context<E> &ctx, u32 type) {
  if (!this->mf)
    return nullptr;

  MachHeader &hdr = *(MachHeader *)this->mf->data;
  u8 *p = this->mf->data + sizeof(hdr);

  for (i64 i = 0; i < hdr.ncmds; i++) {
    LoadCommand &lc = *(LoadCommand *)p;
    if (lc.cmd == type)
      return &lc;
    p += lc.cmdsize;
  }
  return nullptr;
}

template <typename E>
Subsection<E> *
ObjectFile<E>::find_subsection(Context<E> &ctx, u32 addr) {
  assert(subsections.size() > 0);

  auto it = std::partition_point(subsections.begin(), subsections.end(),
                                 [&](Subsection<E> *subsec) {
    return subsec->input_addr <= addr;
  });

  if (it == subsections.begin())
    return nullptr;
  return *(it - 1);
}

// __compact_unwind consitss of fixed-sized records so-called compact
// unwinding records. There is usually a compact unwinding record for each
// function, and the record explains how to handle exceptions for that
// function.
//
// Output file's __compact_unwind contains not only unwinding records but
// also contains a two-level lookup table to quickly find out an unwinding
// record for a given function address. When an exception is thrown at
// runtime, the runtime looks up the table with the current program
// counter as a key to find out an unwinding record to know how to handle
// the exception.
//
// In order to construct the lookup table, we need to parse input files'
// unwinding records. The following function does that.
template <typename E>
void ObjectFile<E>::parse_compact_unwind(Context<E> &ctx) {
  MachSection<E> &hdr = *unwind_sec;

  if (hdr.size % sizeof(CompactUnwindEntry<E>))
    Fatal(ctx) << *this << ": invalid __compact_unwind section size";

  i64 num_entries = hdr.size / sizeof(CompactUnwindEntry<E>);
  unwind_records.reserve(num_entries);

  CompactUnwindEntry<E> *src =
    (CompactUnwindEntry<E> *)(this->mf->data + hdr.offset);

  // Read compact unwind entries
  for (i64 i = 0; i < num_entries; i++) {
    unwind_records.push_back(UnwindRecord<E>{
      .code_len = src[i].code_len,
      .encoding = src[i].encoding,
    });
  }

  auto find_symbol = [&](u32 addr) -> Symbol<E> * {
    for (i64 i = 0; i < mach_syms.size(); i++)
      if (MachSym<E> &msym = mach_syms[i]; msym.is_extern && msym.value == addr)
        return this->syms[i];
    return nullptr;
  };

  // Read relocations
  MachRel *mach_rels = (MachRel *)(this->mf->data + hdr.reloff);
  for (i64 i = 0; i < hdr.nreloc; i++) {
    MachRel &r = mach_rels[i];
    if (r.offset >= hdr.size)
      Fatal(ctx) << *this << ": relocation offset too large: " << i;

    i64 idx = r.offset / sizeof(CompactUnwindEntry<E>);
    UnwindRecord<E> &dst = unwind_records[idx];

    auto error = [&] {
      Fatal(ctx) << *this << ": __compact_unwind: unsupported relocation: " << i
                 << " " << *this->syms[r.idx];
    };

    if ((1 << r.p2size) != sizeof(Word<E>) || r.type != E::abs_rel)
      error();

    switch (r.offset % sizeof(CompactUnwindEntry<E>)) {
    case offsetof(CompactUnwindEntry<E>, code_start): {
      Subsection<E> *target;
      if (r.is_extern) {
        dst.subsec = sym_to_subsec[r.idx];
        dst.input_offset = src[idx].code_start;
      } else {
        dst.subsec = find_subsection(ctx, src[idx].code_start);
        dst.input_offset = dst.subsec->input_addr - src[idx].code_start;
      }

      if (!dst.subsec)
        error();
      break;
    }
    case offsetof(CompactUnwindEntry<E>, personality):
      if (r.is_extern) {
        dst.personality = this->syms[r.idx];
      } else {
        u32 addr = *(ul32 *)(this->mf->data + hdr.offset + r.offset);
        dst.personality = find_symbol(addr);
      }

      if (!dst.personality)
        Fatal(ctx) << *this << ": __compact_unwind: unsupported "
                   << "personality reference: " << i;
      break;
    case offsetof(CompactUnwindEntry<E>, lsda): {
      u32 addr = *(ul32 *)((u8 *)this->mf->data + hdr.offset + r.offset);

      if (r.is_extern) {
        dst.lsda = sym_to_subsec[r.idx];
        dst.lsda_offset = addr;
      } else {
        dst.lsda = find_subsection(ctx, addr);
        if (!dst.lsda)
          error();
        dst.lsda_offset = addr - dst.lsda->input_addr;
      }
      break;
    }
    default:
      error();
    }
  }

  // We want to ignore compact unwind records that point to DWARF unwind
  // info because we synthesize them ourselves. Object files usually don't
  // contain such records, but `ld -r` often produces them.
  std::erase_if(unwind_records, [](UnwindRecord<E> &rec) {
    return (rec.encoding & UNWIND_MODE_MASK) == E::unwind_mode_dwarf;
  });

  for (UnwindRecord<E> &rec : unwind_records) {
    if (!rec.subsec)
      Fatal(ctx) << *this << ": __compact_unwind: missing relocation at offset 0x"
                 << std::hex << rec.input_offset;
    rec.subsec->has_compact_unwind = true;
  }
}

// __eh_frame contains variable-sized records called CIE and FDE.
// In an __eh_frame section, there's usually one CIE record followed
// by as many FDE records as the number of functions defined by the
// same input file.
//
// A CIE usually contains one PC-relative GOT-referencing relocation.
// FDE usually contains no relocations. However, object files created
// by `ld -r` contains many relocations for __eh_frame.
//
// This function applies relocations against __eh_frame input sections
// so that all __eh_frame contains only one relocation for an CIE and
// no relocation for FDEs.
template <typename E>
static void apply_eh_frame_relocs(Context<E> &ctx, ObjectFile<E> &file) {
  MachSection<E> &msec = *file.eh_frame_sec;
  u8 *buf = (u8 *)file.mf->get_contents().data() + msec.offset;
  MachRel *mach_rels = (MachRel *)(file.mf->data + msec.reloff);

  for (i64 i = 0; i < msec.nreloc; i++) {
    MachRel &r1 = mach_rels[i];

    switch (r1.type) {
    case E::subtractor_rel: {
      if (i + 1 == msec.nreloc)
        Fatal(ctx) << file << ": __eh_frame: invalid subtractor reloc";

      MachRel &r2 = mach_rels[++i];
      if (r2.type != E::abs_rel)
        Fatal(ctx) << file << ": __eh_frame: invalid subtractor reloc pair";

      u32 target1 = r1.is_extern ? file.mach_syms[r1.idx].value : r1.idx;
      u32 target2 = r2.is_extern ? file.mach_syms[r2.idx].value : r2.idx;

      if (r1.p2size == 2)
        *(ul32 *)(buf + r1.offset) += target2 - target1;
      else if (r1.p2size == 3)
        *(ul64 *)(buf + r1.offset) += (i32)(target2 - target1);
      else
        Fatal(ctx) << file << ": __eh_frame: invalid p2size";
      break;
    }
    case E::gotpc_rel:
      break;
    default:
      Fatal(ctx) << file << ": unknown relocation type";
    }
  }
}

template <typename E>
void ObjectFile<E>::parse_eh_frame(Context<E> &ctx) {
  apply_eh_frame_relocs(ctx, *this);

  const char *start = this->mf->get_contents().data() + eh_frame_sec->offset;
  std::string_view data(start, eh_frame_sec->size);

  // Split section contents into CIE and FDE records
  while (!data.empty()) {
    u32 len = *(ul32 *)data.data();
    if (len == 0xffff'ffff)
      Fatal(ctx) << *this
                 << ": __eh_frame record with an extended length is not supported";

    u32 offset = data.data() - start;

    u32 id = *(ul32 *)(data.data() + 4);
    if (id == 0) {
      cies.emplace_back(new CieRecord<E>{
        .file = this,
        .input_addr = (u32)(eh_frame_sec->addr + offset),
      });
    } else {
      u64 addr = *(il64 *)(data.data() + 8) + eh_frame_sec->addr + offset + 8;
      Subsection<E> *subsec = find_subsection(ctx, addr);
      if (!subsec)
        Fatal(ctx) << *this << ": __unwind_info: FDE with invalid function"
                   << " reference at 0x" << std::hex << offset;

      if (!subsec->has_compact_unwind) {
        fdes.push_back(FdeRecord<E>{
          .subsec = subsec,
          .input_addr = (u32)(eh_frame_sec->addr + offset),
       });
      }
    }

    data = data.substr(len + 4);
  }

  sort(fdes, [](const FdeRecord<E> &a, const FdeRecord<E> &b) {
    return a.subsec->input_addr < b.subsec->input_addr;
  });

  for (std::unique_ptr<CieRecord<E>> &cie : cies)
    cie->parse(ctx);
  for (FdeRecord<E> &fde : fdes)
    fde.parse(ctx);

  MachRel *mach_rels = (MachRel *)(this->mf->data + eh_frame_sec->reloff);

  auto find_cie = [&](u32 input_addr) -> CieRecord<E> * {
    for (std::unique_ptr<CieRecord<E>> &cie : cies)
      if (cie->input_addr <= input_addr &&
          input_addr < cie->input_addr + cie->size())
        return &*cie;
    Fatal(ctx) << *this << ": __eh_frame: unexpected relocation offset";
  };

  for (i64 i = 0; i < eh_frame_sec->nreloc; i++) {
    MachRel &r = mach_rels[i];
    if (r.type != E::gotpc_rel)
      continue;

    if (r.p2size != 2)
      Fatal(ctx) << *this << ": __eh_frame: unexpected p2size";
    if (!r.is_extern)
      Fatal(ctx) << *this << ": __eh_frame: unexpected is_extern value";

    CieRecord<E> *cie = find_cie(eh_frame_sec->addr + r.offset);
    cie->personality = this->syms[r.idx];
    cie->personality_offset = eh_frame_sec->addr + r.offset - cie->input_addr;
  }
}

template <typename E>
void ObjectFile<E>::associate_compact_unwind(Context<E> &ctx) {
  // If a subsection has a DWARF unwind info, we need to create a compact
  // unwind record that points to it.
  for (FdeRecord<E> &fde : fdes) {
    unwind_records.push_back(UnwindRecord<E>{
      .subsec = fde.subsec,
      .fde = &fde,
      .input_offset = 0,
      .code_len = fde.subsec->input_size,
    });
  }

  // Sort unwind entries by offset
  sort(unwind_records, [](const UnwindRecord<E> &a, const UnwindRecord<E> &b) {
    return std::tuple(a.subsec->input_addr, a.input_offset) <
           std::tuple(b.subsec->input_addr, b.input_offset);
  });

  // Associate unwind entries to subsections
  for (i64 i = 0, end = unwind_records.size(); i < end;) {
    Subsection<E> &subsec = *unwind_records[i].subsec;
    subsec.unwind_offset = i;

    i64 j = i + 1;
    while (j < end && unwind_records[j].subsec == &subsec)
      j++;
    subsec.nunwind = j - i;
    i = j;
  }
}

// __mod_init_func section contains pointers to glolbal initializers, e.g.
// functions that have to run before main().
//
// We can just copy input __mod_init_func sections into an output
// __mod_init_func. In this case, the output consists of absolute
// addresses of functions, which needs base relocation for PIE.
//
// If -init_offset is given, we translate __mod_init_func to __init_offset,
// which contains 32-bit offsets from the image base to initializer functions.
// __init_offset and __mod_init_func are functionally the same, but the
// former doesn't need to be base-relocated and thus a bit more efficient.
template <typename E>
void ObjectFile<E>::parse_mod_init_func(Context<E> &ctx) {
  MachSection<E> &hdr = *mod_init_func;

  if (hdr.size % sizeof(Word<E>))
    Fatal(ctx) << *this << ": __mod_init_func: unexpected section size";

  MachRel *begin = (MachRel *)(this->mf->data + hdr.reloff);
  std::vector<MachRel> rels(begin, begin + hdr.nreloc);

  sort(rels, [](const MachRel &a, const MachRel &b) {
    return a.offset < b.offset;
  });

  for (i64 i = 0; i < rels.size(); i++) {
    MachRel r = rels[i];

    if (r.type != E::abs_rel)
      Fatal(ctx) << *this << ": __mod_init_func: unexpected relocation type";
    if (r.offset != i * sizeof(Word<E>))
      Fatal(ctx) << *this << ": __mod_init_func: unexpected relocation offset";
    if (!r.is_extern)
      Fatal(ctx) << *this << ": __mod_init_func: unexpected is_extern value";

    Symbol<E> *sym = this->syms[r.idx];

    if (sym->visibility != SCOPE_LOCAL)
      Fatal(ctx) << *this << ": __mod_init_func: non-local initializer function";

    init_functions.push_back(sym);
  }
}

// Symbols with higher priorities overwrites symbols with lower priorities.
// Here is the list of priorities, from the highest to the lowest.
//
//  1. Strong defined symbol
//  2. Weak defined symbol
//  3. Strong defined symbol in a DSO/archive
//  4. Weak Defined symbol in a DSO/archive
//  5. Common symbol
//  6. Common symbol in an archive
//  7. Unclaimed (nonexistent) symbol
//
// Ties are broken by file priority.
template <typename E>
static u64 get_rank(InputFile<E> *file, bool is_common, bool is_weak) {
  bool is_in_archive = !file->is_alive;

  auto get_sym_rank = [&] {
    if (is_common) {
      assert(!file->is_dylib);
      return is_in_archive ? 6 : 5;
    }

    if (file->is_dylib || is_in_archive)
      return is_weak ? 4 : 3;
    return is_weak ? 2 : 1;
  };

  return (get_sym_rank() << 24) + file->priority;
}

template <typename E>
static u64 get_rank(Symbol<E> &sym) {
  if (!sym.file)
    return 7 << 24;
  return get_rank(sym.file, sym.is_common, sym.is_weak);
}

template <typename E>
void ObjectFile<E>::resolve_symbols(Context<E> &ctx) {
  for (i64 i = 0; i < this->syms.size(); i++) {
    MachSym<E> &msym = mach_syms[i];

    if (!msym.is_extern || msym.is_undef())
      continue;

    // Global symbols in a discarded segment (i.e. __LLVM segment) are
    // silently ignored.
    if (msym.type == N_SECT && !sym_to_subsec[i])
      continue;

    Symbol<E> &sym = *this->syms[i];
    std::scoped_lock lock(sym.mu);
    bool is_weak = (msym.desc & N_WEAK_DEF);

    if (get_rank(this, msym.is_common(), is_weak) < get_rank(sym)) {
      sym.file = this;
      sym.visibility = SCOPE_MODULE;
      sym.is_weak = is_weak;
      sym.no_dead_strip = (msym.desc & N_NO_DEAD_STRIP);

      switch (msym.type) {
      case N_UNDF:
        assert(msym.is_common());
        sym.subsec = nullptr;
        sym.value = msym.value;
        sym.is_common = true;
        sym.is_abs = false;
        sym.is_tlv = false;
        break;
      case N_ABS:
        sym.subsec = nullptr;
        sym.value = msym.value;
        sym.is_common = false;
        sym.is_abs = true;
        sym.is_tlv = false;
        break;
      case N_SECT:
        sym.subsec = sym_to_subsec[i];
        sym.value = msym.value - sym.subsec->input_addr;
        sym.is_common = false;
        sym.is_abs = false;
        sym.is_tlv = (sym.subsec->isec->hdr.type == S_THREAD_LOCAL_VARIABLES);
        break;
      default:
        Fatal(ctx) << sym << ": unknown symbol type: " << (u64)msym.type;
      }
    }
  }
}

template <typename E>
bool ObjectFile<E>::is_objc_object(Context<E> &ctx) {
  for (std::unique_ptr<InputSection<E>> &isec : sections)
    if (isec)
      if (isec->hdr.match("__DATA", "__objc_catlist") ||
          (isec->hdr.get_segname() == "__TEXT" &&
           isec->hdr.get_sectname().starts_with("__swift")))
        return true;

  for (i64 i = 0; i < this->syms.size(); i++)
    if (!mach_syms[i].is_undef() && mach_syms[i].is_extern &&
        this->syms[i]->name.starts_with("_OBJC_CLASS_$_"))
      return true;

  return false;
}

template <typename E>
void
ObjectFile<E>::mark_live_objects(Context<E> &ctx,
                                 std::function<void(ObjectFile<E> *)> feeder) {
  assert(this->is_alive);

  auto is_module_local = [&](MachSym<E> &msym) {
    return this->is_hidden || msym.is_private_extern ||
           ((msym.desc & N_WEAK_REF) && (msym.desc & N_WEAK_DEF));
  };

  for (i64 i = 0; i < this->syms.size(); i++) {
    MachSym<E> &msym = mach_syms[i];
    if (!msym.is_extern)
      continue;

    Symbol<E> &sym = *this->syms[i];
    std::scoped_lock lock(sym.mu);

    // If at least one symbol defines it as an GLOBAL symbol, the result
    // is an GLOBAL symbol instead of MODULE, so that the symbol is exported.
    if (!msym.is_undef() && !is_module_local(msym))
      sym.visibility = SCOPE_GLOBAL;

    if (InputFile<E> *file = sym.file)
      if (msym.is_undef() || (msym.is_common() && !sym.is_common))
        if (!file->is_alive.test_and_set() && !file->is_dylib)
          feeder((ObjectFile<E> *)file);
  }

  for (Subsection<E> *subsec : subsections)
    for (UnwindRecord<E> &rec : subsec->get_unwind_records())
      if (Symbol<E> *sym = rec.personality)
        if (InputFile<E> *file = sym->file)
          if (!file->is_alive.test_and_set() && !file->is_dylib)
            feeder((ObjectFile<E> *)file);
}

template <typename E>
void ObjectFile<E>::convert_common_symbols(Context<E> &ctx) {
  for (i64 i = 0; i < this->mach_syms.size(); i++) {
    Symbol<E> &sym = *this->syms[i];
    MachSym<E> &msym = mach_syms[i];

    if (sym.file == this && sym.is_common) {
      InputSection<E> *isec = get_common_sec(ctx);
      Subsection<E> *subsec = new Subsection<E>{
        .isec = isec,
        .input_size = (u32)msym.value,
        .p2align = (u8)msym.common_p2align,
        .is_alive = !ctx.arg.dead_strip,
      };

      subsections.emplace_back(subsec);

      sym.is_weak = false;
      sym.no_dead_strip = (msym.desc & N_NO_DEAD_STRIP);
      sym.subsec = subsec;
      sym.value = 0;
      sym.is_common = false;
      sym.is_abs = false;
      sym.is_tlv = false;
    }
  }
}

template <typename E>
void ObjectFile<E>::check_duplicate_symbols(Context<E> &ctx) {
  for (i64 i = 0; i < this->mach_syms.size(); i++) {
    Symbol<E> *sym = this->syms[i];
    MachSym<E> &msym = mach_syms[i];
    if (sym && sym->file && sym->file != this && !msym.is_undef() &&
        !msym.is_common() && !(msym.desc & N_WEAK_DEF))
      Error(ctx) << "duplicate symbol: " << *this << ": " << *sym->file
                 << ": " << *sym;
  }
}

template <typename E>
InputSection<E> *ObjectFile<E>::get_common_sec(Context<E> &ctx) {
  if (!common_sec) {
    MachSection<E> *hdr = new MachSection<E>;
    common_hdr.reset(hdr);

    memset(hdr, 0, sizeof(*hdr));
    hdr->set_segname("__DATA");
    hdr->set_sectname("__common");
    hdr->type = S_ZEROFILL;

    common_sec = new InputSection<E>(ctx, *this, *hdr, sections.size());
    sections.emplace_back(common_sec);
  }
  return common_sec;
}

template <typename E>
void ObjectFile<E>::parse_lto_symbols(Context<E> &ctx) {
  i64 nsyms = ctx.lto.module_get_num_symbols(this->lto_module);
  this->syms.reserve(nsyms);
  this->mach_syms2.reserve(nsyms);

  for (i64 i = 0; i < nsyms; i++) {
    std::string_view name = ctx.lto.module_get_symbol_name(this->lto_module, i);
    this->syms.push_back(get_symbol(ctx, name));

    u32 attr = ctx.lto.module_get_symbol_attribute(this->lto_module, i);
    MachSym<E> msym = {};

    switch (attr & LTO_SYMBOL_DEFINITION_MASK) {
    case LTO_SYMBOL_DEFINITION_REGULAR:
    case LTO_SYMBOL_DEFINITION_TENTATIVE:
    case LTO_SYMBOL_DEFINITION_WEAK:
      msym.type = N_ABS;
      break;
    case LTO_SYMBOL_DEFINITION_UNDEFINED:
    case LTO_SYMBOL_DEFINITION_WEAKUNDEF:
      msym.type = N_UNDF;
      break;
    default:
      unreachable();
    }

    switch (attr & LTO_SYMBOL_SCOPE_MASK) {
    case 0:
    case LTO_SYMBOL_SCOPE_INTERNAL:
    case LTO_SYMBOL_SCOPE_HIDDEN:
      break;
    case LTO_SYMBOL_SCOPE_DEFAULT:
    case LTO_SYMBOL_SCOPE_PROTECTED:
    case LTO_SYMBOL_SCOPE_DEFAULT_CAN_BE_HIDDEN:
      msym.is_extern = true;
      break;
    default:
      unreachable();
    }

    mach_syms2.push_back(msym);
  }

  mach_syms = mach_syms2;
}

template <typename E>
std::string_view ObjectFile<E>::get_linker_optimization_hints(Context<E> &ctx) {
  LinkEditDataCommand *cmd =
    (LinkEditDataCommand *)find_load_command(ctx, LC_LINKER_OPTIMIZATION_HINT);

  if (cmd)
    return {(char *)this->mf->data + cmd->dataoff, cmd->datasize};
  return {};
}

// As a space optimization, Xcode 14 or later emits code to just call
// `_objc_msgSend$foo` to call `_objc_msgSend` function with a selector
// `foo`.
//
// It is now the linker's responsibility to synthesize code and data
// for undefined symbol of the form `_objc_msgSend$<method_name>`.
// To do so, we need to synthsize three subsections containing the following
// pieces of code/data:
//
//  1. `__objc_stubs`:    containing machine code to call `_objc_msgSend`
//  2. `__objc_methname`: containing a null-terminated method name string
//  3. `__objc_selrefs`:  containing a pointer to the method name string
template <typename E>
void ObjectFile<E>::add_msgsend_symbol(Context<E> &ctx, Symbol<E> &sym) {
  assert(this == ctx.internal_obj);

  std::string_view prefix = "_objc_msgSend$";
  assert(sym.name.starts_with(prefix));

  this->syms.push_back(&sym);
  sym.file = this;

  Subsection<E> *subsec = add_methname_string(ctx, sym.name.substr(prefix.size()));
  ctx.objc_stubs->methnames.push_back(subsec);
  ctx.objc_stubs->selrefs.push_back(add_selrefs(ctx, *subsec));
  ctx.objc_stubs->hdr.size += ObjcStubsSection<E>::ENTRY_SIZE;
}

template <typename E>
Subsection<E> *
ObjectFile<E>::add_methname_string(Context<E> &ctx, std::string_view contents) {
  assert(this == ctx.internal_obj);
  assert(contents[contents.size()] == '\0');

  u64 addr = 0;
  if (!sections.empty()) {
    const MachSection<E> &hdr = sections.back()->hdr;
    addr = hdr.addr + hdr.size;
  }

  // Create a dummy Mach-O section
  MachSection<E> *msec = new MachSection<E>;
  mach_sec_pool.emplace_back(msec);

  memset(msec, 0, sizeof(*msec));
  msec->set_segname("__TEXT");
  msec->set_sectname("__objc_methname");
  msec->addr = addr;
  msec->size = contents.size() + 1;
  msec->type = S_CSTRING_LITERALS;

  // Create a dummy InputSection
  InputSection<E> *isec = new InputSection<E>(ctx, *this, *msec, sections.size());
  sections.emplace_back(isec);
  isec->contents = contents;

  Subsection<E> *subsec = new Subsection<E>{
    .isec = isec,
    .input_addr = (u32)addr,
    .input_size = (u32)contents.size() + 1,
    .p2align = 0,
    .is_alive = !ctx.arg.dead_strip,
  };

  subsec_pool.emplace_back(subsec);
  subsections.push_back(subsec);
  return subsec;
}

template <typename E>
Subsection<E> *
ObjectFile<E>::add_selrefs(Context<E> &ctx, Subsection<E> &methname) {
  assert(this == ctx.internal_obj);

  // Create a dummy Mach-O section
  MachSection<E> *msec = new MachSection<E>;
  mach_sec_pool.emplace_back(msec);

  memset(msec, 0, sizeof(*msec));
  msec->set_segname("__DATA");
  msec->set_sectname("__objc_selrefs");
  msec->addr = sections.back()->hdr.addr + sections.back()->hdr.size,
  msec->size = sizeof(Word<E>);
  msec->type = S_LITERAL_POINTERS;
  msec->attr = S_ATTR_NO_DEAD_STRIP;

  // Create a dummy InputSection
  InputSection<E> *isec = new InputSection<E>(ctx, *this, *msec, sections.size());
  sections.emplace_back(isec);
  isec->contents = "\0\0\0\0\0\0\0\0"sv;

  // Create a dummy relocation
  isec->rels.push_back(Relocation<E>{
    .target = &methname,
    .offset = 0,
    .type = E::abs_rel,
    .size = (u8)sizeof(Word<E>),
    .is_sym = false,
  });

  // Create a dummy subsection
  Subsection<E> *subsec = new Subsection<E>{
    .isec = isec,
    .input_addr = (u32)msec->addr,
    .input_size = sizeof(Word<E>),
    .rel_offset = 0,
    .nrels = 1,
    .p2align = (u8)std::countr_zero(sizeof(Word<E>)),
    .is_alive = !ctx.arg.dead_strip,
  };

  subsec_pool.emplace_back(subsec);
  subsections.push_back(subsec);
  return subsec;
}

template <typename E>
void ObjectFile<E>::compute_symtab_size(Context<E> &ctx) {
  auto get_oso_name = [&]() -> std::string {
    if (!this->mf)
      return "<internal>";

    std::string name = path_clean(this->mf->name);
    if (!this->mf->parent) {
      if (name.starts_with('/'))
        return name;
      return ctx.cwd + "/" + name;
    }

    std::string parent = path_clean(this->mf->parent->name);
    if (parent.starts_with('/'))
      return parent + "(" + name + ")";
    return ctx.cwd + "/" + parent + "(" + name + ")";
  };

  // Debug symbols. Mach-O executables and dylibs generally don't directly
  // contain debug info records. Instead, they have only symbols that
  // specify function/global variable names and their addresses along with
  // pathnames to object files. The debugger reads the symbols on startup
  // and read debug info from object files.
  //
  // Debug symbols are called "stab" symbols.
  bool emit_debug_syms = debug_info && !ctx.arg.S;

  if (emit_debug_syms) {
    this->oso_name = get_oso_name();
    if (!ctx.arg.oso_prefix.empty() &&
        this->oso_name.starts_with(ctx.arg.oso_prefix))
      this->oso_name = this->oso_name.substr(ctx.arg.oso_prefix.size());

    this->strtab_size += this->source_name.size() + 1;
    this->strtab_size += this->oso_name.size() + 1;
    this->num_stabs = 3;
  }

  // Symbols copied from an input symtab to the output symtab
  for (Symbol<E> *sym : this->syms) {
    if (!sym || sym->file != this || (sym->subsec && !sym->subsec->is_alive))
      continue;

    // Symbols starting with l or L are compiler-generated private labels
    // that should be stripped from the symbol table.
    if (sym->name.starts_with('l') || sym->name.starts_with('L'))
      continue;

    if (ctx.arg.x && sym->visibility == SCOPE_LOCAL)
      continue;

    if (sym->is_imported)
      this->num_undefs++;
    else if (sym->visibility == SCOPE_GLOBAL)
      this->num_globals++;
    else
      this->num_locals++;

    if (emit_debug_syms && sym->subsec)
      this->num_stabs += sym->subsec->isec->hdr.is_text() ? 2 : 1;

    this->strtab_size += sym->name.size() + 1;
    sym->output_symtab_idx = -2;
  }
}

template <typename E>
void ObjectFile<E>::populate_symtab(Context<E> &ctx) {
  MachSym<E> *buf = (MachSym<E> *)(ctx.buf + ctx.symtab.hdr.offset);
  u8 *strtab = ctx.buf + ctx.strtab.hdr.offset;
  i64 stroff = this->strtab_offset;

  // Write to the string table
  std::vector<i32> pos(this->syms.size());

  for (i64 i = 0; i < this->syms.size(); i++) {
    Symbol<E> *sym = this->syms[i];
    if (sym && sym->file == this && sym->output_symtab_idx != -1) {
      pos[i] = stroff;
      stroff += write_string(strtab + stroff, sym->name);
    }
  }

  // Write debug symbols. A stab symbol of type N_SO with a nonempty name
  // marks a start of a new object file.
  //
  // The first N_SO symbol is intended to have a source filename (e.g.
  // path/too/foo.cc).
  //
  // The following N_OSO symbol specifies a object file path, which is
  // followed by N_FUN, N_STSYM or N_GSYM symbols for functions,
  // file-scope global variables and global variables, respectively.
  //
  // N_FUN symbol is always emitted as a pair. The first N_FUN symbol
  // specifies the start address of a function, and the second specifies
  // the size.
  //
  // At the end of stab symbols, we have a N_SO symbol without symbol name
  // as an end marker.
  if (debug_info && !ctx.arg.S) {
    MachSym<E> *stab = buf + this->stabs_offset;
    i64 stab_idx = 2;

    stab[0].stroff = stroff;
    stab[0].n_type = N_SO;
    stroff += write_string(strtab + stroff, this->source_name);

    stab[1].stroff = stroff;
    stab[1].n_type = N_OSO;
    stab[1].sect = E::cpusubtype;
    stab[1].desc = 1;
    stroff += write_string(strtab + stroff, this->oso_name);

    for (i64 i = 0; i < this->syms.size(); i++) {
      Symbol<E> *sym = this->syms[i];
      if (!sym || sym->file != this || sym->output_symtab_idx == -1 || !sym->subsec)
        continue;

      stab[stab_idx].stroff = pos[i];
      stab[stab_idx].sect = sym->subsec->isec->osec.sect_idx;
      stab[stab_idx].value = sym->get_addr(ctx);

      if (sym->subsec->isec->hdr.is_text()) {
        stab[stab_idx].n_type = N_FUN;
        stab[stab_idx + 1].stroff = 1; // empty string
        stab[stab_idx + 1].n_type = N_FUN;
        stab[stab_idx + 1].value = sym->subsec->input_size;
        stab_idx += 2;
      } else {
        stab[stab_idx].n_type = (sym->visibility == SCOPE_LOCAL) ? N_STSYM : N_GSYM;
        stab_idx++;
      }
    }

    assert(stab_idx == this->num_stabs - 1);
    stab[stab_idx].stroff = 1; // empty string
    stab[stab_idx].n_type = N_SO;
    stab[stab_idx].sect = 1;
  }

  // Copy symbols from input symtabs to the output sytmab
  for (i64 i = 0; i < this->syms.size(); i++) {
    Symbol<E> *sym = this->syms[i];
    if (!sym || sym->file != this || sym->output_symtab_idx == -1)
      continue;

    MachSym<E> &msym = buf[sym->output_symtab_idx];
    msym.stroff = pos[i];
    msym.is_extern = (sym->visibility == SCOPE_GLOBAL);

    if (sym->subsec && sym->subsec->is_alive) {
      msym.type = N_SECT;
      msym.sect = sym->subsec->isec->osec.sect_idx;
      msym.value = sym->get_addr(ctx);
    } else if (sym == ctx.__dyld_private || sym == ctx.__mh_dylib_header ||
               sym == ctx.__mh_bundle_header || sym == ctx.___dso_handle) {
      msym.type = N_SECT;
      msym.sect = ctx.data->sect_idx;
      msym.value = 0;
    } else if (sym == ctx.__mh_execute_header) {
      msym.type = N_SECT;
      msym.sect = ctx.text->sect_idx;
      msym.value = 0;
    } else if (sym->is_imported) {
      msym.type = N_UNDF;
      msym.sect = N_UNDF;
    } else {
      msym.type = N_ABS;
      msym.sect = N_ABS;
    }
  }
}

static bool is_system_dylib(std::string_view path) {
  if (!path.starts_with("/usr/lib/") &&
      !path.starts_with("/System/Library/Frameworks/"))
    return false;

  static std::regex re(
    R"(/usr/lib/.+\.dylib|/System/Library/Frameworks/([^/]+)\.framework/.+/\1)",
    std::regex_constants::ECMAScript | std::regex_constants::optimize);

  return std::regex_match(path.begin(), path.end(), re);
}

template <typename E>
DylibFile<E>::DylibFile(Context<E> &ctx, MappedFile<Context<E>> *mf)
    : InputFile<E>(mf) {
  this->is_dylib = true;
  this->is_weak = ctx.reader.weak;
  this->is_reexported = ctx.reader.reexport;

  if (ctx.reader.implicit) {
    // Libraries implicitly specified by LC_LINKER_OPTION are dead-stripped
    // if not used.
    this->is_alive = false;
  } else {
    // Even if -dead_strip was not given, a dylib with
    // MH_DEAD_STRIPPABLE_DYLIB is dead-stripped if unreferenced.
    bool is_dead_strippable_dylib =
      get_file_type(ctx, mf) == FileType::MACH_DYLIB &&
      (((MachHeader *)mf->data)->flags & MH_DEAD_STRIPPABLE_DYLIB);

    bool is_dead_strippable = ctx.arg.dead_strip_dylibs || is_dead_strippable_dylib;
    this->is_alive = ctx.reader.needed || !is_dead_strippable;
  }
}

template <typename E>
DylibFile<E> *DylibFile<E>::create(Context<E> &ctx, MappedFile<Context<E>> *mf) {
  DylibFile<E> *file = new DylibFile<E>(ctx, mf);
  ctx.dylib_pool.emplace_back(file);
  return file;
}

template <typename E>
static MappedFile<Context<E>> *
find_external_lib(Context<E> &ctx, DylibFile<E> &loader, std::string path) {
  auto find = [&](std::string path) -> MappedFile<Context<E>> * {
    if (!path.starts_with('/'))
      return MappedFile<Context<E>>::open(ctx, path);

    for (const std::string &root : ctx.arg.syslibroot) {
      if (path.ends_with(".tbd")) {
        if (auto *file = MappedFile<Context<E>>::open(ctx, root + path))
          return file;
        continue;
      }

      if (path.ends_with(".dylib")) {
        std::string stem(path.substr(0, path.size() - 6));
        if (auto *file = MappedFile<Context<E>>::open(ctx, root + stem + ".tbd"))
          return file;
        if (auto *file = MappedFile<Context<E>>::open(ctx, root + path))
          return file;
      }

      if (auto *file = MappedFile<Context<E>>::open(ctx, root + path + ".tbd"))
        return file;
      if (auto *file = MappedFile<Context<E>>::open(ctx, root + path + ".dylib"))
        return file;
    }

    return nullptr;
  };

  if (path.starts_with("@executable_path/") && ctx.output_type == MH_EXECUTE) {
    path = path_clean(ctx.arg.executable_path + "/../" + path.substr(17));
    return find(path);
  }

  if (path.starts_with("@loader_path/")) {
    path = path_clean(std::string(loader.mf->name) + "/../" + path.substr(13));
    return find(path);
  }

  if (path.starts_with("@rpath/")) {
    for (std::string_view rpath : loader.rpaths) {
      std::string p = path_clean(std::string(rpath) + "/" + path.substr(6));
      if (MappedFile<Context<E>> *ret = find(p))
        return ret;
    }
    return nullptr;
  }

  return find(path);
}

template <typename E>
void DylibFile<E>::parse(Context<E> &ctx) {
  switch (get_file_type(ctx, this->mf)) {
  case FileType::TAPI:
    parse_tapi(ctx);
    break;
  case FileType::MACH_DYLIB:
    parse_dylib(ctx);
    break;
  case FileType::MACH_EXE:
    parse_dylib(ctx);
    dylib_idx = BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE;
    break;
  default:
    Fatal(ctx) << *this << ": is not a dylib";
  }

  // Read reexported libraries if any
  for (std::string_view path : reexported_libs) {
    MappedFile<Context<E>> *mf =
      find_external_lib(ctx, *this, std::string(path));
    if (!mf)
      Fatal(ctx) << install_name << ": cannot open reexported library " << path;

    DylibFile<E> *child = DylibFile<E>::create(ctx, mf);
    child->parse(ctx);

    // By default, symbols defined by re-exported libraries are handled as
    // if they were defined by the umbrella library. At runtime, the dynamic
    // linker tries to find re-exported symbols from re-exported libraries.
    // That incurs some run-time cost because the runtime has to do linear
    // search.
    //
    // As an exception, system libraries get different treatment. Their
    // symbols are directly linked against their original library names
    // even if they are re-exported to reduce the cost of runtime symbol
    // lookup. This optimization can be disable by passing `-no_implicit_dylibs`.
    if (ctx.arg.implicit_dylibs && is_system_dylib(child->install_name)) {
      hoisted_libs.push_back(child);
      child->is_alive = false;
    } else {
      for (auto [name, flags] : child->exports)
        add_export(ctx, name, flags);
      append(hoisted_libs, child->hoisted_libs);
    }
  }

  // Initialize syms
  for (auto [name, flags] : exports)
    this->syms.push_back(get_symbol(ctx, name));
}

template <typename E>
void DylibFile<E>::add_export(Context<E> &ctx, std::string_view name, u32 flags) {
  auto mask = EXPORT_SYMBOL_FLAGS_KIND_MASK;
  auto tls = EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL;
  auto weak = EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION;

  u32 &existing = exports[name];
  if (existing == 0) {
    existing = flags;
    return;
  }

  if (((existing & mask) == tls) != ((flags & mask) == tls))
    Error(ctx) << *this << ": inconsistent TLS type: " << name;

  if ((existing & weak) && !(flags & weak))
    existing = flags;
}

template <typename E>
void DylibFile<E>::read_trie(Context<E> &ctx, u8 *start, i64 offset,
                             const std::string &prefix) {
  u8 *buf = start + offset;

  if (*buf) {
    read_uleb(buf); // size
    u32 flags = read_uleb(buf);
    std::string_view name;

    if (flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
      read_uleb(buf); // skip a library ordinal
      std::string_view str((char *)buf);
      buf += str.size() + 1;
      name = !str.empty() ? str : save_string(ctx, prefix);
    } else if (flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) {
      name = save_string(ctx, prefix);
      read_uleb(buf); // stub offset
      read_uleb(buf); // resolver offset
    } else {
      name = save_string(ctx, prefix);
      read_uleb(buf); // addr
    }

    add_export(ctx, name, flags);
  } else {
    buf++;
  }

  i64 nchild = *buf++;

  for (i64 i = 0; i < nchild; i++) {
    std::string suffix((char *)buf);
    buf += suffix.size() + 1;
    i64 off = read_uleb(buf);
    assert(off != offset);
    read_trie(ctx, start, off, prefix + suffix);
  }
}

template <typename E>
void DylibFile<E>::parse_tapi(Context<E> &ctx) {
  TextDylib tbd = parse_tbd(ctx, this->mf);

  install_name = tbd.install_name;
  reexported_libs = std::move(tbd.reexported_libs);

  for (std::string_view name : tbd.exports)
    add_export(ctx, name, 0);

  for (std::string_view name : tbd.weak_exports)
    add_export(ctx, name, EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION);
}

template <typename E>
void DylibFile<E>::parse_dylib(Context<E> &ctx) {
  MachHeader &hdr = *(MachHeader *)this->mf->data;
  u8 *p = this->mf->data + sizeof(hdr);

  if (ctx.arg.application_extension && !(hdr.flags & MH_APP_EXTENSION_SAFE))
    Warn(ctx) << "linking against a dylib which is not safe for use in "
              << "application extensions: " << *this;

  for (i64 i = 0; i < hdr.ncmds; i++) {
    LoadCommand &lc = *(LoadCommand *)p;

    switch (lc.cmd) {
    case LC_ID_DYLIB: {
      DylibCommand &cmd = *(DylibCommand *)p;
      install_name = (char *)p + cmd.nameoff;
      break;
    }
    case LC_DYLD_INFO_ONLY: {
      DyldInfoCommand &cmd = *(DyldInfoCommand *)p;
      if (cmd.export_off && cmd.export_size)
        read_trie(ctx, this->mf->data + cmd.export_off, 0, "");
      break;
    }
    case LC_DYLD_EXPORTS_TRIE: {
      LinkEditDataCommand &cmd = *(LinkEditDataCommand *)p;
      read_trie(ctx, this->mf->data + cmd.dataoff, 0, "");
      break;
    }
    case LC_REEXPORT_DYLIB:
      if (!(hdr.flags & MH_NO_REEXPORTED_DYLIBS)) {
        DylibCommand &cmd = *(DylibCommand *)p;
        reexported_libs.push_back((char *)p + cmd.nameoff);
      }
      break;
    case LC_RPATH: {
      RpathCommand &cmd = *(RpathCommand *)p;
      std::string rpath = (char *)p + cmd.path_off;
      if (rpath.starts_with("@loader_path/"))
        rpath = std::string(this->mf->name) + "/../" + rpath.substr(13);
      rpaths.push_back(rpath);
      break;
    }
    }
    p += lc.cmdsize;
  }
}

template <typename E>
void DylibFile<E>::resolve_symbols(Context<E> &ctx) {
  auto it = exports.begin();

  for (i64 i = 0; i < this->syms.size(); i++) {
    Symbol<E> &sym = *this->syms[i];
    u32 flags = (*it++).second;
    u32 kind = (flags & EXPORT_SYMBOL_FLAGS_KIND_MASK);

    std::scoped_lock lock(sym.mu);

    if (get_rank(this, false, false) < get_rank(sym)) {
      sym.file = this;
      sym.visibility = SCOPE_GLOBAL;
      sym.is_weak = this->is_weak || (flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION);
      sym.no_dead_strip = false;
      sym.subsec = nullptr;
      sym.value = 0;
      sym.is_common = false;
      sym.is_abs = false;
      sym.is_tlv = (kind == EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL);
    }
  }

  assert(it == exports.end());
}

template <typename E>
void DylibFile<E>::compute_symtab_size(Context<E> &ctx) {
  for (Symbol<E> *sym : this->syms) {
    if (sym && sym->file == this && (sym->stub_idx != -1 || sym->got_idx != -1)) {
      this->num_undefs++;
      this->strtab_size += sym->name.size() + 1;
      sym->output_symtab_idx = -2;
    }
  }
}

template <typename E>
void DylibFile<E>::populate_symtab(Context<E> &ctx) {
  MachSym<E> *buf = (MachSym<E> *)(ctx.buf + ctx.symtab.hdr.offset);
  u8 *strtab = ctx.buf + ctx.strtab.hdr.offset;
  i64 stroff = this->strtab_offset;

  // Copy symbols from input symtabs to the output sytmab
  for (i64 i = 0; i < this->syms.size(); i++) {
    Symbol<E> *sym = this->syms[i];
    if (!sym || sym->file != this || sym->output_symtab_idx == -1)
      continue;

    MachSym<E> &msym = buf[sym->output_symtab_idx];
    msym.stroff = stroff;
    msym.is_extern = true;
    msym.type = N_UNDF;
    msym.sect = N_UNDF;
    msym.desc = dylib_idx << 8;

    stroff += write_string(strtab + stroff, sym->name);
  }
}

using E = MOLD_TARGET;

template class InputFile<E>;
template class ObjectFile<E>;
template class DylibFile<E>;
template std::ostream &operator<<(std::ostream &, const InputFile<E> &);

} // namespace mold::macho
