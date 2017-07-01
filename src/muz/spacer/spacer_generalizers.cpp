/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    spacer_generalizers.cpp

Abstract:

    Lemma generalizers.

Author:

    Nikolaj Bjorner (nbjorner) 2011-11-20.
    Arie Gurfinkel

Revision History:

--*/


#include "spacer_context.h"
#include "spacer_generalizers.h"
#include "expr_abstract.h"
#include "var_subst.h"
#include "for_each_expr.h"
#include "obj_equiv_class.h"


namespace spacer {
void lemma_sanity_checker::operator()(lemma_ref &lemma) {
    unsigned uses_level;
    expr_ref_vector cube(lemma->get_ast_manager());
    cube.append(lemma->get_cube());
    VERIFY(!lemma->get_pob()->pt().check_inductive(lemma->level(),
                                                   cube, uses_level));
}


// ------------------------
// core_bool_inductive_generalizer

// main propositional induction generalizer.
// drop literals one by one from the core and check if the core is still inductive.
//
void lemma_bool_inductive_generalizer::operator()(lemma_ref &lemma)
{
    m_st.count++;
    scoped_watch _w_(m_st.watch);
    if (lemma->get_cube().size() <= 1) {
        return;
    }
    unsigned uses_level;
    pred_transformer &pt = lemma->get_pob()->pt();
    ast_manager& m = pt.get_ast_manager();
    expr_ref_vector cube(m);
    cube.append(lemma->get_cube());
    TRACE("spacer", for (unsigned i = 0; i < cube.size(); ++i)
                    { tout << mk_pp(cube[i].get(), m) << "\n"; });
    unsigned num_failures = 0, i = 0, old_size = cube.size();
    ptr_vector<expr> processed;

    while (i < cube.size() && 1 < cube.size() &&
           (!m_failure_limit || num_failures <= m_failure_limit)) {
        expr_ref lit(m);
        lit = cube[i].get();
        cube[i] = m.mk_true();
        if (pt.check_inductive(lemma->level(), cube, uses_level)) {
            num_failures = 0;
            for (i = 0; i < cube.size() && processed.contains(cube.get(i)); ++i);
        }
        else {
            cube[i] = lit;
            processed.push_back(lit);
            ++num_failures;
            ++m_st.num_failures;
            ++i;
        }
    }
    IF_VERBOSE(2, verbose_stream() << "old size: " << old_size
               << " new size: " << cube.size() << "\n";);
    TRACE("spacer", tout << "old size: " << old_size
          << " new size: " << cube.size() << "\n";);

    if (old_size > cube.size()) {
        lemma->update_cube (lemma->get_pob(), cube);
        SASSERT(uses_level >= lemma->level());
        lemma->set_level(uses_level);
    }
}
void lemma_bool_inductive_generalizer::collect_statistics(statistics &st) const
{
    st.update("time.spacer.solve.reach.gen.bool_ind", m_st.watch.get_seconds());
    st.update("bool inductive gen", m_st.count);
    st.update("bool inductive gen failures", m_st.num_failures);
}

void unsat_core_generalizer::operator()(lemma_ref &lemma)
{
    m_st.count++;
    scoped_watch _w_(m_st.watch);
    ast_manager &m = lemma->get_ast_manager();

    pred_transformer &pt = lemma->get_pob()->pt();

    unsigned old_sz = lemma->get_cube().size();
    unsigned old_level = lemma->level();

    unsigned uses_level;
    expr_ref_vector core(m);
    bool r;
    r = pt.is_invariant(lemma->level(), lemma->get_expr(), uses_level, &core);
    SASSERT(r);

    CTRACE("spacer", old_sz > core.size(),
           tout << "unsat core reduced lemma from: "
           << old_sz << " to " << core.size() << "\n";);
    CTRACE("spacer", old_level < uses_level,
           tout << "unsat core moved lemma up from: "
           << old_level << " to " << uses_level << "\n";);
    if (old_sz > core.size()) {
        lemma->update_cube(lemma->get_pob(), core);
        lemma->set_level(uses_level);
    }
}

void unsat_core_generalizer::collect_statistics(statistics &st) const
{
    st.update("time.spacer.solve.reach.gen.unsat_core", m_st.watch.get_seconds());
    st.update("gen.unsat_core.cnt", m_st.count);
    st.update("gen.unsat_core.fail", m_st.num_failures);
}

namespace {
class collect_array_proc {
    array_util m_au;
    func_decl_set &m_symbs;
    sort *m_sort;
public:
    collect_array_proc(ast_manager &m, func_decl_set& s) :
        m_au(m), m_symbs(s), m_sort(NULL) {}

    void operator()(app* a)
    {
        if (a->get_family_id() == null_family_id && m_au.is_array(a)) {
            if (m_sort && m_sort != get_sort(a)) { return; }
            if (!m_sort) { m_sort = get_sort(a); }
            m_symbs.insert(a->get_decl());
        }
    }
    void operator()(var*) {}
    void operator()(quantifier*) {}
};
}

void lemma_array_eq_generalizer::operator() (lemma_ref &lemma)
{
    TRACE("core_array_eq", tout << "Looking for equalities\n";);

    // -- find array constants
    ast_manager &m = lemma->get_ast_manager();
    manager &pm = m_ctx.get_manager();

    expr_ref_vector core(m);
    expr_ref v(m);
    func_decl_set symb;
    collect_array_proc cap(m, symb);

    core.append (lemma->get_cube());
    v = mk_and(core);
    for_each_expr(cap, v);

    TRACE("core_array_eq",
          tout << "found " << symb.size() << " array variables in: \n"
          << mk_pp(v, m) << "\n";);

    // too few constants
    if (symb.size() <= 1) { return; }
    // too many constants, skip this
    if (symb.size() >= 8) { return; }


    // -- for every pair of variables, try an equality
    typedef func_decl_set::iterator iterator;
    ptr_vector<func_decl> vsymbs;
    for (iterator it = symb.begin(), end = symb.end();
            it != end; ++it)
    { vsymbs.push_back(*it); }

    expr_ref_vector eqs(m);

    for (unsigned i = 0, sz = vsymbs.size(); i < sz; ++i)
        for (unsigned j = i + 1; j < sz; ++j)
        { eqs.push_back(m.mk_eq(m.mk_const(vsymbs.get(i)),
                                m.mk_const(vsymbs.get(j)))); }

    smt::kernel solver(m, m_ctx.get_manager().fparams2());
    expr_ref_vector lits(m);
    for (unsigned i = 0, core_sz = core.size(); i < core_sz; ++i) {
        SASSERT(lits.size() == i);
        solver.push();
        solver.assert_expr(core.get(i));
        for (unsigned j = 0, eqs_sz = eqs.size(); j < eqs_sz; ++j) {
            solver.push();
            solver.assert_expr(eqs.get(j));
            lbool res = solver.check();
            solver.pop(1);

            if (res == l_false) {
                TRACE("core_array_eq",
                      tout << "strengthened " << mk_pp(core.get(i), m)
                      << " with " << mk_pp(m.mk_not(eqs.get(j)), m) << "\n";);
                lits.push_back(m.mk_not(eqs.get(j)));
                break;
            }
        }
        solver.pop(1);
        if (lits.size() == i) { lits.push_back(core.get(i)); }
    }

    /**
       HACK: if the first 3 arguments of pt are boolean, assume
       they correspond to SeaHorn encoding and condition the equality on them.
    */
    // pred_transformer &pt = n.pt ();
    // if (pt.sig_size () >= 3 &&
    //     m.is_bool (pt.sig (0)->get_range ()) &&
    //     m.is_bool (pt.sig (1)->get_range ()) &&
    //     m.is_bool (pt.sig (2)->get_range ()))
    // {
    //   lits.push_back (m.mk_const (pm.o2n(pt.sig (0), 0)));
    //   lits.push_back (m.mk_not (m.mk_const (pm.o2n(pt.sig (1), 0))));
    //   lits.push_back (m.mk_not (m.mk_const (pm.o2n(pt.sig (2), 0))));
    // }

    TRACE("core_array_eq", tout << "new possible core "
          << mk_pp(pm.mk_and(lits), m) << "\n";);


    pred_transformer &pt = lemma->get_pob()->pt();
    // -- check if it is consistent with the transition relation
    unsigned uses_level1;
    if (pt.check_inductive(lemma->level(), lits, uses_level1)) {
        TRACE("core_array_eq", tout << "Inductive!\n";);
        lemma->update_cube(lemma->get_pob(),lits);
        lemma->set_level(uses_level1);
        return;
    } else
    { TRACE("core_array_eq", tout << "Not-Inductive!\n";);}
}

void lemma_eq_generalizer::operator() (lemma_ref &lemma)
{
    TRACE("core_eq", tout << "Transforming equivalence classes\n";);

    ast_manager &m = m_ctx.get_ast_manager();
    expr_ref_vector core(m);
    core.append (lemma->get_cube());

    bool dirty;
    expr_equiv_class eq_classes(m);
    factor_eqs(core, eq_classes);
    // create all possible equalities to allow for simple inductive generalization
    dirty = equiv_to_expr_full(eq_classes, core);
    if (dirty) {
        lemma->update_cube(lemma->get_pob(), core);
    }
}
};
