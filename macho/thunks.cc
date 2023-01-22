#include "mold.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>

namespace mold::macho {

using E = ARM64;

// We create a thunk no further than 100 MiB from any section.
static constexpr i64 MAX_DISTANCE = 100 * 1024 * 1024;

// We create a thunk for each 10 MiB input sections.
static constexpr i64 GROUP_SIZE = 10 * 1024 * 1024;

static bool is_reachable(Context<E> &ctx, Symbol<E> &sym,
                         Subsection<E> &subsec, Relocation<E> &rel) {
  // We pessimistically assume that PLT entries are unreacahble.
  if (sym.stub_idx != -1)
    return false;

  // We create thunks with a pessimistic assumption that all
  // out-of-section relocations would be out-of-range.
  if (!sym.subsec || &sym.subsec->isec.osec != &subsec.isec.osec)
    return false;

  if (sym.subsec->output_offset == -1)
    return false;

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
void create_range_extension_thunks(Context<E> &ctx, OutputSection<E> &osec) {
  std::span<Subsection<E> *> members = osec.members;
  if (members.empty())
    return;

  members[0]->output_offset = 0;

  // Initialize input sections with a dummy offset so that we can
  // distinguish sections that have got an address with the one who
  // haven't.
  tbb::parallel_for((i64)1, (i64)members.size(), [&](i64 i) {
    members[i]->output_offset = -1;
  });

  // We create thunks from the beginning of the section to the end.
  // We manage progress using four offsets which increase monotonically.
  // The locations they point to are always A <= B <= C <= D.
  i64 a = 0;
  i64 b = 0;
  i64 c = 0;
  i64 d = 0;
  i64 offset = 0;

  while (b < members.size()) {
    // Move D foward as far as we can jump from B to D.
    while (d < members.size() &&
           offset - members[b]->output_offset < MAX_DISTANCE) {
      offset = align_to(offset, 1 << members[d]->p2align);
      members[d]->output_offset = offset;
      offset += members[d]->input_size;
      d++;
    }

    // Move C forward so that C is apart from B by GROUP_SIZE.
    while (c < members.size() &&
           members[c]->output_offset - members[b]->output_offset < GROUP_SIZE)
      c++;

    // Move A forward so that A is reachable from C.
    if (c > 0) {
      i64 c_end = members[c - 1]->output_offset + members[c - 1]->input_size;
      while (a < osec.thunks.size() &&
             osec.thunks[a]->offset < c_end - MAX_DISTANCE)
        reset_thunk(*osec.thunks[a++]);
    }

    // Create a thunk for input sections between B and C and place it at D.
    osec.thunks.emplace_back(new RangeExtensionThunk<E>{osec});

    RangeExtensionThunk<E> &thunk = *osec.thunks.back();
    thunk.thunk_idx = osec.thunks.size() - 1;
    thunk.offset = offset;

    // Scan relocations between B and C to collect symbols that need thunks.
    tbb::parallel_for_each(members.begin() + b, members.begin() + c,
                           [&](Subsection<E> *subsec) {
      std::span<Relocation<E>> rels = subsec->get_rels();

      for (i64 i = 0; i < rels.size(); i++) {
        Relocation<E> &r = rels[i];
        if (!r.sym->file || r.type != ARM64_RELOC_BRANCH26)
          continue;

        // Skip if the destination is within reach.
        if (is_reachable(ctx, *r.sym, *subsec, r))
          continue;

        // If the symbol is already in another thunk, reuse it.
        if (r.sym->thunk_idx != -1) {
          r.thunk_idx = r.sym->thunk_idx;
          r.thunk_sym_idx = r.sym->thunk_sym_idx;
          continue;
        }

        // Otherwise, add the symbol to this thunk if it's not added already.
        r.thunk_idx = thunk.thunk_idx;
        r.thunk_sym_idx = -1;

        if (!(r.sym->flags.fetch_or(NEEDS_RANGE_EXTN_THUNK) &
              NEEDS_RANGE_EXTN_THUNK)) {
          std::scoped_lock lock(thunk.mu);
          thunk.symbols.push_back(r.sym);
        }
      }
    });

    // Now that we know the number of symbols in the thunk, we can compute
    // its size.
    offset += thunk.size();

    // Sort symbols added to the thunk to make the output deterministic.
    sort(thunk.symbols, [](Symbol<E> *a, Symbol<E> *b) {
      return std::tuple{a->file->priority, a->value} <
             std::tuple{b->file->priority, b->value};
    });

    // Assign offsets within the thunk to the symbols.
    for (i64 i = 0; Symbol<E> *sym : thunk.symbols) {
      sym->thunk_idx = thunk.thunk_idx;
      sym->thunk_sym_idx = i++;
    }

    // Scan relocations again to fix symbol offsets in the last thunk.
    tbb::parallel_for_each(members.begin() + b, members.begin() + c,
                           [&](Subsection<E> *subsec) {
      for (Relocation<E> &r : subsec->get_rels())
        if (r.thunk_idx == thunk.thunk_idx)
          r.thunk_sym_idx = r.sym->thunk_sym_idx;
    });

    // Move B forward to point to the begining of the next group.
    b = c;
  }

  while (a < osec.thunks.size())
    reset_thunk(*osec.thunks[a++]);

  osec.hdr.size = offset;
}

} // namespace mold::macho
