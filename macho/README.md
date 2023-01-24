# Mach-O linker overview

Mach-O is an executable and linkable file format. It was originally
created for the Mach kernel-based system. Since NeXTSTEP based on the Mach
kernel and the modern macOS/iOS is a direct descendent of it, Mach-O is
now universally used as an object file on Apple's systems.

In this document, I'll explain Mach-O by comparing it with ELF.

## Sections and subsections

Section is not an atomic unit of copying in Mach-O. If a section has the
MH_SUBSECTIONS_VIA_SYMBOLS flag, it can be split at locations where
symbols in the section points to. Splitted section is called "subsection".

For example, assume your source file contains two function definitions,
`foo` and `bar`. The compiled object file for the source file contains
only one text section which consists of two subsections. One subsection
starts at where `foo` refers to, and the other starts at where `bar`
refers to.

So, Mach-O's section is not equivalent to ELF's section; Mach-O's
subsection is.

# Relocations

A Mach-O relocation can refer to either a symbol or a section, while an
ELF relocation can refer to only a symbol.

# Dynamic name resolution

In Mach-O, dynamic symbols are resolved with a tuple of (dylib's name,
symbol name). That means dynamic symbols must be resolved by their
corresponding dynamic libraries at runtime.

This is contrary to ELF in which dynamic symbols are resolved only by
name. In ELF, dynamic symbols can be resolved by any dynamic library as
long as their names match.

Mach-O's dynamic symbols are stored as a trie so that symbols' common
prefixes are shared in a file. In ELF, dynamic symbol strings are just a
run of NUL-terminated strings. So, Mach-O is more complicated but compact.

# Endianness and bitwidth

Since Mach-O is effectively used only by Apple, and all Apple systems are
little-endian, Mach-O is effectively little-endian only. Likewise, it is
64-bit only since Apple has deprecated 32-bit apps.

Apple Watch seems to be using an ILP32 ABI on ARM64, but I don't know the
details about it.
