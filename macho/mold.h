#pragma once

#include "macho.h"
#include "lto.h"
#include "../common/common.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tbb/concurrent_hash_map.h>
#include <tbb/spin_mutex.h>
#include <tbb/task_group.h>
#include <tbb/task_group.h>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace mold::macho {

template <typename E> class Chunk;
template <typename E> class InputSection;
template <typename E> class OutputSection;
template <typename E> class Subsection;
template <typename E> struct Context;
template <typename E> struct Symbol;

//
// input-files.cc
//

// We read Mach-O-native relocations from input files and translate
// them to mold's representation of relocations and then attach them
// to subsections.
template <typename E>
struct Relocation {
  u64 get_addr(Context<E> &ctx) const {
    return sym() ? sym()->get_addr(ctx) : subsec()->get_addr(ctx);
  }

  // A relocation refers to either a symbol or a subsection
  Symbol<E> *sym() const {
    return is_sym ? (Symbol<E> *)target : nullptr;
  }

  Subsection<E> *subsec() const {
    return is_sym ? nullptr : (Subsection<E> *)target;
  }

  bool refers_to_tls() const {
    if (sym() && sym()->subsec) {
      u32 type = sym()->subsec->isec->hdr.type;
      return type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL;
    }
    return false;
  }

  void *target = nullptr;
  i64 addend = 0;
  u32 offset = 0;
  u8 type = -1;
  u8 size = 0;
  bool is_sym : 1 = false;
  bool is_subtracted : 1 = false;

  // For range extension thunks
  i16 thunk_idx = -1;
  i16 thunk_sym_idx = -1;
};

template <typename E>
std::ostream &operator<<(std::ostream &out, const Relocation<E> &rel) {
  out << rel_to_string<E>(rel.type);
  return out;
}

// UnwindRecord represent a record describing how to handle exceptions.
// At runtime, the exception handler searches an unwinding record by
// instruction pointer.
template <typename E>
struct UnwindRecord {
  UnwindRecord(u32 len, u32 enc) : code_len(len), encoding(enc) {}

  u64 get_func_addr(Context<E> &ctx) const {
    return subsec->get_addr(ctx) + offset;
  }

  Subsection<E> *subsec = nullptr;
  Symbol<E> *personality = nullptr;
  Subsection<E> *lsda = nullptr;
  u32 offset = 0;
  u32 code_len = 0;
  u32 encoding = 0;
  u32 lsda_offset = 0;
};

template <typename E>
class InputFile {
public:
  virtual ~InputFile() = default;
  virtual void resolve_symbols(Context<E> &ctx) = 0;
  virtual void compute_symtab_size(Context<E> &ctx) = 0;
  virtual void populate_symtab(Context<E> &ctx) = 0;

  void clear_symbols();

  MappedFile<Context<E>> *mf = nullptr;
  std::string_view filename;
  std::vector<Symbol<E> *> syms;
  i64 priority = 0;
  Atomic<bool> is_alive = false;
  bool is_dylib = false;
  bool is_hidden = false;
  bool is_weak = false;
  std::string archive_name;

  // For SymtabSection
  i32 num_stabs = 0;
  i32 num_locals = 0;
  i32 num_globals = 0;
  i32 num_undefs = 0;
  i32 stabs_offset = 0;
  i32 locals_offset = 0;
  i32 globals_offset = 0;
  i32 undefs_offset = 0;
  i32 strtab_size = 0;
  i32 strtab_offset = 0;
  std::string oso_name;

protected:
  InputFile(MappedFile<Context<E>> *mf) : mf(mf), filename(mf->name) {}
  InputFile() : filename("<internal>") {}
};

template <typename E>
class ObjectFile : public InputFile<E> {
public:
  ObjectFile() = default;
  ObjectFile(MappedFile<Context<E>> *mf) : InputFile<E>(mf) {}

  static ObjectFile *create(Context<E> &ctx, MappedFile<Context<E>> *mf,
                            std::string archive_name);
  void parse(Context<E> &ctx);
  Subsection<E> *find_subsection(Context<E> &ctx, u32 addr);
  std::vector<std::string> get_linker_options(Context<E> &ctx);
  LoadCommand *find_load_command(Context<E> &ctx, u32 type);
  void parse_compact_unwind(Context<E> &ctx);
  void parse_mod_init_func(Context<E> &ctx);
  void resolve_symbols(Context<E> &ctx) override;
  void compute_symtab_size(Context<E> &ctx) override;
  void populate_symtab(Context<E> &ctx) override;
  bool is_objc_object(Context<E> &ctx);
  void mark_live_objects(Context<E> &ctx,
                         std::function<void(ObjectFile<E> *)> feeder);
  void convert_common_symbols(Context<E> &ctx);
  void check_duplicate_symbols(Context<E> &ctx);
  std::string_view get_linker_optimization_hints(Context<E> &ctx);

  Relocation<E> read_reloc(Context<E> &ctx, const MachSection<E> &hdr, MachRel r);

  std::vector<std::unique_ptr<InputSection<E>>> sections;
  std::vector<Subsection<E> *> subsections;
  std::vector<Subsection<E> *> sym_to_subsec;
  std::span<MachSym<E>> mach_syms;
  std::vector<Symbol<E>> local_syms;
  std::vector<UnwindRecord<E>> unwind_records;
  ObjcImageInfo *objc_image_info = nullptr;
  LTOModule *lto_module = nullptr;

  // For __init_offsets
  MachSection<E> *mod_init_func = nullptr;
  std::vector<Symbol<E> *> init_functions;

  // For the internal file and LTO object files
  std::vector<MachSym<E>> mach_syms2;

  // For the internal file
  void add_msgsend_symbol(Context<E> &ctx, Symbol<E> &sym);

private:
  void parse_sections(Context<E> &ctx);
  void parse_symbols(Context<E> &ctx);
  void split_subsections_via_symbols(Context<E> &ctx);
  void init_subsections(Context<E> &ctx);
  void split_cstring_literals(Context<E> &ctx);
  void split_fixed_size_literals(Context<E> &ctx);
  void split_literal_pointers(Context<E> &ctx);
  InputSection<E> *get_common_sec(Context<E> &ctx);
  void parse_lto_symbols(Context<E> &ctx);

  // For ther internal file
  Subsection<E> *add_methname_string(Context<E> &ctx, std::string_view contents);
  Subsection<E> *add_selrefs(Context<E> &ctx, Subsection<E> &methname);

  MachSection<E> *unwind_sec = nullptr;
  std::unique_ptr<MachSection<E>> common_hdr;
  InputSection<E> *common_sec = nullptr;

  std::vector<std::unique_ptr<Subsection<E>>> subsec_pool;
  std::vector<std::unique_ptr<MachSection<E>>> mach_sec_pool;
};

template <typename E>
class DylibFile : public InputFile<E> {
public:
  static DylibFile *create(Context<E> &ctx, MappedFile<Context<E>> *mf);

  void parse(Context<E> &ctx);
  void resolve_symbols(Context<E> &ctx) override;
  void compute_symtab_size(Context<E> &ctx) override;
  void populate_symtab(Context<E> &ctx) override;

  std::string_view install_name;
  i64 dylib_idx = 0;
  bool is_reexported = false;

  std::vector<std::string_view> reexported_libs;
  std::vector<std::string> rpaths;

private:
  DylibFile(Context<E> &ctx, MappedFile<Context<E>> *mf);

  void parse_tapi(Context<E> &ctx);
  void parse_dylib(Context<E> &ctx);
  void add_export(Context<E> &ctx, std::string_view name, u32 flags);
  void read_trie(Context<E> &ctx, u8 *start, i64 offset, const std::string &prefix);

  std::map<std::string_view, u32> exports;
};

template <typename E>
std::ostream &operator<<(std::ostream &out, const InputFile<E> &file);

//
// input-sections.cc
//

// InputSection represents an input section in an input file. InputSection
// is always split into one or more Subsections. If an input section is not
// splittable, we still create a subsection and let it cover the entire input
// section.
template <typename E>
class InputSection {
public:
  InputSection(Context<E> &ctx, ObjectFile<E> &file, const MachSection<E> &hdr,
               u32 secidx);
  void parse_relocations(Context<E> &ctx);

  ObjectFile<E> &file;
  const MachSection<E> &hdr;
  u32 secidx = 0;
  OutputSection<E> &osec;
  std::string_view contents;
  std::vector<Symbol<E> *> syms;
  std::vector<Relocation<E>> rels;
};

template <typename E>
std::ostream &operator<<(std::ostream &out, const InputSection<E> &sec);

// Subsection represents a region in an InputSection.
template <typename E>
class Subsection {
public:
  u64 get_addr(Context<E> &ctx) const {
    return isec->osec.hdr.addr + output_offset;
  }

  std::string_view get_contents() {
    assert(isec->hdr.type != S_ZEROFILL);
    return isec->contents.substr(input_addr - isec->hdr.addr, input_size);
  }

  std::span<UnwindRecord<E>> get_unwind_records() {
    return std::span<UnwindRecord<E>>(isec->file.unwind_records)
      .subspan(unwind_offset, nunwind);
  }

  std::span<Relocation<E>> get_rels() const {
    return std::span<Relocation<E>>(isec->rels).subspan(rel_offset, nrels);
  }

  void scan_relocations(Context<E> &ctx);
  void apply_reloc(Context<E> &ctx, u8 *buf);

  union {
    InputSection<E> *isec = nullptr;
    Subsection<E> *replacer; // used only if `is_replaced` is true
  };

  u32 input_addr = 0;
  u32 input_size = 0;
  u32 output_offset = (u32)-1;
  u32 rel_offset = 0;
  u32 nrels = 0;
  u32 unwind_offset = 0;
  u32 nunwind = 0;

  Atomic<u8> p2align = 0;
  Atomic<bool> is_alive = true;
  bool added_to_osec : 1 = false;
  bool is_replaced : 1 = false;
};

template <typename E>
std::vector<Relocation<E>>
read_relocations(Context<E> &ctx, ObjectFile<E> &file, const MachSection<E> &hdr);

//
// Symbol
//

enum {
  NEEDS_GOT              = 1 << 0,
  NEEDS_STUB             = 1 << 1,
  NEEDS_THREAD_PTR       = 1 << 2,
  NEEDS_OBJC_STUB        = 1 << 3,
  NEEDS_RANGE_EXTN_THUNK = 1 << 4,
};

enum {
  SCOPE_LOCAL,  // input file visibility
  SCOPE_MODULE, // output file visibility (non-exported symbol)
  SCOPE_GLOBAL, // global visibility (exported symbol)
};

template <typename E>
struct Symbol {
  Symbol() = default;
  Symbol(std::string_view name) : name(name) {}
  Symbol(const Symbol<E> &other) : name(other.name) {}

  u64 get_addr(Context<E> &ctx) const {
    if (stub_idx != -1)
      return ctx.stubs.hdr.addr + stub_idx * E::stub_size;
    if (subsec) {
      assert(subsec->is_alive);
      return subsec->get_addr(ctx) + value;
    }
    return value;
  }

  u64 get_got_addr(Context<E> &ctx) const {
    assert(got_idx != -1);
    return ctx.got.hdr.addr + got_idx * sizeof(Word<E>);
  }

  u64 get_tlv_addr(Context<E> &ctx) const {
    assert(tlv_idx != -1);
    return ctx.thread_ptrs.hdr.addr + tlv_idx * sizeof(Word<E>);
  }

  bool has_stub() const { return stub_idx != -1; }
  bool has_got() const { return got_idx != -1; }
  bool has_tlv() const { return tlv_idx != -1; }

  std::string_view name;
  InputFile<E> *file = nullptr;
  Subsection<E> *subsec = nullptr;
  u64 value = 0;

  i32 stub_idx = -1;
  i32 got_idx = -1;
  i32 tlv_idx = -1;

  tbb::spin_mutex mu;

  Atomic<u8> flags = 0;

  u8 visibility : 2 = SCOPE_LOCAL;
  bool is_common : 1 = false;
  bool is_weak : 1 = false;
  bool is_abs : 1 = false;
  bool is_tlv : 1 = false;
  bool no_dead_strip : 1 = false;

  // `is_exported` is true if this symbol is exported from this Mach-O
  // file. `is_imported` is true if this symbol is resolved by the dynamic
  // loader at load time.
  //
  // Note that if `-flat_namespace` is given (which is rare), both are
  // true. In that case, dynamic symbols are resolved in the same
  // semantics as ELF's.
  bool is_imported : 1 = false;
  bool is_exported : 1 = false;

  union {
    // For range extension thunks. Used by OutputSection::compute_size().
    struct {
      i16 thunk_idx = -1;
      i16 thunk_sym_idx = -1;
    };

    // For chained fixups. Used by ChainedFixupsSection.
    i32 fixup_ordinal;
  };

  // For symtab
  i32 output_symtab_idx = -1;
};

template <typename E>
std::ostream &operator<<(std::ostream &out, const Symbol<E> &sym);

// This operator defines a total order over symbols. This is used to
// make the output deterministic.
template <typename E>
inline bool operator<(const Symbol<E> &a, const Symbol<E> &b) {
  return std::tuple{a.file->priority, a.value} <
         std::tuple{b.file->priority, b.value};
}

//
// output-chunks.cc
//

template <typename E>
class OutputSegment {
public:
  static OutputSegment<E> *
  get_instance(Context<E> &ctx, std::string_view name);

  void set_offset(Context<E> &ctx, i64 fileoff, u64 vmaddr);

  SegmentCommand<E> cmd = {};
  i32 seg_idx = -1;
  std::vector<Chunk<E> *> chunks;

private:
  void set_offset_regular(Context<E> &ctx, i64 fileoff, u64 vmaddr);
  void set_offset_linkedit(Context<E> &ctx, i64 fileoff, u64 vmaddr);

  OutputSegment(std::string_view name);
};

template <typename E>
class Chunk {
public:
  Chunk(Context<E> &ctx, std::string_view segname, std::string_view sectname) {
    ctx.chunks.push_back(this);
    hdr.set_segname(segname);
    hdr.set_sectname(sectname);
    seg = OutputSegment<E>::get_instance(ctx, segname);
  }

  virtual ~Chunk() = default;
  virtual void compute_size(Context<E> &ctx) {}
  virtual void copy_buf(Context<E> &ctx) {}

  OutputSection<E> *to_osec() const  {
    if (is_output_section)
      return (OutputSection<E> *)this;
    return nullptr;
  }

  MachSection<E> hdr = {};
  u32 sect_idx = 0;
  bool is_hidden = false;
  OutputSegment<E> *seg = nullptr;

protected:
  bool is_output_section = false;
};

template <typename E>
std::ostream &operator<<(std::ostream &out, const Chunk<E> &chunk);

template <typename E>
class OutputMachHeader : public Chunk<E> {
public:
  OutputMachHeader(Context<E> &ctx)
    : Chunk<E>(ctx, "__TEXT", "__mach_header") {
    this->is_hidden = true;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;
};

template <typename E>
class RangeExtensionThunk {};

template <typename E> requires is_arm<E>
class RangeExtensionThunk<E> {
public:
  RangeExtensionThunk(OutputSection<E> &osec, i64 thunk_idx, i64 offset)
    : output_section(osec), thunk_idx(thunk_idx), offset(offset) {}

  i64 size() const {
    return symbols.size() * ENTRY_SIZE;
  }

  u64 get_addr(i64 idx) const {
    return output_section.hdr.addr + offset + idx * ENTRY_SIZE;
  }

  void copy_buf(Context<E> &ctx);

  static constexpr i64 ALIGNMENT = 16;
  static constexpr i64 ENTRY_SIZE = 12;

  OutputSection<E> &output_section;
  i64 thunk_idx;
  i64 offset;
  std::mutex mu;
  std::vector<Symbol<E> *> symbols;
};

template <typename E>
class OutputSection : public Chunk<E> {
public:
  static OutputSection<E> *
  get_instance(Context<E> &ctx, std::string_view segname,
               std::string_view sectname);

  OutputSection(Context<E> &ctx, std::string_view segname,
                std::string_view sectname)
    : Chunk<E>(ctx, segname, sectname) {
    this->is_output_section = true;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

  void add_subsec(Subsection<E> *subsec) {
    members.push_back(subsec);
    this->hdr.p2align = std::max<u32>(this->hdr.p2align, subsec->p2align);
    this->hdr.attr |= subsec->isec->hdr.attr;
    this->hdr.type = subsec->isec->hdr.type;

    assert(!subsec->added_to_osec);
    subsec->added_to_osec = true;
  }

  std::vector<Subsection<E> *> members;
  std::vector<std::unique_ptr<RangeExtensionThunk<E>>> thunks;
};

template <typename E>
class RebaseSection : public Chunk<E> {
public:
  RebaseSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__rebase") {
    this->is_hidden = true;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

  std::vector<u8> contents;
};

template <typename E>
class BindSection : public Chunk<E> {
public:
  BindSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__binding") {
    this->is_hidden = true;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

  std::vector<u8> contents;
};

template <typename E>
class LazyBindSection : public Chunk<E> {
public:
  LazyBindSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__lazy_binding") {
    this->is_hidden = true;
    this->hdr.p2align = 3;
  }

  void add(Context<E> &ctx, Symbol<E> &sym, i64 idx);

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

  std::vector<u32> bind_offsets;
  std::vector<u8> contents;
};

class ExportEncoder {
public:
  i64 finish();

  struct Entry {
    std::string_view name;
    u32 flags;
    u64 addr;
  };

  struct TrieNode {
    std::string_view prefix;
    std::vector<std::unique_ptr<TrieNode>> children;
    u64 addr = 0;
    u32 flags = 0;
    u32 offset = -1;
    bool is_leaf = false;
  };

  void construct_trie(TrieNode &node, std::span<Entry> entries, i64 len,
                      tbb::task_group *tg, i64 grain_size, bool divide);

  static i64 set_offset(TrieNode &node, i64 offset);
  void write_trie(u8 *buf, TrieNode &node);

  TrieNode root;
  std::vector<Entry> entries;
};

template <typename E>
class ExportSection : public Chunk<E> {
public:
  ExportSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__export") {
    this->is_hidden = true;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

private:
  ExportEncoder enc;
};

template <typename E>
class FunctionStartsSection : public Chunk<E> {
public:
  FunctionStartsSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__func_starts") {
    this->is_hidden = true;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

  std::vector<u8> contents;
};

template <typename E>
class SymtabSection : public Chunk<E> {
public:
  SymtabSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__symbol_table") {
    this->is_hidden = true;
    this->hdr.p2align = 3;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

  i64 globals_offset = 0;
  i64 undefs_offset = 0;

  static constexpr std::string_view strtab_init_image = " \0-\0"sv;
};

template <typename E>
class StrtabSection : public Chunk<E> {
public:
  StrtabSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__string_table") {
    this->is_hidden = true;
    this->hdr.p2align = 3;
    this->hdr.size = 1;
  }
};

template <typename E>
class IndirectSymtabSection : public Chunk<E> {
public:
  IndirectSymtabSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__ind_sym_tab") {
    this->is_hidden = true;
  }

  static constexpr i64 ENTRY_SIZE = 4;

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;
};

template <typename E>
class ObjcImageInfoSection : public Chunk<E> {
public:
  static std::unique_ptr<ObjcImageInfoSection> create(Context<E> &ctx);

  ObjcImageInfoSection(Context<E> &ctx, ObjcImageInfo contents)
    : Chunk<E>(ctx, "__DATA", "__objc_imageinfo"),
      contents(contents) {
    this->hdr.p2align = 2;
    this->hdr.size = sizeof(contents);
  }

  void copy_buf(Context<E> &ctx) override;

private:
  ObjcImageInfo contents;
};

template <typename E>
class ObjcStubsSection : public Chunk<E> {
public:
  ObjcStubsSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__TEXT", "__objc_stubs") {
    this->hdr.p2align = 5;
    this->hdr.attr = S_ATTR_SOME_INSTRUCTIONS | S_ATTR_PURE_INSTRUCTIONS;
  }

  void copy_buf(Context<E> &ctx) override;

  std::vector<Subsection<E> *> methnames;
  std::vector<Subsection<E> *> selrefs;

  static constexpr i64 ENTRY_SIZE = is_arm<E> ? 32 : 16;
};

template <typename E>
class InitOffsetsSection : public Chunk<E> {
public:
  InitOffsetsSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__TEXT", "__init_offsets") {
    this->hdr.p2align = 2;
    this->hdr.type = S_INIT_FUNC_OFFSETS;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;
};

template <typename E>
class CodeSignatureSection : public Chunk<E> {
public:
  CodeSignatureSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__code_signature") {
    this->is_hidden = true;
    this->hdr.p2align = 4;
  }

  void compute_size(Context<E> &ctx) override;
  void write_signature(Context<E> &ctx);
};

template <typename E>
class DataInCodeSection : public Chunk<E> {
public:
  DataInCodeSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__data_in_code") {
    this->is_hidden = true;
    this->hdr.p2align = 3;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

  std::vector<DataInCodeEntry> contents;
};

template <typename E>
struct Fixup {
  u64 addr;
  Symbol<E> *sym = nullptr;
  u64 addend = 0;
};

template <typename E>
struct SymbolAddend {
  bool operator==(const SymbolAddend &) const = default;

  Symbol<E> *sym = nullptr;
  u64 addend = 0;
};

template <typename E>
class ChainedFixupsSection : public Chunk<E> {
public:
  ChainedFixupsSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__LINKEDIT", "__chainfixups") {
    this->is_hidden = true;
    this->hdr.p2align = 3;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;
  void write_fixup_chains(Context<E> &ctx);

private:
  u8 *allocate(i64 size);
  template <typename T> void write_imports(Context<E> &ctx);

  std::vector<Fixup<E>> fixups;
  std::vector<SymbolAddend<E>> dynsyms;
  std::vector<u8> contents;
};

template <typename E>
class StubsSection : public Chunk<E> {
public:
  StubsSection(Context<E> &ctx) : Chunk<E>(ctx, "__TEXT", "__stubs") {
    this->hdr.p2align = 4;
    this->hdr.type = S_SYMBOL_STUBS;
    this->hdr.attr = S_ATTR_SOME_INSTRUCTIONS | S_ATTR_PURE_INSTRUCTIONS;
    this->hdr.reserved1 = 0;
    this->hdr.reserved2 = E::stub_size;
  }

  void add(Context<E> &ctx, Symbol<E> *sym);
  void copy_buf(Context<E> &ctx) override;

  std::vector<Symbol<E> *> syms;
};

template <typename E>
class StubHelperSection : public Chunk<E> {
public:
  StubHelperSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__TEXT", "__stub_helper") {
    this->hdr.p2align = 4;
    this->hdr.attr = S_ATTR_SOME_INSTRUCTIONS | S_ATTR_PURE_INSTRUCTIONS;
  }

  void copy_buf(Context<E> &ctx) override;
};

template <typename E>
class UnwindInfoSection : public Chunk<E> {
public:
  UnwindInfoSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__TEXT", "__unwind_info") {
    this->hdr.p2align = 2;
  }

  void compute_size(Context<E> &ctx) override;
  void copy_buf(Context<E> &ctx) override;

  std::vector<u8> contents;
};

template <typename E>
class GotSection : public Chunk<E> {
public:
  GotSection(Context<E> &ctx) : Chunk<E>(ctx, "__DATA_CONST", "__got") {
    this->hdr.p2align = 3;
    this->hdr.type = S_NON_LAZY_SYMBOL_POINTERS;
  }

  void add(Context<E> &ctx, Symbol<E> *sym);
  void copy_buf(Context<E> &ctx) override;

  std::vector<Symbol<E> *> syms;
  std::vector<Subsection<E> *> subsections;
};

template <typename E>
class LazySymbolPtrSection : public Chunk<E> {
public:
  LazySymbolPtrSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__DATA", "__la_symbol_ptr") {
    this->hdr.p2align = 3;
    this->hdr.type = S_LAZY_SYMBOL_POINTERS;
  }

  void copy_buf(Context<E> &ctx) override;
};

template <typename E>
class ThreadPtrsSection : public Chunk<E> {
public:
  ThreadPtrsSection(Context<E> &ctx)
    : Chunk<E>(ctx, "__DATA", "__thread_ptrs") {
    this->hdr.p2align = 3;
    this->hdr.type = S_THREAD_LOCAL_VARIABLE_POINTERS;
  }

  void add(Context<E> &ctx, Symbol<E> *sym);
  void copy_buf(Context<E> &ctx) override;

  std::vector<Symbol<E> *> syms;
};

template <typename E>
class SectCreateSection : public Chunk<E> {
public:
  SectCreateSection(Context<E> &ctx, std::string_view seg, std::string_view sect,
                    std::string_view contents);

  void copy_buf(Context<E> &ctx) override;

  std::string_view contents;
};

//
// mapfile.cc
//

template <typename E>
void print_map(Context<E> &ctx);

//
// yaml.cc
//

struct YamlNode {
  std::variant<std::string_view,
               std::vector<YamlNode>,
               std::map<std::string_view, YamlNode>> data;
};

struct YamlError {
  std::string msg;
  i64 pos;
};

std::variant<std::vector<YamlNode>, YamlError>
parse_yaml(std::string_view str);

//
// tapi.cc
//

struct TextDylib {
  std::string_view install_name;
  std::vector<std::string_view> reexported_libs;
  std::set<std::string_view> exports;
  std::set<std::string_view> weak_exports;
};

template <typename E>
TextDylib parse_tbd(Context<E> &ctx, MappedFile<Context<E>> *mf);

//
// cmdline.cc
//

struct VersionTriple {
  VersionTriple &operator=(const VersionTriple &) = default;
  auto operator<=>(const VersionTriple &) const = default;

  i64 encode() const { return (major << 16) | (minor << 8) | patch; }

  i64 major = 0;
  i64 minor = 0;
  i64 patch = 0;
};

template <typename E>
VersionTriple parse_version(Context<E> &ctx, std::string_view arg);

template <typename E>
std::vector<std::string> parse_nonpositional_args(Context<E> &ctx);

//
// dead-strip.cc
//

template <typename E>
void dead_strip(Context<E> &ctx);

//
// lto.cc
//

template <typename E>
void load_lto_plugin(Context<E> &ctx);

template <typename E>
void do_lto(Context<E> &ctx);

//
// arch-arm64.cc
//

template <typename E> requires is_arm<E>
void create_range_extension_thunks(Context<E> &ctx, OutputSection<E> &osec);

//
// main.cc
//

enum UuidKind { UUID_NONE, UUID_HASH, UUID_RANDOM };

struct AddEmptySectionOption {
  std::string_view segname;
  std::string_view sectname;
};

struct SectAlignOption {
  std::string_view segname;
  std::string_view sectname;
  u8 p2align;
};

struct SectCreateOption {
  std::string_view segname;
  std::string_view sectname;
  std::string_view filename;
};

template <typename E>
struct Context {
  Context() {
    text_seg = OutputSegment<E>::get_instance(*this, "__TEXT");
    data_const_seg = OutputSegment<E>::get_instance(*this, "__DATA_CONST");
    data_seg = OutputSegment<E>::get_instance(*this, "__DATA");
    linkedit_seg = OutputSegment<E>::get_instance(*this, "__LINKEDIT");

    text = OutputSection<E>::get_instance(*this, "__TEXT", "__text");
    data = OutputSection<E>::get_instance(*this, "__DATA", "__data");
    bss = OutputSection<E>::get_instance(*this, "__DATA", "__bss");
    common = OutputSection<E>::get_instance(*this, "__DATA", "__common");

    bss->hdr.type = S_ZEROFILL;
    common->hdr.type = S_ZEROFILL;

    dyld_stub_binder = get_symbol(*this, "dyld_stub_binder");
    _objc_msgSend = get_symbol(*this, "_objc_msgSend");
    __mh_execute_header = get_symbol(*this, "__mh_execute_header");
    __dyld_private = get_symbol(*this, "__dyld_private");
    __mh_dylib_header = get_symbol(*this, "__mh_dylib_header");
    __mh_bundle_header = get_symbol(*this, "__mh_bundle_header");
    ___dso_handle = get_symbol(*this, "___dso_handle");
  }

  Context(const Context<E> &) = delete;

  void checkpoint() {
    if (has_error) {
      cleanup();
      _exit(1);
    }
  }

  // Command-line arguments
  struct {
    MultiGlob exported_symbols_list;
    MultiGlob unexported_symbols_list;
    Symbol<E> *entry = nullptr;
    UuidKind uuid = UUID_HASH;
    VersionTriple compatibility_version;
    VersionTriple current_version;
    VersionTriple platform_min_version;
    VersionTriple platform_sdk_version;
    bool ObjC = false;
    bool S = false;
    bool adhoc_codesign = is_arm<E>;
    bool application_extension = false;
    bool bind_at_load = false;
    bool color_diagnostics = false;
    bool data_in_code_info = true;
    bool dead_strip = false;
    bool dead_strip_dylibs = false;
    bool deduplicate = true;
    bool demangle = true;
    bool dynamic = true;
    bool export_dynamic = false;
    bool fatal_warnings = false;
    bool fixup_chains = false;
    bool flat_namespace = false;
    bool function_starts = true;
    bool init_offsets = false;
    bool mark_dead_strippable_dylib = false;
    bool noinhibit_exec = false;
    bool perf = false;
    bool quick_exit = true;
    bool search_paths_first = true;
    bool stats = false;
    bool trace = false;
    bool x = false;
    i64 arch = CPU_TYPE_ARM64;
    i64 filler = 0;
    i64 headerpad = 256;
    i64 pagezero_size = 0;
    i64 platform = PLATFORM_MACOS;
    i64 stack_size = 0;
    i64 thread_count = 0;
    std::string bundle_loader;
    std::string chroot;
    std::string dependency_info;
    std::string executable_path;
    std::string final_output;
    std::string install_name;
    std::string lto_library;
    std::string map;
    std::string object_path_lto;
    std::string oso_prefix;
    std::string output = "a.out";
    std::string plugin;
    std::string umbrella;
    std::vector<AddEmptySectionOption> add_empty_section;
    std::vector<SectAlignOption> sectalign;
    std::vector<SectCreateOption> sectcreate;
    std::vector<std::string> U;
    std::vector<std::string> add_ast_path;
    std::vector<std::string> framework_paths;
    std::vector<std::string> library_paths;
    std::vector<std::string> mllvm;
    std::vector<std::string> order_file;
    std::vector<std::string> rpaths;
    std::vector<std::string> syslibroot;
    std::vector<std::string> u;
  } arg;

  std::vector<std::string_view> cmdline_args;
  tbb::task_group tg;
  u32 output_type = MH_EXECUTE;
  i64 file_priority = 10000;
  bool all_load = false;
  bool needed_l = false;
  bool hidden_l = false;
  bool weak_l = false;
  bool reexport_l = false;
  std::set<std::string> missing_files; // for -dependency_info

  u8 uuid[16] = {};
  bool has_error = false;
  u64 tls_begin = 0;
  std::string cwd = std::filesystem::current_path().string();

  LTOPlugin lto = {};
  std::once_flag lto_plugin_loaded;

  tbb::concurrent_hash_map<std::string_view, Symbol<E>, HashCmp> symbol_map;

  std::unique_ptr<OutputFile<Context<E>>> output_file;
  u8 *buf;
  bool overwrite_output_file = false;

  tbb::concurrent_vector<std::unique_ptr<ObjectFile<E>>> obj_pool;
  tbb::concurrent_vector<std::unique_ptr<DylibFile<E>>> dylib_pool;
  tbb::concurrent_vector<std::unique_ptr<u8[]>> string_pool;
  tbb::concurrent_vector<std::unique_ptr<MappedFile<Context<E>>>> mf_pool;
  std::vector<std::unique_ptr<Chunk<E>>> chunk_pool;

  tbb::concurrent_vector<std::unique_ptr<TimerRecord>> timer_records;

  std::vector<ObjectFile<E> *> objs;
  std::vector<DylibFile<E> *> dylibs;
  ObjectFile<E> *internal_obj = nullptr;

  OutputSegment<E> *text_seg = nullptr;
  OutputSegment<E> *data_const_seg = nullptr;
  OutputSegment<E> *data_seg = nullptr;
  OutputSegment<E> *linkedit_seg = nullptr;

  std::vector<std::unique_ptr<OutputSegment<E>>> segments;
  std::vector<Chunk<E> *> chunks;

  OutputMachHeader<E> mach_hdr{*this};
  StubsSection<E> stubs{*this};
  UnwindInfoSection<E> unwind_info{*this};
  GotSection<E> got{*this};
  ThreadPtrsSection<E> thread_ptrs{*this};
  ExportSection<E> export_{*this};
  SymtabSection<E> symtab{*this};
  StrtabSection<E> strtab{*this};
  IndirectSymtabSection<E> indir_symtab{*this};

  std::unique_ptr<StubHelperSection<E>> stub_helper;
  std::unique_ptr<LazySymbolPtrSection<E>> lazy_symbol_ptr;
  std::unique_ptr<LazyBindSection<E>> lazy_bind;
  std::unique_ptr<RebaseSection<E>> rebase;
  std::unique_ptr<BindSection<E>> bind;

  std::unique_ptr<ChainedFixupsSection<E>> chained_fixups;
  std::unique_ptr<FunctionStartsSection<E>> function_starts;
  std::unique_ptr<ObjcImageInfoSection<E>> image_info;
  std::unique_ptr<CodeSignatureSection<E>> code_sig;
  std::unique_ptr<ObjcStubsSection<E>> objc_stubs;
  std::unique_ptr<DataInCodeSection<E>> data_in_code;
  std::unique_ptr<InitOffsetsSection<E>> init_offsets;

  OutputSection<E> *text = nullptr;
  OutputSection<E> *data = nullptr;
  OutputSection<E> *bss = nullptr;
  OutputSection<E> *common = nullptr;

  Symbol<E> *dyld_stub_binder = nullptr;
  Symbol<E> *_objc_msgSend = nullptr;
  Symbol<E> *__mh_execute_header = nullptr;
  Symbol<E> *__dyld_private = nullptr;
  Symbol<E> *__mh_dylib_header = nullptr;
  Symbol<E> *__mh_bundle_header = nullptr;
  Symbol<E> *___dso_handle = nullptr;
};

template <typename E>
int macho_main(int argc, char **argv);

int main(int argc, char **argv);

//
// Inline functions
//

template <typename E>
std::ostream &operator<<(std::ostream &out, const InputSection<E> &sec) {
  out << sec.file << "(" << sec.hdr.get_segname() << ","
      << sec.hdr.get_sectname() << ")";
  return out;
}

template <typename E>
inline std::ostream &operator<<(std::ostream &out, const Symbol<E> &sym) {
  if (opt_demangle && sym.name.starts_with("__Z"))
    out << demangle(sym.name.substr(1));
  else
    out << sym.name;
  return out;
}

template <typename E>
inline Symbol<E> *get_symbol(Context<E> &ctx, std::string_view name) {
  typename decltype(ctx.symbol_map)::const_accessor acc;
  ctx.symbol_map.insert(acc, {name, Symbol<E>(name)});
  return (Symbol<E> *)(&acc->second);
}

} // namespace mold::macho
