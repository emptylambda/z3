/*

  spacer_cluster.cpp

  Discover and mark lemma clusters.

*/
#include <algorithm>

#include "ast/arith_decl_plugin.h"
#include "ast/ast_util.h"
#include "ast/rewriter/var_subst.h"
#include "ast/substitution/substitution.h"
#include "muz/spacer/spacer_antiunify.h"
#include "muz/spacer/spacer_cluster.h"
#include "muz/spacer/spacer_context.h"
#include "muz/spacer/spacer_manager.h"
#include "muz/spacer/spacer_util.h"
#include "smt/tactic/unit_subsumption_tactic.h"
#include "util/mpq.h"
#include "util/vector.h"

using namespace spacer;
namespace spacer {
lemma_cluster_finder::lemma_cluster_finder(ast_manager &am)
    : m(am), m_arith(m) {}

// returns whether formulas differ only in interpreted constants
bool lemma_cluster_finder::is_intrp_diff(expr_ref antiU_result,
                                         substitution &s1, substitution &s2) {
    SASSERT(s1.get_num_bindings() == s2.get_num_bindings());
    expr_ref e(m), e2(m);
    expr_offset r1, r2;
    var_offset v1, v2;
    for (unsigned j = 0, sz = s1.get_num_bindings(); j < sz; j++) {
        s1.get_binding(j, v1, r1);
        s2.get_binding(j, v2, r2);
        if (!(m_arith.is_numeral(r1.get_expr()) &&
              m_arith.is_numeral(r2.get_expr())))
            return false;
    }
    return true;
}

bool lemma_cluster_finder::are_neighbours(const expr_ref &cube,
                                          const expr_ref &lcube) {
    anti_unifier anti(m);
    expr_ref pat(m);
    substitution sub1(m), sub2(m);
    anti(cube, lcube, pat, sub1, sub2);
    return is_intrp_diff(pat, sub1, sub2);
}

// Compute antiunification of cube with all formulas in fmls. should return
// \exist res (\forall f \in fmls (\exist i_sub res[i_sub] == f)) however, the
// algorithm is incomplete: it returns such a res iff the res \in {antiU(cube,
// e) | e \in fmls}
// TODO: do complete n-ary anti-unification. Not done now
// because anti_unifier does not support free variables
void lemma_cluster_finder::anti_unify_n_intrp(expr_ref &cube,
                                              expr_ref_vector &fmls,
                                              expr_ref_vector &res) {
    expr_ref_vector patterns(m);
    expr_ref pat(m), fml(m);
    substitution sub1(m), sub2(m);
    anti_unifier anti_u(m);
    substitution s1(m), s2(m);

    SASSERT(res.empty());
    TRACE("cluster_stats_verb",
          tout << "Trying to generate a general pattern for " << cube
               << " neighbours are " << fmls << "\n";);

    // collect candidates for res
    for (expr *c : fmls) {
        anti_u.reset();
        s1.reset();
        s2.reset();
        fml.reset();
        fml = expr_ref(c, m);
        SASSERT(are_neighbours(cube, fml));
        anti_u(cube, fml, pat, s1, s2);
        if (get_num_vars(pat) == 0) {
            TRACE("cluster_stats", tout << "Repeated lemmas\n";);
            return;
        }
        if (!patterns.contains(pat))
            patterns.push_back(pat);
    }

    // go through all the patterns to see if there is a pattern which is general
    // enough to include all lemmas.
    bool is_general_pattern = false, pos = true;
    sem_matcher matcher(m);
    unsigned n_vars_pat = 0;
    for (expr *e : patterns) {

        TRACE("cluster_stats_verb",
              tout << "Checking pattern " << mk_pp(e, m) << "\n";);
        is_general_pattern = true;
        n_vars_pat = get_num_vars(e);
        for (auto *lcube : fmls) {
            matcher.reset();
            s1.reset();
            s1.reserve(1, n_vars_pat);
            if (!(matcher(e, lcube, s1, pos) && pos)) {
                // this pattern is no good
                is_general_pattern = false;
                break;
            }
        }
        if (is_general_pattern) {
            SASSERT(e != nullptr);
            TRACE("cluster_stats",
                  tout << "Found a general pattern " << mk_pp(e, m) << "\n";);
            // found a good pattern
            res.push_back(expr_ref(e, m));
        }
    }

    CTRACE("cluster_stats", res.empty(),
           tout << "Failed to find a general pattern for cluster. Cube is: "
                << cube << " Patterns are " << patterns << "\n";);
}

void lemma_cluster_finder::cluster(lemma_ref &lemma) {
    scoped_watch _w_(m_st.watch);
    pred_transformer &pt = (lemma->get_pob())->pt();

    // check whether lemmas has already been added
    if (pt.get_cluster(lemma) != nullptr) return;

    // if the lemma matches a pattern of one of the clusters, but is not in it,
    // add it.
    if (pt.clstr_match(lemma)) {
        TRACE("cluster_stats_verb", tout << "Trying to add lemma "
                                         << lemma->get_cube()
                                         << " to existing clusters\n";);
        pt.add_to_cluster(lemma);

        // avoid creating clusters with lemmas that can't be added to existing
        // clusters
        return;
    }

    // Check whether a new cluster can be formed
    lemma_ref_vector all_lemmas;
    pt.get_all_lemmas(all_lemmas, false);

    expr_ref lcube(m), cube(m);
    lcube = mk_and(lemma->get_cube());
    normalize_order(lcube, lcube);

    expr_ref_vector lma_cubes(m);
    lemma_ref_vector neighbours;

    for (auto *l : all_lemmas) {
        cube.reset();
        cube = mk_and(l->get_cube());
        normalize_order(cube, cube);
        if (are_neighbours(lcube, cube) && cube != lcube) {
            neighbours.push_back(l);
            lma_cubes.push_back(cube);
        }
    }

    if (neighbours.empty()) return;

    // compute the patterns to which lemmas
    expr_ref_vector patterns(m);
    anti_unify_n_intrp(lcube, lma_cubes, patterns);
    bool is_cluster = !patterns.empty();

    // no general pattern
    if (!is_cluster) return;

    for (auto p : patterns) {
        expr_ref pattern(p, m);
        lemma_cluster *cluster = pt.mk_cluster(pattern);

        TRACE("cluster_stats",
              tout << "created new cluster with pattern: " << pattern << "\n"
              << " and lemma cube:\n";
              for (auto f : neighbours) {
                  tout << mk_and(f->get_cube()) << "\n";
              };);

        IF_VERBOSE(1, verbose_stream()
                   << "\ncreated new cluster with pattern: " << pattern
                   << "\n"
                   << " and lemma cube: " << lcube << "\n";);

        for (const lemma_ref &l : neighbours) {
            SASSERT(cluster->can_contain(l));
            cluster->add_lemma(l, false);
            TRACE("cluster_stats",
                  tout << "Added lemma " << mk_and(l->get_cube()) << "\n";);
        }

        // finally add the lemma and do subsumption check
        cluster->add_lemma(lemma, true);
        SASSERT(cluster->get_size() >= 1);
    }
}

void lemma_cluster_finder::collect_statistics(statistics &st) const {
    st.update("time.spacer.solve.reach.cluster", m_st.watch.get_seconds());
}

void lemma_cluster ::rm_subsumed(lemma_info_vector &removed_lemmas) {
    removed_lemmas.reset();
    if (m_lemma_vec.size() <= 1) return;
    tactic_ref simplifier = mk_unit_subsumption_tactic(m);
    goal_ref g(alloc(goal, m, false, false, false));

    goal_ref_buffer result;
    for (auto l : m_lemma_vec) { g->assert_expr(l.get_lemma()->get_expr()); }
    (*simplifier)(g, result);
    SASSERT(result.size() == 1);
    goal *r = result[0];
    if (r->size() == m_lemma_vec.size()) return;
    lemma_info_vector non_subsumd_lemmas;
    for (auto l : m_lemma_vec) {
        bool found = false;
        for (unsigned i = 0; i < r->size(); i++) {
            if (l.get_lemma()->get_expr() == r->form(i)) {
                found = true;
                non_subsumd_lemmas.push_back(l);
                TRACE("cluster_stats_verb", tout << "Keeping lemma "
                                                 << l.get_lemma()->get_cube()
                                                 << "\n";);
                break;
            }
        }
        if (!found) {
            TRACE("cluster_stats_verb", tout << "Removing subsumed lemma "
                                             << l.get_lemma()->get_cube()
                                             << "\n";);
            removed_lemmas.push_back(l);
        }
    }
    m_lemma_vec.reset();
    m_lemma_vec.append(non_subsumd_lemmas);
}

bool lemma_cluster ::match(const expr_ref &e, substitution &sub) {
    m_matcher.reset();
    bool pos;
    bool is_match = m_matcher(m_pattern.get(), e.get(), sub, pos);
    unsigned n_binds = sub.get_num_bindings();
    std::pair<unsigned, unsigned> var;
    expr_offset r;
    arith_util a_util(m);
    if (!(is_match && pos)) return false;
    // All the matches should be numerals
    for (unsigned i = 0; i < n_binds; i++) {
        sub.get_binding(i, var, r);
        if (!a_util.is_numeral(r.get_expr())) return false;
    }
    return true;
}

// Repetition of lemmas is avoided by doing a linear scan over the lemmas in the
// cluster.
bool lemma_cluster ::add_lemma(const lemma_ref &lemma, bool subs_check) {
    substitution sub(m);
    sub.reserve(1, get_num_vars(m_pattern.get()));
    expr_ref cube(m);
    cube = mk_and(lemma->get_cube());
    normalize_order(cube, cube);
    if (!match(cube, sub)) return false;
    // cluster already contains the lemma
    if (contains(lemma)) return false;
    TRACE("cluster_stats_verb",
          tout << "Trying to add lemma " << lemma->get_cube() << "\n";);
    lemma_info l_i(lemma, sub);
    m_lemma_vec.push_back(l_i);
    if (subs_check) {
        lemma_info_vector removed_lemmas;
        rm_subsumed(removed_lemmas);
        for (auto r_l : removed_lemmas) {
            // There is going to atmost subsumed lemma that matches l_i
            if (r_l.get_lemma() == l_i.get_lemma()) return false;
        }
    }
    TRACE("cluster_stats", tout << "Added lemma " << lemma->get_cube()
                                << " to  existing cluster " << m_pattern
                                << "\n";);
    return true;
}

} // namespace spacer
