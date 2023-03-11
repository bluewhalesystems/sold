#include "mold.h"

#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for_each.h>

namespace mold::macho {

template <typename E>
static void collect_root_set(Context<E> &ctx,
                             tbb::concurrent_vector<Subsection<E> *> &rootset) {
  Timer t(ctx, "collect_root_set");

  auto add = [&](Symbol<E> *sym) {
    if (sym && sym->subsec)
      rootset.push_back(sym->subsec);
  };

  auto keep = [&](Symbol<E> *sym) {
    if (sym->no_dead_strip)
      return true;
    if (ctx.output_type == MH_DYLIB || ctx.output_type == MH_BUNDLE)
      if (sym->visibility == SCOPE_GLOBAL)
        return true;
    return false;
  };

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (Symbol<E> *sym : file->syms)
      if (sym->file == file && keep(sym))
        add(sym);

    for (Symbol<E> *sym : file->init_functions)
      add(sym);

    for (Subsection<E> *subsec : file->subsections)
      if (const MachSection<E> &hdr = subsec->isec->hdr;
          (hdr.attr & S_ATTR_NO_DEAD_STRIP) ||
          hdr.type == S_MOD_INIT_FUNC_POINTERS ||
          hdr.type == S_MOD_TERM_FUNC_POINTERS)
        rootset.push_back(subsec);

    for (CieRecord<E> &cie : file->cies)
      add(cie.personality);
  });

  for (std::string_view name : ctx.arg.u)
    if (Symbol<E> *sym = get_symbol(ctx, name); sym->file)
      add(sym);

  add(ctx.arg.entry);

  if (ctx.stub_helper)
    add(get_symbol(ctx, "dyld_stub_binder"));
}

template <typename E>
static void visit(Context<E> &ctx, Subsection<E> *subsec) {
  if (!subsec)
    return;

  if (subsec->is_alive.test_and_set())
    return;

  for (Relocation<E> &rel : subsec->get_rels()) {
    if (rel.sym())
      visit(ctx, rel.sym()->subsec);
    else
      visit(ctx, rel.subsec());
  }

  for (UnwindRecord<E> &rec : subsec->get_unwind_records()) {
    visit(ctx, rec.subsec);
    visit(ctx, rec.lsda);

    if (rec.personality)
      visit(ctx, rec.personality->subsec);

    if (rec.fde) {
      visit(ctx, rec.fde->subsec);
      visit(ctx, rec.fde->lsda);
    }
  }
}

template <typename E>
static bool refers_to_live_subsection(Subsection<E> &subsec) {
  for (Relocation<E> &rel : subsec.get_rels()) {
    if (rel.sym()) {
      if (rel.sym()->subsec && rel.sym()->subsec->is_alive)
        return true;
    } else {
      if (rel.subsec()->is_alive)
        return true;
    }
  }
  return false;
}

template <typename E>
static void mark(Context<E> &ctx,
                 tbb::concurrent_vector<Subsection<E> *> &rootset) {
  Timer t(ctx, "mark");

  tbb::parallel_for_each(rootset, [&](Subsection<E> *subsec) {
    visit(ctx, subsec);
  });

  Atomic<bool> repeat;
  do {
    repeat = false;
    tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
      for (Subsection<E> *subsec : file->subsections) {
        if ((subsec->isec->hdr.attr & S_ATTR_LIVE_SUPPORT) &&
            !subsec->is_alive &&
            refers_to_live_subsection(*subsec)) {
          visit(ctx, subsec);
          repeat = true;
        }
      }
    });
  } while (repeat);
}

template <typename E>
static void sweep(Context<E> &ctx) {
  Timer t(ctx, "sweep");

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (Symbol<E> *&sym : file->syms)
      if (sym->file == file && sym->subsec && !sym->subsec->is_alive)
        sym = nullptr;
  });

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    std::erase_if(file->subsections, [](Subsection<E> *subsec) {
      return !subsec->is_alive;
    });
  });
}

template <typename E>
void dead_strip(Context<E> &ctx) {
  Timer t(ctx, "dead_strip");

  tbb::concurrent_vector<Subsection<E> *> rootset;
  collect_root_set(ctx, rootset);
  mark(ctx, rootset);
  sweep(ctx);
}

using E = MOLD_TARGET;

template void dead_strip(Context<E> &);

} // namespace mold::macho
