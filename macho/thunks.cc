#if MOLD_ARM64 || MOLD_ARM64_32

#include "mold.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>

namespace mold::macho {

using E = MOLD_TARGET;

// We create a thunk no further than 100 MiB from any subsection.
static constexpr i64 MAX_DISTANCE = 100 * 1024 * 1024;

// We create a thunk for each 10 MiB subsections.
static constexpr i64 BATCH_SIZE = 10 * 1024 * 1024;

// We assume that a single thunk group is smaller than 100 KiB.
static constexpr i64 MAX_THUNK_SIZE = 102400;

static bool is_reachable(Context<E> &ctx, Symbol<E> &sym,
                         Subsection<E> &subsec, Relocation<E> &rel) {
  if (sym.has_stub()) {
    // If it branches to PLT, the PLT must have already got an address.
    if (ctx.stubs.hdr.addr == 0)
      return false;
  } else {
    // We create thunks with a pessimistic assumption that all
    // out-of-section relocations would be out-of-range.
    if (!sym.subsec || &sym.subsec->isec->osec != &subsec.isec->osec)
      return false;

    // Uninitialized subsections in the same output section are
    // out-of-reach.
    if (sym.subsec->output_offset == -1)
      return false;
  }

  // Compute a distance between the relocated place and the symbol
  // and check if they are within reach.
  i64 addr = sym.get_addr(ctx);
  i64 pc = subsec.get_addr(ctx) + rel.offset;
  i64 val = addr + rel.addend - pc;
  return -(1 << 27) <= val && val < (1 << 27);
}

static void reset_thunk(RangeExtensionThunk<E> &thunk) {
  for (Symbol<E> *sym : thunk.symbols) {
    sym->thunk_idx = -1;
    sym->thunk_sym_idx = -1;
    sym->flags &= (u8)~NEEDS_RANGE_EXTN_THUNK;
  }
}

// ARM64's call/jump instructions take 27 bits displacement, so they
// can refer only up to ±128 MiB. If a branch target is further than
// that, we need to let it branch to a linker-synthesized code
// sequence that construct a full 32 bit address in a register and
// jump there. That linker-synthesized code is called "thunk".
template <>
void create_range_extension_thunks(Context<E> &ctx, OutputSection<E> &osec) {
  std::span<Subsection<E> *> m = osec.members;
  if (m.empty())
    return;

  m[0]->output_offset = 0;

  // Initialize input sections with a dummy offset so that we can
  // distinguish sections that have got an address with the one who
  // haven't.
  tbb::parallel_for((i64)1, (i64)m.size(), [&](i64 i) {
    m[i]->output_offset = -1;
  });

  // We create thunks from the beginning of the section to the end.
  // We manage progress using four offsets which increase monotonically.
  // The locations they point to are always A <= B <= C <= D.
  i64 a = 0;
  i64 b = 0;
  i64 c = 0;
  i64 d = 0;
  i64 offset = 0;
  i64 thunk_idx = 0;

  while (b < m.size()) {
    // Move D foward as far as we can jump from B to D.
    while (d < m.size() &&
           align_to(offset, 1 << m[d]->p2align) + m[d]->input_size <
           m[b]->output_offset + MAX_DISTANCE - MAX_THUNK_SIZE) {
      offset = align_to(offset, 1 << m[d]->p2align);
      m[d]->output_offset = offset;
      offset += m[d]->input_size;
      d++;
    }

    // Move C forward so that C is apart from B by BATCH_SIZE.
    c = b + 1;
    while (c < m.size() &&
           m[c]->output_offset + m[c]->input_size <
           m[b]->output_offset + BATCH_SIZE)
      c++;

    // Move A forward so that A is reachable from C.
    i64 c_offset = (c == m.size()) ? offset : m[c]->output_offset;
    while (a < m.size() && m[a]->output_offset + MAX_DISTANCE < c_offset)
      a++;

    // Erase references to out-of-range thunks.
    while (thunk_idx < osec.thunks.size() &&
           osec.thunks[thunk_idx]->offset < m[a]->output_offset)
      reset_thunk(*osec.thunks[thunk_idx++]);

    // Create a thunk for input sections between B and C and place it at D.
    offset = align_to(offset, RangeExtensionThunk<E>::ALIGNMENT);
    RangeExtensionThunk<E> *thunk =
      new RangeExtensionThunk<E>(osec, osec.thunks.size(), offset);
    osec.thunks.emplace_back(thunk);

    // Scan relocations between B and C to collect symbols that need thunks.
    tbb::parallel_for_each(m.begin() + b, m.begin() + c,
                           [&](Subsection<E> *subsec) {
      std::span<Relocation<E>> rels = subsec->get_rels();

      for (i64 i = 0; i < rels.size(); i++) {
        Relocation<E> &r = rels[i];
        if (!r.sym()->file || r.type != ARM64_RELOC_BRANCH26)
          continue;

        // Skip if the destination is within reach.
        if (is_reachable(ctx, *r.sym(), *subsec, r))
          continue;

        // If the symbol is already in another thunk, reuse it.
        if (r.sym()->thunk_idx != -1) {
          r.thunk_idx = r.sym()->thunk_idx;
          r.thunk_sym_idx = r.sym()->thunk_sym_idx;
          continue;
        }

        // Otherwise, add the symbol to this thunk if it's not added already.
        r.thunk_idx = thunk->thunk_idx;
        r.thunk_sym_idx = -1;

        if (!(r.sym()->flags.fetch_or(NEEDS_RANGE_EXTN_THUNK) &
              NEEDS_RANGE_EXTN_THUNK)) {
          std::scoped_lock lock(thunk->mu);
          thunk->symbols.push_back(r.sym());
        }
      }
    });

    // Now that we know the number of symbols in the thunk, we can compute
    // its size.
    offset += thunk->size();

    // Sort symbols added to the thunk to make the output deterministic.
    sort(thunk->symbols, [](Symbol<E> *a, Symbol<E> *b) {
      return std::tuple{a->file->priority, a->value} <
             std::tuple{b->file->priority, b->value};
    });

    // Assign offsets within the thunk to the symbols.
    for (i64 i = 0; Symbol<E> *sym : thunk->symbols) {
      sym->thunk_idx = thunk->thunk_idx;
      sym->thunk_sym_idx = i++;
    }

    // Scan relocations again to fix symbol offsets in the last thunk.
    tbb::parallel_for_each(m.begin() + b, m.begin() + c,
                           [&](Subsection<E> *subsec) {
      for (Relocation<E> &r : subsec->get_rels())
        if (r.thunk_idx == thunk->thunk_idx)
          r.thunk_sym_idx = r.sym()->thunk_sym_idx;
    });

    // Move B forward to point to the begining of the next group.
    b = c;
  }

  while (thunk_idx < osec.thunks.size())
    reset_thunk(*osec.thunks[thunk_idx++]);

  osec.hdr.size = offset;
}

} // namespace mold::macho

#endif
