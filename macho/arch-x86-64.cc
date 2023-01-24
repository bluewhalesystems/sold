#include "mold.h"

namespace mold::macho {

using E = X86_64;

template <>
void StubsSection<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + this->hdr.offset;

  static const u8 insn[] = {
    0xff, 0x25, 0, 0, 0, 0, // jmp *imm(%rip)
  };

  static_assert(E::stub_size == sizeof(insn));

  for (i64 i = 0; i < syms.size(); i++) {
    memcpy(buf, insn, sizeof(insn));

    u64 dest = ctx.lazy_symbol_ptr.hdr.addr + i * word_size;
    u64 src = this->hdr.addr + i * 6 + 6;
    *(ul32 *)(buf + 2) = dest - src;
    buf += sizeof(insn);
  }
}

template <>
void StubHelperSection<E>::copy_buf(Context<E> &ctx) {
  u8 *start = ctx.buf + this->hdr.offset;
  u8 *buf = start;

  static const u8 insn0[] = {
    0x4c, 0x8d, 0x1d, 0, 0, 0, 0, // lea $__dyld_private(%rip), %r11
    0x41, 0x53,                   // push %r11
    0xff, 0x25, 0, 0, 0, 0,       // jmp *$dyld_stub_binder@GOT(%rip)
    0x90,                         // nop
  };

  static_assert(sizeof(insn0) == E::stub_helper_hdr_size);

  memcpy(buf, insn0, sizeof(insn0));
  *(ul32 *)(buf + 3) =
    get_symbol(ctx, "__dyld_private")->get_addr(ctx) - this->hdr.addr - 7;
  *(ul32 *)(buf + 11) =
    get_symbol(ctx, "dyld_stub_binder")->get_got_addr(ctx) - this->hdr.addr - 15;

  buf += 16;

  for (i64 i = 0; i < ctx.stubs.syms.size(); i++) {
    u8 insn[] = {
      0x68, 0, 0, 0, 0, // push $bind_offset
      0xe9, 0, 0, 0, 0, // jmp $__stub_helper
    };

    static_assert(sizeof(insn) == E::stub_helper_size);

    memcpy(buf, insn, sizeof(insn));
    *(ul32 *)(buf + 1) = ctx.stubs.bind_offsets[i];
    *(ul32 *)(buf + 6) = start - buf - 10;
    buf += 10;
  }
}

template <>
void ObjcStubsSection<E>::copy_buf(Context<E> &ctx) {
  if (this->hdr.size == 0)
    return;

  static const u8 insn[] = {
    0x48, 0x8b, 0x35, 0, 0, 0, 0, // mov @selector("foo")(%rip), %rsi
    0xff, 0x25, 0, 0, 0, 0,       // jmp *_objc_msgSend@GOT(%rip)
    0xcc, 0xcc, 0xcc,             // (padding)
  };
  static_assert(sizeof(insn) == ENTRY_SIZE);

  u64 msgsend_got_addr = ctx._objc_msgSend->get_got_addr(ctx);

  for (i64 i = 0; i < methnames.size(); i++) {
    ul32 *buf = (ul32 *)(ctx.buf + this->hdr.offset + ENTRY_SIZE * i);
    u64 sel_addr = selrefs[i]->get_addr(ctx);
    u64 ent_addr = this->hdr.addr + ENTRY_SIZE * i;

    memcpy(buf, insn, sizeof(insn));
    *(ul32 *)(buf + 3) = sel_addr - ent_addr - 7;
    *(ul32 *)(buf + 9) = msgsend_got_addr - ent_addr - 13;
  }
}

static i64 get_reloc_addend(u32 type) {
  switch (type) {
  case X86_64_RELOC_SIGNED_1:
    return 1;
  case X86_64_RELOC_SIGNED_2:
    return 2;
  case X86_64_RELOC_SIGNED_4:
    return 4;
  default:
    return 0;
  }
}

static i64 read_addend(u8 *buf, const MachRel &r) {
  if (r.p2size == 2)
    return *(il32 *)(buf + r.offset);
  assert(r.p2size == 3);
  return *(il64 *)(buf + r.offset);
}

template <>
std::vector<Relocation<E>>
read_relocations(Context<E> &ctx, ObjectFile<E> &file,
                 const MachSection &hdr) {
  std::vector<Relocation<E>> vec;
  vec.reserve(hdr.nreloc);

  MachRel *rels = (MachRel *)(file.mf->data + hdr.reloff);

  for (i64 i = 0; i < hdr.nreloc; i++) {
    MachRel &r = rels[i];
    vec.push_back({r.offset, (u8)r.type, (u8)(1 << r.p2size)});

    Relocation<E> &rel = vec.back();
    rel.is_pcrel = r.is_pcrel;
    rel.is_subtracted = (i > 0 && rels[i - 1].type == X86_64_RELOC_SUBTRACTOR);

    i64 addend = read_addend(file.mf->data + hdr.offset, r) +
                 get_reloc_addend(r.type);

    if (r.is_extern) {
      rel.sym = file.syms[r.idx];
      rel.addend = addend;
    } else {
      u64 addr = r.is_pcrel ? (hdr.addr + r.offset + addend + 4) : addend;
      Subsection<E> *target = file.find_subsection(ctx, addr);
      if (!target)
        Fatal(ctx) << file << ": bad relocation: " << r.offset;

      rel.subsec = target;
      rel.addend = addr - target->input_addr;
    }
  }

  return vec;
}

template <>
void Subsection<E>::scan_relocations(Context<E> &ctx) {
  for (Relocation<E> &r : get_rels()) {
    Symbol<E> *sym = r.sym;
    if (!sym)
      continue;

    if (sym->is_imported && sym->file->is_dylib)
      ((DylibFile<E> *)sym->file)->is_alive = true;

    switch (r.type) {
    case X86_64_RELOC_UNSIGNED:
      if (sym->is_imported)
        r.needs_dynrel = true;
      break;
    case X86_64_RELOC_GOT:
    case X86_64_RELOC_GOT_LOAD:
      sym->flags |= NEEDS_GOT;
      break;
    case X86_64_RELOC_TLV:
      sym->flags |= NEEDS_THREAD_PTR;
      break;
    }

    if (sym->is_imported)
      sym->flags |= NEEDS_STUB;
  }
}

template <>
void Subsection<E>::apply_reloc(Context<E> &ctx, u8 *buf) {
  std::span<Relocation<E>> rels = get_rels();

  for (i64 i = 0; i < rels.size(); i++) {
    Relocation<E> &r = rels[i];
    u8 *loc = buf + r.offset;

    if (r.sym && !r.sym->file) {
      Error(ctx) << "undefined symbol: " << isec.file << ": " << *r.sym;
      continue;
    }

    u64 S = r.get_addr(ctx);
    u64 A = r.addend;
    u64 P = get_addr(ctx) + r.offset;
    u64 G = r.sym ? r.sym->got_idx * word_size : 0;
    u64 GOT = ctx.got.hdr.addr;

    switch (r.type) {
    case X86_64_RELOC_UNSIGNED:
      ASSERT(!r.is_pcrel);
      ASSERT(r.size == 8);
      if (r.refers_tls())
        *(ul64 *)loc = S + A - ctx.tls_begin;
      else
        *(ul64 *)loc = S + A;
      break;
    case X86_64_RELOC_SUBTRACTOR:
      ASSERT(r.size == 4);
      i++;
      ASSERT(rels[i].type == X86_64_RELOC_UNSIGNED);
      *(ul32 *)loc = rels[i].get_addr(ctx) + rels[i].addend - S;
      break;
    case X86_64_RELOC_SIGNED:
    case X86_64_RELOC_SIGNED_1:
    case X86_64_RELOC_SIGNED_2:
    case X86_64_RELOC_SIGNED_4:
      ASSERT(r.is_pcrel);
      ASSERT(r.size == 4);
      *(ul32 *)loc = S + A - P - 4 - get_reloc_addend(r.type);
      break;
    case X86_64_RELOC_BRANCH:
      ASSERT(r.is_pcrel);
      ASSERT(r.size == 4);
      *(ul32 *)loc = S + A - P - 4;
      break;
    case X86_64_RELOC_GOT_LOAD:
    case X86_64_RELOC_GOT:
      ASSERT(r.is_pcrel);
      ASSERT(r.size == 4);
      *(ul32 *)loc = G + GOT + A - P - 4;
      break;
    case X86_64_RELOC_TLV:
      ASSERT(r.is_pcrel);
      ASSERT(r.size == 4);
      *(ul32 *)loc = r.sym->get_tlv_addr(ctx) + A - P - 4;
      break;
    default:
      Fatal(ctx) << isec << ": unknown reloc: " << (int)r.type;
    }
  }
}

} // namespace mold::macho
