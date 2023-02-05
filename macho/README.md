# Mach-O linker overview

Mach-O is an executable and linkable file format. It was originally
created for the Mach kernel-based system. Since NeXTSTEP is based on the
Mach kernel and the modern macOS/iOS is a direct descendent of it, Mach-O
is now universally used as an object file for the Apple systems.

In this document, I'll explain Mach-O by comparing it with ELF.

## Sections and subsections

Section is not an atomic unit of copying in Mach-O. If a section has the
`MH_SUBSECTIONS_VIA_SYMBOLS` flag, it can be split at locations where
global symbols points to. Splitted section is called "subsection".

For example, assume your source file contains two function definitions,
`foo` and `bar`. The compiled object file contains only one text section
which consists of two subsections. One subsection starts at where `foo`
refers to, and the other starts at where `bar` refers to.

In other words, Mach-O's section is not equivalent to ELF's section;
Mach-O's subsection is.

## Relocations

A Mach-O relocation can refer to either a symbol or a section, while an
ELF relocation can refer to only a symbol.

## Dynamic name resolution

In Mach-O, dynamic symbols are resolved with a tuple of (dylib's name,
symbol name). That means dynamic symbols must be resolved by their
corresponding dynamic libraries at runtime.

This is contrary to ELF in which dynamic symbols are resolved only by
name. In ELF, dynamic symbols can be resolved by any dynamic library as
long as their names match.

If a dylib in a special value `BIND_SPECIAL_DYLIB_FLAT_LOOKUP`, the symbol
name is looked up from all dylibs at runtime. This gives Mach-O the
semantics that are the same as the ELF's flat namespace. This special value
is used in the following rare occasions:

1. If a symbol name specified as an argument for `-U` is missing at
   link-time, the symbol is promoted to a dynamic symbol so that it'll get
   another chance to be resolved at load-time. Since we don't know the
   library name for such symbol, we'll set `BIND_SPECIAL_DYLIB_FLAT_LOOKUP`
   as a library identifier instead.

2. If `-flat_namespace` is given, all dynamic symbols are exported without
   specifying their corresponding library names. This option gives the
   ELF semantics to Mach-O programs.

The other difference between Mach-O and ELF Is that Mach-O's dynamic
symbols are stored in a trie so that symbols' common prefixes are shared
in a file. In ELF, dynamic symbol strings are just a run of NUL-terminated
strings. So, Mach-O's import table is more complicated but compact than
ELF's table.

## Lazy function symbol resolution

Imported function symbols are resolved in the same way as ELF but with
slightly different file layout.

`__stubs` section contains PLT entries. It reads a function address from
`__la_symbol_ptr` section and jump there.

`__la_symbol_ptr` entries are initialized to point to entries in
`__stubs_helper` section. If a PLT entry is called for the first time, the
control is transferred to its corresponding entry in `__stubs_helper`.
It then calls `dyld_stub_binder` with appropriate arguments for symbol
resolution.

There's no notion of "canonical PLT" in Mach-O because the compiler always
emit code to load a function pointer value from GOT even for `-fno-PIC`.
In other words, the compiler always assumes that the address of a function
is not known at link-time if the address is used as a value.

## Re-exported libraries

Mach-O dylibs can refer to other dylibs as "reexported libraries". If
`libfoo.dylib` refers to `libbar.dylib` as a reexported library, all symbols
defined by `libbar.dylib` become available as if they were defined by
`libfoo.dylib`.

The purpose of reexporting is to give developers freedom to reorganize their
libraries while keeping the programming interface the same. For example,
macOS's `libSystem.dylib` provides basic functionalities for macOS apps.
Apple may want to move some features out of `libSystem.dylib` to a new
dylib. They can safely do it by referring to the new dylib as a reexported
library of `libSystem.dylib`. If there's no reexporting feature, they can't
do this without asking the library users to add a new `-l` line to their
build files.

## Thread-local variables

If a symbol is of thread-local variable (TLV), the symbol is defined in a
`__thread_vars` section in an input file. A `__thread_vars` entry consists of
a vector of the three word-size tuples that contains

1. a function pointer to `tlv_get_addr`,
2. an indirect pointer to the TLS block of the Mach-O image, and
3. the offset within the TLS block

at runtime. You can obtain the address of a TLV by calling the function
pointer with the address of the tuple as an argument.

TLVs are always referenced by `ARM64_RELOC_TLVP_LOAD_*` or `X86_64_RELOC_TLV`
relocations. For each TLV referenced by these relocations, the linker
creates an entry in the linker-synthesized `__thread_ptrs` section and let
it refer to the address of its corresponding `__thread_vars` entry.

TLVs are always referenced indirectly via `__thread_ptrs`. If a TLV is
defined locally, its `__thread_ptrs` entry refers to a `__thread_vars`
entry in the same file. Otherwise, the `__thread_ptr` entry refers to a
`__thread_vars` entry in other Mach-O file image. In other words,
`__thread_ptrs` is a GOT-like section for TLVs.

A TLS template image consists of `__thread_data` and `__thread_bss` sections.
`__thread_data` contains initial values for TLVs. `__thread_bss` specifies
only its size. These sections must be consecutive in the output file;
otherwise each per-thread block contains unrelated piece of data, resulting
in waste of memory.

## Endianness and bitwidth

Since Mach-O is effectively used only by Apple, and all Apple systems are
little-endian nowadays, Mach-O is effectively little-endian only.
Likewise, it is 64-bit only since Apple has terminated 32-bit app support.

Apple Watch seems to be using an ILP32 ABI on ARM64, but I don't know the
details about it.
