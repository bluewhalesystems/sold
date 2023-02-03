#include "mold.h"

namespace mold::macho {

using E = ARM64;

static u64 page(u64 val) {
  return val & 0xffff'ffff'ffff'f000;
}

static u64 page_offset(u64 hi, u64 lo) {
  u64 val = page(hi) - page(lo);
  return (bits(val, 13, 12) << 29) | (bits(val, 32, 14) << 5);
}

// Write an immediate to an ADD, LDR or STR instruction.
static void write_add_ldst(u8 *loc, u32 val) {
  u32 insn = *(ul32 *)loc;
  i64 scale = 0;

  if ((insn & 0x3b000000) == 0x39000000) {
    // LDR/STR accesses an aligned 1, 2, 4, 8 or 16 byte data on memory.
    // The immediate is scaled by the data size, so we need to know the
    // data size to write a correct immediate.
    //
    // The most significant two bits of the instruction usually
    // specifies the data size.
    scale = bits(insn, 31, 30);

    // Vector and byte LDR/STR shares the same scale bits.
    // We can distinguish them by looking at other bits.
    if (scale == 0 && (insn & 0x04800000) == 0x04800000)
      scale = 4;
  }

  *(ul32 *)loc |= bits(val, 11, scale) << 10;
}

template <>
void StubsSection<E>::copy_buf(Context<E> &ctx) {
  ul32 *buf = (ul32 *)(ctx.buf + this->hdr.offset);

  for (i64 i = 0, j = 0; i < syms.size(); i++) {
    static const ul32 insn[] = {
      0x90000010, // adrp x16, $ptr@PAGE
      0xf9400210, // ldr  x16, [x16, $ptr@PAGEOFF]
      0xd61f0200, // br   x16
    };

    static_assert(sizeof(insn) == E::stub_size);

    u64 this_addr = this->hdr.addr + E::stub_size * i;

    u64 ptr_addr;
    if (syms[i]->has_got())
      ptr_addr = syms[i]->get_got_addr(ctx);
    else
      ptr_addr = ctx.lazy_symbol_ptr->hdr.addr + word_size * j++;

    memcpy(buf, insn, sizeof(insn));
    buf[0] |= page_offset(ptr_addr, this_addr);
    buf[1] |= bits(ptr_addr, 11, 3) << 10;
    buf += 3;
  }
}

// __stubs_helper contains code to call the dynamic symbol resolver for
// PLT.
template <>
void StubHelperSection<E>::copy_buf(Context<E> &ctx) {
  ul32 *start = (ul32 *)(ctx.buf + this->hdr.offset);
  ul32 *buf = start;

  static const ul32 insn0[] = {
    0x90000011, // adrp x17, $__dyld_private@PAGE
    0x91000231, // add  x17, x17, $__dyld_private@PAGEOFF
    0xa9bf47f0, // stp  x16, x17, [sp, #-16]!
    0x90000010, // adrp x16, $dyld_stub_binder@PAGE
    0xf9400210, // ldr  x16, [x16, $dyld_stub_binder@PAGEOFF]
    0xd61f0200, // br   x16
  };

  static_assert(sizeof(insn0) == E::stub_helper_hdr_size);
  memcpy(buf, insn0, sizeof(insn0));

  u64 dyld_private = ctx.__dyld_private->get_addr(ctx);
  buf[0] |= page_offset(dyld_private, this->hdr.addr);
  buf[1] |= bits(dyld_private, 11, 0) << 10;

  u64 stub_binder = ctx.dyld_stub_binder->get_got_addr(ctx);
  buf[3] |= page_offset(stub_binder, this->hdr.addr - 12);
  buf[4] |= bits(stub_binder, 11, 0) << 10;

  buf += 6;

  for (i64 i = 0; Symbol<E> *sym : ctx.stubs.syms) {
    if (sym->has_got())
      continue;

    static const ul32 insn[] = {
      0x18000050, // ldr  w16, addr
      0x14000000, // b    stubHelperHeader
      0x00000000, // addr: .long <idx>
    };

    static_assert(sizeof(insn) == E::stub_helper_size);

    memcpy(buf, insn, sizeof(insn));
    buf[1] |= bits((start - buf - 1) * 4, 27, 2);
    buf[2] = ctx.lazy_bind->bind_offsets[i];
    buf += 3;
    i++;
  }
}

// For each _objc_msgSend$<classname>, we create an entry in _objc_stubs
// section. Each entry consist of machine code that calls _objc_msgSend
// with the address to an interned class name as an argument.
template <>
void ObjcStubsSection<E>::copy_buf(Context<E> &ctx) {
  if (this->hdr.size == 0)
    return;

  static const ul32 insn[] = {
    0x90000001, // adrp  x1, @selector("foo")@PAGE
    0xf9400021, // ldr   x1, [x1, @selector("foo")@PAGEOFF]
    0x90000010, // adrp  x16, _objc_msgSend@GOTPAGE
    0xf9400210, // ldr   x16, [x16, _objc_msgSend@GOTPAGEOFF]
    0xd61f0200, // br    x16
    0xd4200020, // brk   #0x1
    0xd4200020, // brk   #0x1
    0xd4200020, // brk   #0x1
  };
  static_assert(sizeof(insn) == ENTRY_SIZE);

  u64 msgsend_got_addr = ctx._objc_msgSend->get_got_addr(ctx);

  for (i64 i = 0; i < methnames.size(); i++) {
    ul32 *buf = (ul32 *)(ctx.buf + this->hdr.offset + ENTRY_SIZE * i);
    u64 sel_addr = selrefs[i]->get_addr(ctx);
    u64 ent_addr = this->hdr.addr + ENTRY_SIZE * i;

    memcpy(buf, insn, sizeof(insn));
    buf[0] |= page_offset(sel_addr, ent_addr);
    buf[1] |= bits(sel_addr, 11, 3) << 10;
    buf[2] |= page_offset(msgsend_got_addr, ent_addr + 8);
    buf[3] |= bits(msgsend_got_addr, 11, 3) << 10;
  }
}

template <>
std::vector<Relocation<E>>
read_relocations(Context<E> &ctx, ObjectFile<E> &file, const MachSection &hdr) {
  std::vector<Relocation<E>> vec;
  vec.reserve(hdr.nreloc);

  MachRel *rels = (MachRel *)(file.mf->data + hdr.reloff);

  // Read one Mach-O relocation at a time and create a Relocation for it
  for (i64 i = 0; i < hdr.nreloc; i++) {
    i64 addend = 0;

    // Mach-O relocation doesn't contain an addend. UNSIGNED relocs have
    // addends in the input section they refer to. Users can also specify
    // an addend for other types of relocations by prepending an ADDEND
    // reloc.
    switch (rels[i].type) {
    case ARM64_RELOC_UNSIGNED: {
      i64 size = 1 << rels[i].p2size;
      ASSERT(size == 4 || size == 8);
      if (size == 4)
        addend = *(il32 *)(file.mf->data + hdr.offset + rels[i].offset);
      else
        addend = *(il64 *)(file.mf->data + hdr.offset + rels[i].offset);
      break;
    }
    case ARM64_RELOC_ADDEND:
      addend = sign_extend(rels[i].idx, 23);
      i++;
      break;
    case ARM64_RELOC_POINTER_TO_GOT:
      ASSERT(rels[i].is_pcrel);
      break;
    }

    MachRel &r = rels[i];

    vec.push_back(Relocation<E>{
      .offset = r.offset,
      .type = (u8)r.type,
      .size = (u8)(1 << r.p2size),
    });

    Relocation<E> &rel = vec.back();
    rel.is_subtracted = (i > 0 && rels[i - 1].type == ARM64_RELOC_SUBTRACTOR);

    // A relocation refers to either a symbol or a section
    if (r.is_extern) {
      rel.target = file.syms[r.idx];
      rel.is_sym = true;
      rel.addend = addend;
    } else {
      u64 addr = r.is_pcrel ? (hdr.addr + r.offset + addend) : addend;
      Subsection<E> *target = file.find_subsection(ctx, addr);
      if (!target)
        Fatal(ctx) << file << ": bad relocation: " << r.offset;

      rel.target = target;
      rel.is_sym = false;
      rel.addend = addr - target->input_addr;
    }
  }

  return vec;
}

template <>
void Subsection<E>::scan_relocations(Context<E> &ctx) {
  for (Relocation<E> &r : get_rels()) {
    Symbol<E> *sym = r.sym();
    if (!sym)
      continue;

    if (sym->is_imported && sym->file->is_dylib)
      ((DylibFile<E> *)sym->file)->is_alive = true;

    switch (r.type) {
    case ARM64_RELOC_BRANCH26:
      if (sym->is_imported)
        sym->flags |= NEEDS_STUB;
      break;
    case ARM64_RELOC_GOT_LOAD_PAGE21:
    case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
    case ARM64_RELOC_POINTER_TO_GOT:
      sym->flags |= NEEDS_GOT;
      break;
    case ARM64_RELOC_TLVP_LOAD_PAGE21:
    case ARM64_RELOC_TLVP_LOAD_PAGEOFF12:
      if (!sym->is_tlv)
        Error(ctx) << "illegal thread local variable reference to regular symbol `"
                   << *sym << "`";

      sym->flags |= NEEDS_THREAD_PTR;
      break;
    }
  }
}

template <>
void Subsection<E>::apply_reloc(Context<E> &ctx, u8 *buf) {
  std::span<Relocation<E>> rels = get_rels();

  for (i64 i = 0; i < rels.size(); i++) {
    Relocation<E> &r = rels[i];

    if (r.sym() && !r.sym()->file) {
      Error(ctx) << "undefined symbol: " << isec->file << ": " << *r.sym();
      continue;
    }

    u8 *loc = buf + r.offset;
    u64 S = r.get_addr(ctx);
    u64 A = r.addend;
    u64 P = get_addr(ctx) + r.offset;
    u64 G = r.sym() ? r.sym()->got_idx * word_size : 0;
    u64 GOT = ctx.got.hdr.addr;

    switch (r.type) {
    case ARM64_RELOC_UNSIGNED:
      ASSERT(r.size == 8);

      // Do not write a value if we have a dynamic relocation for this reloc.
      if (r.sym() && r.sym()->is_imported)
        break;

      // __thread_vars contains TP-relative addresses to symbols in the
      // TLS initialization image (i.e. __thread_data and __thread_bss).
      if (r.refers_to_tls())
        *(ul64 *)loc = S + A - ctx.tls_begin;
      else
        *(ul64 *)loc = S + A;
      break;
    case ARM64_RELOC_SUBTRACTOR:
      // A SUBTRACTOR relocation is always followed by an UNSIGNED relocation.
      // They work as a pair to materialize a relative address between two
      // locations.
      ASSERT(r.size == 4 || r.size == 8);
      i++;
      ASSERT(rels[i].type == ARM64_RELOC_UNSIGNED);
      if (r.size == 4)
        *(ul32 *)loc = rels[i].get_addr(ctx) + rels[i].addend - S;
      else
        *(ul64 *)loc = rels[i].get_addr(ctx) + rels[i].addend - S;
      break;
    case ARM64_RELOC_BRANCH26: {
      i64 val = S + A - P;
      if (val < -(1 << 27) || (1 << 27) <= val)
        val = isec->osec.thunks[r.thunk_idx]->get_addr(r.thunk_sym_idx) - P;
      *(ul32 *)loc |= bits(val, 27, 2);
      break;
    }
    case ARM64_RELOC_PAGE21:
      *(ul32 *)loc |= page_offset(S + A, P);
      break;
    case ARM64_RELOC_PAGEOFF12:
      write_add_ldst(loc, S + A);
      break;
    case ARM64_RELOC_GOT_LOAD_PAGE21:
      *(ul32 *)loc |= page_offset(G + GOT + A, P);
      break;
    case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
      write_add_ldst(loc, G + GOT + A);
      break;
    case ARM64_RELOC_POINTER_TO_GOT:
      ASSERT(r.size == 4);
      *(ul32 *)loc = G + GOT + A - P;
      break;
    case ARM64_RELOC_TLVP_LOAD_PAGE21:
      *(ul32 *)loc |= page_offset(r.sym()->get_tlv_addr(ctx) + A, P);
      break;
    case ARM64_RELOC_TLVP_LOAD_PAGEOFF12:
      write_add_ldst(loc, r.sym()->get_tlv_addr(ctx) + A);
      break;
    default:
      Fatal(ctx) << *isec << ": unknown reloc: " << (int)r.type;
    }
  }
}

void RangeExtensionThunk<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + output_section.hdr.offset + offset;

  static const ul32 data[] = {
    0x90000010, // adrp x16, 0   # R_AARCH64_ADR_PREL_PG_HI21
    0x91000210, // add  x16, x16 # R_AARCH64_ADD_ABS_LO12_NC
    0xd61f0200, // br   x16
  };

  static_assert(ENTRY_SIZE == sizeof(data));

  for (i64 i = 0; i < symbols.size(); i++) {
    u64 addr = symbols[i]->get_addr(ctx);
    u64 pc = output_section.hdr.addr + offset + i * ENTRY_SIZE;

    u8 *loc = buf + i * ENTRY_SIZE;
    memcpy(loc , data, sizeof(data));
    *(ul32 *)loc |= page_offset(addr, pc);
    *(ul32 *)(loc + 4) |= bits(addr, 11, 0) << 10;
  }
}

} // namespace mold::macho
