#pragma once

#include "../common/integers.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace mold::macho {

struct ARM64;
struct ARM64_32;
struct X86_64;

template <typename E> static constexpr bool is_arm64 = std::is_same_v<E, ARM64>;
template <typename E> static constexpr bool is_arm64_32 = std::is_same_v<E, ARM64_32>;
template <typename E> static constexpr bool is_arm = is_arm64<E> || is_arm64_32<E>;
template <typename E> static constexpr bool is_x86 = std::is_same_v<E, X86_64>;

template <typename E> using Word = std::conditional_t<is_arm64_32<E>, ul32, ul64>;
template <typename E> using IWord = std::conditional_t<is_arm64_32<E>, il32, il64>;

template <typename E>
std::string rel_to_string(u8 r_type);

enum : u32 {
  FAT_MAGIC = 0xcafebabe,
};

enum : u32 {
  MH_OBJECT = 0x1,
  MH_EXECUTE = 0x2,
  MH_FVMLIB = 0x3,
  MH_CORE = 0x4,
  MH_PRELOAD = 0x5,
  MH_DYLIB = 0x6,
  MH_DYLINKER = 0x7,
  MH_BUNDLE = 0x8,
  MH_DYLIB_STUB = 0x9,
  MH_DSYM = 0xa,
  MH_KEXT_BUNDLE = 0xb,
};

enum : u32 {
  MH_NOUNDEFS = 0x1,
  MH_INCRLINK = 0x2,
  MH_DYLDLINK = 0x4,
  MH_BINDATLOAD = 0x8,
  MH_PREBOUND = 0x10,
  MH_SPLIT_SEGS = 0x20,
  MH_LAZY_INIT = 0x40,
  MH_TWOLEVEL = 0x80,
  MH_FORCE_FLAT = 0x100,
  MH_NOMULTIDEFS = 0x200,
  MH_NOFIXPREBINDING = 0x400,
  MH_PREBINDABLE = 0x800,
  MH_ALLMODSBOUND = 0x1000,
  MH_SUBSECTIONS_VIA_SYMBOLS = 0x2000,
  MH_CANONICAL = 0x4000,
  MH_WEAK_DEFINES = 0x8000,
  MH_BINDS_TO_WEAK = 0x10000,
  MH_ALLOW_STACK_EXECUTION = 0x20000,
  MH_ROOT_SAFE = 0x40000,
  MH_SETUID_SAFE = 0x80000,
  MH_NO_REEXPORTED_DYLIBS = 0x100000,
  MH_PIE = 0x200000,
  MH_DEAD_STRIPPABLE_DYLIB = 0x400000,
  MH_HAS_TLV_DESCRIPTORS = 0x800000,
  MH_NO_HEAP_EXECUTION = 0x1000000,
  MH_APP_EXTENSION_SAFE = 0x02000000,
  MH_NLIST_OUTOFSYNC_WITH_DYLDINFO = 0x04000000,
  MH_SIM_SUPPORT = 0x08000000,
};

enum : u32 {
  VM_PROT_READ = 0x1,
  VM_PROT_WRITE = 0x2,
  VM_PROT_EXECUTE = 0x4,
  VM_PROT_NO_CHANGE = 0x8,
  VM_PROT_COPY = 0x10,
  VM_PROT_WANTS_COPY = 0x10,
};

enum : u32 {
  LC_REQ_DYLD = 0x80000000,
};

enum : u32 {
  LC_SEGMENT = 0x1,
  LC_SYMTAB = 0x2,
  LC_SYMSEG = 0x3,
  LC_THREAD = 0x4,
  LC_UNIXTHREAD = 0x5,
  LC_LOADFVMLIB = 0x6,
  LC_IDFVMLIB = 0x7,
  LC_IDENT = 0x8,
  LC_FVMFILE = 0x9,
  LC_PREPAGE = 0xa,
  LC_DYSYMTAB = 0xb,
  LC_LOAD_DYLIB = 0xc,
  LC_ID_DYLIB = 0xd,
  LC_LOAD_DYLINKER = 0xe,
  LC_ID_DYLINKER = 0xf,
  LC_PREBOUND_DYLIB = 0x10,
  LC_ROUTINES = 0x11,
  LC_SUB_FRAMEWORK = 0x12,
  LC_SUB_UMBRELLA = 0x13,
  LC_SUB_CLIENT = 0x14,
  LC_SUB_LIBRARY = 0x15,
  LC_TWOLEVEL_HINTS = 0x16,
  LC_PREBIND_CKSUM = 0x17,
  LC_LOAD_WEAK_DYLIB = (0x18 | LC_REQ_DYLD),
  LC_SEGMENT_64 = 0x19,
  LC_ROUTINES_64 = 0x1a,
  LC_UUID = 0x1b,
  LC_RPATH = (0x1c | LC_REQ_DYLD),
  LC_CODE_SIGNATURE = 0x1d,
  LC_SEGMENT_SPLIT_INFO = 0x1e,
  LC_REEXPORT_DYLIB = (0x1f | LC_REQ_DYLD),
  LC_LAZY_LOAD_DYLIB = 0x20,
  LC_ENCRYPTION_INFO = 0x21,
  LC_DYLD_INFO = 0x22,
  LC_DYLD_INFO_ONLY = (0x22 | LC_REQ_DYLD),
  LC_LOAD_UPWARD_DYLIB = (0x23 | LC_REQ_DYLD),
  LC_VERSION_MIN_MACOSX = 0x24,
  LC_VERSION_MIN_IPHONEOS = 0x25,
  LC_FUNCTION_STARTS = 0x26,
  LC_DYLD_ENVIRONMENT = 0x27,
  LC_MAIN = (0x28 | LC_REQ_DYLD),
  LC_DATA_IN_CODE = 0x29,
  LC_SOURCE_VERSION = 0x2A,
  LC_DYLIB_CODE_SIGN_DRS = 0x2B,
  LC_ENCRYPTION_INFO_64 = 0x2C,
  LC_LINKER_OPTION = 0x2D,
  LC_LINKER_OPTIMIZATION_HINT = 0x2E,
  LC_VERSION_MIN_TVOS = 0x2F,
  LC_VERSION_MIN_WATCHOS = 0x30,
  LC_NOTE = 0x31,
  LC_BUILD_VERSION = 0x32,
  LC_DYLD_EXPORTS_TRIE = (0x33 | LC_REQ_DYLD),
  LC_DYLD_CHAINED_FIXUPS = (0x34 | LC_REQ_DYLD),
};

enum : u32 {
  SG_HIGHVM = 0x1,
  SG_FVMLIB = 0x2,
  SG_NORELOC = 0x4,
  SG_PROTECTED_VERSION_1 = 0x8,
  SG_READ_ONLY = 0x10,
};

enum : u32 {
  S_REGULAR = 0x0,
  S_ZEROFILL = 0x1,
  S_CSTRING_LITERALS = 0x2,
  S_4BYTE_LITERALS = 0x3,
  S_8BYTE_LITERALS = 0x4,
  S_LITERAL_POINTERS = 0x5,
  S_NON_LAZY_SYMBOL_POINTERS = 0x6,
  S_LAZY_SYMBOL_POINTERS = 0x7,
  S_SYMBOL_STUBS = 0x8,
  S_MOD_INIT_FUNC_POINTERS = 0x9,
  S_MOD_TERM_FUNC_POINTERS = 0xa,
  S_COALESCED = 0xb,
  S_GB_ZEROFILL = 0xc,
  S_INTERPOSING = 0xd,
  S_16BYTE_LITERALS = 0xe,
  S_DTRACE_DOF = 0xf,
  S_LAZY_DYLIB_SYMBOL_POINTERS = 0x10,
  S_THREAD_LOCAL_REGULAR = 0x11,
  S_THREAD_LOCAL_ZEROFILL = 0x12,
  S_THREAD_LOCAL_VARIABLES = 0x13,
  S_THREAD_LOCAL_VARIABLE_POINTERS = 0x14,
  S_THREAD_LOCAL_INIT_FUNCTION_POINTERS = 0x15,
  S_INIT_FUNC_OFFSETS = 0x16,
};

enum : u32 {
  S_ATTR_LOC_RELOC = 0x1,
  S_ATTR_EXT_RELOC = 0x2,
  S_ATTR_SOME_INSTRUCTIONS = 0x4,
  S_ATTR_DEBUG = 0x20000,
  S_ATTR_SELF_MODIFYING_CODE = 0x40000,
  S_ATTR_LIVE_SUPPORT = 0x80000,
  S_ATTR_NO_DEAD_STRIP = 0x100000,
  S_ATTR_STRIP_STATIC_SYMS = 0x200000,
  S_ATTR_NO_TOC = 0x400000,
  S_ATTR_PURE_INSTRUCTIONS = 0x800000,
};

enum : u32 {
  CPU_TYPE_X86_64 = 0x1000007,
  CPU_TYPE_ARM64 = 0x100000c,
  CPU_TYPE_ARM64_32 = 0x200000c,
};

enum : u32 {
  CPU_SUBTYPE_X86_64_ALL = 3,
  CPU_SUBTYPE_ARM64_ALL = 0,
};

enum : u32 {
  INDIRECT_SYMBOL_LOCAL = 0x80000000,
  INDIRECT_SYMBOL_ABS = 0x40000000,
};

enum : u32 {
  REBASE_TYPE_POINTER = 1,
  REBASE_TYPE_TEXT_ABSOLUTE32 = 2,
  REBASE_TYPE_TEXT_PCREL32 = 3,
  REBASE_OPCODE_MASK = 0xf0,
  REBASE_IMMEDIATE_MASK = 0x0f,
  REBASE_OPCODE_DONE = 0x00,
  REBASE_OPCODE_SET_TYPE_IMM = 0x10,
  REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20,
  REBASE_OPCODE_ADD_ADDR_ULEB = 0x30,
  REBASE_OPCODE_ADD_ADDR_IMM_SCALED = 0x40,
  REBASE_OPCODE_DO_REBASE_IMM_TIMES = 0x50,
  REBASE_OPCODE_DO_REBASE_ULEB_TIMES = 0x60,
  REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB = 0x70,
  REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB = 0x80,
};

enum : u32 {
  BIND_SPECIAL_DYLIB_SELF = 0,
  BIND_TYPE_POINTER = 1,
  BIND_TYPE_TEXT_ABSOLUTE32 = 2,
  BIND_TYPE_TEXT_PCREL32 = 3,
  BIND_SPECIAL_DYLIB_WEAK_LOOKUP = (u32)-3,
  BIND_SPECIAL_DYLIB_FLAT_LOOKUP = (u32)-2,
  BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE = (u32)-1,
  BIND_SYMBOL_FLAGS_WEAK_IMPORT = 1,
  BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION = 8,
  BIND_OPCODE_MASK = 0xF0,
  BIND_IMMEDIATE_MASK = 0x0F,
  BIND_OPCODE_DONE = 0x00,
  BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10,
  BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB = 0x20,
  BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30,
  BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40,
  BIND_OPCODE_SET_TYPE_IMM = 0x50,
  BIND_OPCODE_SET_ADDEND_SLEB = 0x60,
  BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70,
  BIND_OPCODE_ADD_ADDR_ULEB = 0x80,
  BIND_OPCODE_DO_BIND = 0x90,
  BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB = 0xA0,
  BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED = 0xB0,
  BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB = 0xC0,
  BIND_OPCODE_THREADED = 0xD0,
  BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB = 0x00,
  BIND_SUBOPCODE_THREADED_APPLY = 0x01,
};

enum : u32 {
  EXPORT_SYMBOL_FLAGS_KIND_MASK = 0x03,
  EXPORT_SYMBOL_FLAGS_KIND_REGULAR = 0x00,
  EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL = 0x01,
  EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE = 0x02,
  EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION = 0x04,
  EXPORT_SYMBOL_FLAGS_REEXPORT = 0x08,
  EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER = 0x10,
};

enum : u32 {
  DICE_KIND_DATA = 1,
  DICE_KIND_JUMP_TABLE8 = 2,
  DICE_KIND_JUMP_TABLE16 = 3,
  DICE_KIND_JUMP_TABLE32 = 4,
  DICE_KIND_ABS_JUMP_TABLE32 = 5,
};

enum : u32 {
  N_UNDF = 0,
  N_ABS = 1,
  N_INDR = 5,
  N_PBUD = 6,
  N_SECT = 7,
};

enum : u32 {
  N_GSYM = 0x20,
  N_FNAME = 0x22,
  N_FUN = 0x24,
  N_STSYM = 0x26,
  N_LCSYM = 0x28,
  N_BNSYM = 0x2e,
  N_AST = 0x32,
  N_OPT = 0x3c,
  N_RSYM = 0x40,
  N_SLINE = 0x44,
  N_ENSYM = 0x4e,
  N_SSYM = 0x60,
  N_SO = 0x64,
  N_OSO = 0x66,
  N_LSYM = 0x80,
  N_BINCL = 0x82,
  N_SOL = 0x84,
  N_PARAMS = 0x86,
  N_VERSION = 0x88,
  N_OLEVEL = 0x8A,
  N_PSYM = 0xa0,
  N_EINCL = 0xa2,
  N_ENTRY = 0xa4,
  N_LBRAC = 0xc0,
  N_EXCL = 0xc2,
  N_RBRAC = 0xe0,
  N_BCOMM = 0xe2,
  N_ECOMM = 0xe4,
  N_ECOML = 0xe8,
  N_LENG = 0xfe,
  N_PC = 0x30,
};

enum : u32 {
  REFERENCE_TYPE = 0xf,
  REFERENCE_FLAG_UNDEFINED_NON_LAZY = 0,
  REFERENCE_FLAG_UNDEFINED_LAZY = 1,
  REFERENCE_FLAG_DEFINED = 2,
  REFERENCE_FLAG_PRIVATE_DEFINED = 3,
  REFERENCE_FLAG_PRIVATE_UNDEFINED_NON_LAZY = 4,
  REFERENCE_FLAG_PRIVATE_UNDEFINED_LAZY = 5,
};

enum : u32 {
  REFERENCED_DYNAMICALLY = 0x0010,
};

enum : u32 {
  SELF_LIBRARY_ORDINAL = 0x0,
  MAX_LIBRARY_ORDINAL = 0xfd,
  DYNAMIC_LOOKUP_ORDINAL = 0xfe,
  EXECUTABLE_ORDINAL = 0xff,
};

enum : u32 {
  N_NO_DEAD_STRIP = 0x0020,
  N_DESC_DISCARDED = 0x0020,
  N_WEAK_REF = 0x0040,
  N_WEAK_DEF = 0x0080,
  N_REF_TO_WEAK = 0x0080,
  N_ARM_THUMB_DEF = 0x0008,
  N_SYMBOL_RESOLVER = 0x0100,
  N_ALT_ENTRY = 0x0200,
};

enum : u32 {
  PLATFORM_MACOS = 1,
  PLATFORM_IOS = 2,
  PLATFORM_TVOS = 3,
  PLATFORM_WATCHOS = 4,
  PLATFORM_BRIDGEOS = 5,
  PLATFORM_MACCATALYST = 6,
  PLATFORM_IOSSIMULATOR = 7,
  PLATFORM_TVOSSIMULATOR = 8,
  PLATFORM_WATCHOSSIMULATOR = 9,
  PLATFORM_DRIVERKIT = 10,
};

enum : u32 {
  TOOL_CLANG = 1,
  TOOL_SWIFT = 2,
  TOOL_LD = 3,
  TOOL_MOLD = 54321, // Randomly chosen
};

enum : u32 {
  OBJC_IMAGE_SUPPORTS_GC = 1 << 1,
  OBJC_IMAGE_REQUIRES_GC = 1 << 2,
  OBJC_IMAGE_OPTIMIZED_BY_DYLD = 1 << 3,
  OBJC_IMAGE_SUPPORTS_COMPACTION = 1 << 4,
  OBJC_IMAGE_IS_SIMULATED = 1 << 5,
  OBJC_IMAGE_HAS_CATEGORY_CLASS_PROPERTIES = 1 << 6,
};

enum : u32 {
  LOH_ARM64_ADRP_ADRP = 1,
  LOH_ARM64_ADRP_LDR = 2,
  LOH_ARM64_ADRP_ADD_LDR = 3,
  LOH_ARM64_ADRP_LDR_GOT_LDR = 4,
  LOH_ARM64_ADRP_ADD_STR = 5,
  LOH_ARM64_ADRP_LDR_GOT_STR = 6,
  LOH_ARM64_ADRP_ADD = 7,
  LOH_ARM64_ADRP_LDR_GOT = 8,
};

enum : u32 {
  ARM64_RELOC_UNSIGNED = 0,
  ARM64_RELOC_SUBTRACTOR = 1,
  ARM64_RELOC_BRANCH26 = 2,
  ARM64_RELOC_PAGE21 = 3,
  ARM64_RELOC_PAGEOFF12 = 4,
  ARM64_RELOC_GOT_LOAD_PAGE21 = 5,
  ARM64_RELOC_GOT_LOAD_PAGEOFF12 = 6,
  ARM64_RELOC_POINTER_TO_GOT = 7,
  ARM64_RELOC_TLVP_LOAD_PAGE21 = 8,
  ARM64_RELOC_TLVP_LOAD_PAGEOFF12 = 9,
  ARM64_RELOC_ADDEND = 10,
};

enum : u32 {
  X86_64_RELOC_UNSIGNED = 0,
  X86_64_RELOC_SIGNED = 1,
  X86_64_RELOC_BRANCH = 2,
  X86_64_RELOC_GOT_LOAD = 3,
  X86_64_RELOC_GOT = 4,
  X86_64_RELOC_SUBTRACTOR = 5,
  X86_64_RELOC_SIGNED_1 = 6,
  X86_64_RELOC_SIGNED_2 = 7,
  X86_64_RELOC_SIGNED_4 = 8,
  X86_64_RELOC_TLV = 9,
};

template <>
inline std::string rel_to_string<ARM64>(u8 type) {
  switch (type) {
  case ARM64_RELOC_UNSIGNED: return "ARM64_RELOC_UNSIGNED";
  case ARM64_RELOC_SUBTRACTOR: return "ARM64_RELOC_SUBTRACTOR";
  case ARM64_RELOC_BRANCH26: return "ARM64_RELOC_BRANCH26";
  case ARM64_RELOC_PAGE21: return "ARM64_RELOC_PAGE21";
  case ARM64_RELOC_PAGEOFF12: return "ARM64_RELOC_PAGEOFF12";
  case ARM64_RELOC_GOT_LOAD_PAGE21: return "ARM64_RELOC_GOT_LOAD_PAGE21";
  case ARM64_RELOC_GOT_LOAD_PAGEOFF12: return "ARM64_RELOC_GOT_LOAD_PAGEOFF12";
  case ARM64_RELOC_POINTER_TO_GOT: return "ARM64_RELOC_POINTER_TO_GOT";
  case ARM64_RELOC_TLVP_LOAD_PAGE21: return "ARM64_RELOC_TLVP_LOAD_PAGE21";
  case ARM64_RELOC_TLVP_LOAD_PAGEOFF12: return "ARM64_RELOC_TLVP_LOAD_PAGEOFF12";
  case ARM64_RELOC_ADDEND: return "ARM64_RELOC_ADDEND";
  }
  return "unknown (" + std::to_string(type) + ")";
}

template <>
inline std::string rel_to_string<X86_64>(u8 type) {
  switch (type) {
  case X86_64_RELOC_UNSIGNED: return "X86_64_RELOC_UNSIGNED";
  case X86_64_RELOC_SIGNED: return "X86_64_RELOC_SIGNED";
  case X86_64_RELOC_BRANCH: return "X86_64_RELOC_BRANCH";
  case X86_64_RELOC_GOT_LOAD: return "X86_64_RELOC_GOT_LOAD";
  case X86_64_RELOC_GOT: return "X86_64_RELOC_GOT";
  case X86_64_RELOC_SUBTRACTOR: return "X86_64_RELOC_SUBTRACTOR";
  case X86_64_RELOC_SIGNED_1: return "X86_64_RELOC_SIGNED_1";
  case X86_64_RELOC_SIGNED_2: return "X86_64_RELOC_SIGNED_2";
  case X86_64_RELOC_SIGNED_4: return "X86_64_RELOC_SIGNED_4";
  case X86_64_RELOC_TLV: return "X86_64_RELOC_TLV";
  }
  return "unknown (" + std::to_string(type) + ")";
}

struct FatHeader {
  ub32 magic;
  ub32 nfat_arch;
};

struct FatArch {
  ub32 cputype;
  ub32 cpusubtype;
  ub32 offset;
  ub32 size;
  ub32 align;
};

struct MachHeader {
  ul32 magic;
  ul32 cputype;
  ul32 cpusubtype;
  ul32 filetype;
  ul32 ncmds;
  ul32 sizeofcmds;
  ul32 flags;
  ul32 reserved;
};

struct LoadCommand {
  ul32 cmd;
  ul32 cmdsize;
};

template <typename E>
struct SegmentCommand {
  std::string_view get_segname() const {
    return {segname, strnlen(segname, sizeof(segname))};
  }

  ul32 cmd;
  ul32 cmdsize;
  char segname[16];
  Word<E> vmaddr;
  Word<E> vmsize;
  Word<E> fileoff;
  Word<E> filesize;
  ul32 maxprot;
  ul32 initprot;
  ul32 nsects;
  ul32 flags;
};

template <typename E>
struct MachSection {
  void set_segname(std::string_view name) {
    assert(name.size() <= sizeof(segname));
    memcpy(segname, name.data(), name.size());
  }

  std::string_view get_segname() const {
    return {segname, strnlen(segname, sizeof(segname))};
  }

  void set_sectname(std::string_view name) {
    assert(name.size() <= sizeof(sectname));
    memcpy(sectname, name.data(), name.size());
  }

  std::string_view get_sectname() const {
    return {sectname, strnlen(sectname, sizeof(sectname))};
  }

  bool match(std::string_view segname, std::string_view sectname) const {
    return get_segname() == segname && get_sectname() == sectname;
  }

  bool is_text() const {
    return (attr & S_ATTR_SOME_INSTRUCTIONS) || (attr & S_ATTR_PURE_INSTRUCTIONS);
  }

  char sectname[16];
  char segname[16];
  Word<E> addr;
  Word<E> size;
  ul32 offset;
  ul32 p2align;
  ul32 reloff;
  ul32 nreloc;
  u8 type;
  ul24 attr;
  ul32 reserved1;
  ul32 reserved2;
  ul32 reserved3;
};

struct DylibCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 nameoff;
  ul32 timestamp;
  ul32 current_version;
  ul32 compatibility_version;
};

struct DylinkerCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 nameoff;
};

struct SymtabCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 symoff;
  ul32 nsyms;
  ul32 stroff;
  ul32 strsize;
};

struct DysymtabCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 ilocalsym;
  ul32 nlocalsym;
  ul32 iextdefsym;
  ul32 nextdefsym;
  ul32 iundefsym;
  ul32 nundefsym;
  ul32 tocoff;
  ul32 ntoc;
  ul32 modtaboff;
  ul32 nmodtab;
  ul32 extrefsymoff;
  ul32 nextrefsyms;
  ul32 indirectsymoff;
  ul32 nindirectsyms;
  ul32 extreloff;
  ul32 nextrel;
  ul32 locreloff;
  ul32 nlocrel;
};

struct VersionMinCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 version;
  ul32 sdk;
};

struct DyldInfoCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 rebase_off;
  ul32 rebase_size;
  ul32 bind_off;
  ul32 bind_size;
  ul32 weak_bind_off;
  ul32 weak_bind_size;
  ul32 lazy_bind_off;
  ul32 lazy_bind_size;
  ul32 export_off;
  ul32 export_size;
};

struct UUIDCommand {
  ul32 cmd;
  ul32 cmdsize;
  u8 uuid[16];
};

struct RpathCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 path_off;
};

struct LinkEditDataCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 dataoff;
  ul32 datasize;
};

struct BuildToolVersion {
  ul32 tool;
  ul32 version;
};

struct BuildVersionCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 platform;
  ul32 minos;
  ul32 sdk;
  ul32 ntools;
};

struct EntryPointCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul64 entryoff;
  ul64 stacksize;
};

struct SourceVersionCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul64 version;
};

struct DataInCodeEntry {
  ul32 offset;
  ul16 length;
  ul16 kind;
};

struct UmbrellaCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 umbrella_off;
};

struct LinkerOptionCommand {
  ul32 cmd;
  ul32 cmdsize;
  ul32 count;
};

// This struct is named `n_list` on BSD and macOS.
template <typename E>
struct MachSym {
  bool is_undef() const {
    return type == N_UNDF && !is_common();
  }

  bool is_common() const {
    return type == N_UNDF && is_extern && value;
  }

  ul32 stroff;

  union {
    u8 n_type;
    struct {
      u8 is_extern : 1;
      u8 type : 3;
      u8 is_private_extern : 1;
      u8 stab : 3;
    };
  };

  u8 sect;

  union {
    ul16 desc;
    struct {
      u8 padding;
      u8 common_p2align : 4;
    };
  };

  Word<E> value;
};

// This struct is named `relocation_info` on BSD and macOS.
struct MachRel {
  ul32 offset;
  ul24 idx;
  u8 is_pcrel : 1;
  u8 p2size : 2;
  u8 is_extern : 1;
  u8 type : 4;
};

// __TEXT,__unwind_info section contents

enum : u32 {
  UNWIND_SECTION_VERSION = 1,
  UNWIND_SECOND_LEVEL_REGULAR = 2,
  UNWIND_SECOND_LEVEL_COMPRESSED = 3,
  UNWIND_PERSONALITY_MASK = 0x30000000,
};

enum : u32 {
  UNWIND_MODE_MASK = 0x0f000000,
  UNWIND_ARM64_MODE_DWARF = 0x03000000,
  UNWIND_X86_64_MODE_STACK_IND = 0x03000000,
  UNWIND_X86_64_MODE_DWARF = 0x04000000,
};

struct UnwindSectionHeader {
  ul32 version;
  ul32 encoding_offset;
  ul32 encoding_count;
  ul32 personality_offset;
  ul32 personality_count;
  ul32 page_offset;
  ul32 page_count;
};

struct UnwindFirstLevelPage {
  ul32 func_addr;
  ul32 page_offset;
  ul32 lsda_offset;
};

struct UnwindSecondLevelPage {
  ul32 kind;
  ul16 page_offset;
  ul16 page_count;
  ul16 encoding_offset;
  ul16 encoding_count;
};

struct UnwindLsdaEntry {
  ul32 func_addr;
  ul32 lsda_addr;
};

struct UnwindPageEntry {
  ul24 func_addr;
  u8 encoding;
};

// __LD,__compact_unwind section contents
template <typename E>
struct CompactUnwindEntry {
  Word<E> code_start;
  ul32 code_len;
  ul32 encoding;
  Word<E> personality;
  Word<E> lsda;
};

// __LINKEDIT,__code_signature

enum : u32 {
  CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0,
  CS_SUPPORTSEXECSEG = 0x20400,
  CSMAGIC_CODEDIRECTORY = 0xfade0c02,
  CSSLOT_CODEDIRECTORY = 0,
  CS_ADHOC = 0x00000002,
  CS_LINKER_SIGNED = 0x00020000,
  CS_EXECSEG_MAIN_BINARY = 1,
  CS_HASHTYPE_SHA256 = 2,
};

struct CodeSignatureHeader {
  ub32 magic;
  ub32 length;
  ub32 count;
};

struct CodeSignatureBlobIndex {
  ub32 type;
  ub32 offset;
  ub32 padding;
};

struct CodeSignatureDirectory {
  ub32 magic;
  ub32 length;
  ub32 version;
  ub32 flags;
  ub32 hash_offset;
  ub32 ident_offset;
  ub32 n_special_slots;
  ub32 n_code_slots;
  ub32 code_limit;
  u8 hash_size;
  u8 hash_type;
  u8 platform;
  u8 page_size;
  ub32 spare2;
  ub32 scatter_offset;
  ub32 team_offset;
  ub32 spare3;
  ub64 code_limit64;
  ub64 exec_seg_base;
  ub64 exec_seg_limit;
  ub64 exec_seg_flags;
};

// __DATA,__objc_imageinfo
struct ObjcImageInfo {
  ul32 version = 0;
  u8 flags = 0;
  u8 swift_version = 0;
  ul16 swift_lang_version = 0;
};

// __LINKEDIT,__chainfixups
struct DyldChainedFixupsHeader {
  ul32 fixups_version;
  ul32 starts_offset;
  ul32 imports_offset;
  ul32 symbols_offset;
  ul32 imports_count;
  ul32 imports_format;
  ul32 symbols_format;
};

struct DyldChainedStartsInImage {
  ul32 seg_count;
  ul32 seg_info_offset[];
};

struct DyldChainedStartsInSegment {
  ul32 size;
  ul16 page_size;
  ul16 pointer_format;
  ul64 segment_offset;
  ul32 max_valid_pointer;
  ul16 page_count;
  ul16 page_start[];
};

struct DyldChainedPtr64Rebase {
  u64 target   : 36;
  u64 high8    :  8;
  u64 reserved :  7;
  u64 next     : 12;
  u64 bind     :  1;
};

struct DyldChainedPtr64Bind {
  u64 ordinal  : 24;
  u64 addend   :  8;
  u64 reserved : 19;
  u64 next     : 12;
  u64 bind     :  1;
};

struct DyldChainedImport {
  u32 lib_ordinal :  8;
  u32 weak_import :  1;
  u32 name_offset : 23;
};

struct DyldChainedImportAddend {
  u32 lib_ordinal :  8;
  u32 weak_import :  1;
  u32 name_offset : 23;
  u32 addend;
};

struct DyldChainedImportAddend64 {
  u64 lib_ordinal : 16;
  u64 weak_import :  1;
  u64 reserved    : 15;
  u64 name_offset : 32;
  u64 addend;
};

enum : u32 {
  DYLD_CHAINED_PTR_ARM64E = 1,
  DYLD_CHAINED_PTR_64 = 2,
  DYLD_CHAINED_PTR_32 = 3,
  DYLD_CHAINED_PTR_32_CACHE = 4,
  DYLD_CHAINED_PTR_32_FIRMWARE = 5,
  DYLD_CHAINED_PTR_START_NONE = 0xFFFF,
  DYLD_CHAINED_PTR_START_MULTI = 0x8000,
  DYLD_CHAINED_PTR_START_LAST = 0x8000,
};

enum : u32 {
  DYLD_CHAINED_IMPORT = 1,
  DYLD_CHAINED_IMPORT_ADDEND = 2,
  DYLD_CHAINED_IMPORT_ADDEND64 = 3,
};

enum : u32 {
  DYLD_CHAINED_PTR_64_OFFSET = 6,
  DYLD_CHAINED_PTR_ARM64E_OFFSET = 7,
};

struct ARM64 {
  static constexpr u32 cputype = CPU_TYPE_ARM64;
  static constexpr u32 cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  static constexpr u32 page_size = 16384;
  static constexpr u32 abs_rel = ARM64_RELOC_UNSIGNED;
  static constexpr u32 subtractor_rel = ARM64_RELOC_SUBTRACTOR;
  static constexpr u32 gotpc_rel = ARM64_RELOC_POINTER_TO_GOT;
  static constexpr u32 stub_size = 12;
  static constexpr u32 stub_helper_hdr_size = 24;
  static constexpr u32 stub_helper_size = 12;
};

struct ARM64_32 {
  static constexpr u32 cputype = CPU_TYPE_ARM64_32;
  static constexpr u32 cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  static constexpr u32 page_size = 16384;
  static constexpr u32 abs_rel = ARM64_RELOC_UNSIGNED;
  static constexpr u32 subtractor_rel = ARM64_RELOC_SUBTRACTOR;
  static constexpr u32 gotpc_rel = ARM64_RELOC_POINTER_TO_GOT;
  static constexpr u32 stub_size = 12;
  static constexpr u32 stub_helper_hdr_size = 24;
  static constexpr u32 stub_helper_size = 12;
};

struct X86_64 {
  static constexpr u32 cputype = CPU_TYPE_X86_64;
  static constexpr u32 cpusubtype = CPU_SUBTYPE_X86_64_ALL;
  static constexpr u32 page_size = 4096;
  static constexpr u32 abs_rel = X86_64_RELOC_UNSIGNED;
  static constexpr u32 subtractor_rel = X86_64_RELOC_SUBTRACTOR;
  static constexpr u32 gotpc_rel = X86_64_RELOC_GOT;
  static constexpr u32 stub_size = 6;
  static constexpr u32 stub_helper_hdr_size = 16;
  static constexpr u32 stub_helper_size = 10;
};

} // namespace mold::macho
