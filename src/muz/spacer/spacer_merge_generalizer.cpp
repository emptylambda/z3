/*

  Suite of merging strategies.

*/
#include "ast/arith_decl_plugin.h"
#include "ast/ast_util.h"
#include "ast/for_each_expr.h"
#include "ast/rewriter/expr_safe_replace.h"
#include "muz/spacer/spacer_context.h"
#include "muz/spacer/spacer_generalizers.h"
#include "muz/spacer/spacer_manager.h"
#include "muz/spacer/spacer_matrix.h"
#include "muz/spacer/spacer_util.h"
#include "smt/smt_solver.h"

namespace {
struct compute_lcm {
    ast_manager &m;
    arith_util m_arith;
    rational m_val;
    compute_lcm(ast_manager &a_m) : m(a_m), m_arith(m), m_val(1) {}
    void operator()(expr *n) const {}
    void operator()(app *n) {
        rational val;
        if (m_arith.is_numeral(n, val)) {
            m_val = lcm(denominator(abs(val)), m_val);
        }
    }
};

} // namespace
namespace spacer {
lemma_merge_generalizer::lemma_merge_generalizer(context &ctx)
    : lemma_generalizer(ctx), m(ctx.get_ast_manager()), m_arith(m),
      m_cvx_cls(m, ctx.use_sage()), m_dim_frsh_cnsts(m), m_dim_vars(m) {
    m_consts.push_back(rational::one());
    m_consts.push_back(rational::zero());
    m_consts.push_back(rational::minus_one());
    m_consts.push_back(rational(100));
    m_consts.push_back(rational(49));
    m_consts.push_back(rational(102));
    m_consts.push_back(rational(103));
    m_consts.push_back(rational(104));
    m_consts.push_back(rational(51));
    m_consts.push_back(rational(52));
    m_consts.push_back(rational(53));
}

void lemma_merge_generalizer::operator()(lemma_ref &lemma) {
    scoped_watch _w_(m_st.watch);

    if (core(lemma)) {
        TRACE("merge_dbg", tout << "Lemma cube after merge generalization: "
                                << lemma->get_cube() << "\n";);
        if (!lemma->get_pob()->widen()) return;
        // try expanding cvx bounds
        expr_ref_vector conj = lemma->get_cube();
        expr_ref_vector expand_expr(m), updt_conj(conj);
        expr *num, *term;
        expr_ref nw_bnd(m);
        for (auto *bnd : conj) {
            if ((m_arith.is_le(bnd, term, num) ||
                 m_arith.is_ge(bnd, term, num)) &&
                m_arith.is_numeral(num) && is_uninterp(term)) {
                TRACE("merge_dbg_verb",
                      tout << "bnd is " << mk_pp(bnd, m) << "\n";);
                expand_expr.reset();
                for (expr *t : updt_conj)
                    if (t != bnd) expand_expr.push_back(t);
                if (apply_widen(lemma, bnd, expand_expr, nw_bnd)) {
                    updt_conj.erase(bnd);
                    updt_conj.push_back(nw_bnd);
                }
            }
        }
        lemma->get_pob()->stop_widening();
    }
}

void lemma_merge_generalizer::to_real(expr_ref &fml) {
    if (m_arith.is_numeral(fml)) return;
    if (is_uninterp_const(fml) && m_arith.is_int(fml)) {
        fml = m_arith.mk_to_real(fml);
        return;
    }
    if (m_arith.is_arith_expr(fml)) {
        app *fml_app = to_app(fml);
        unsigned N = fml_app->get_num_args();
        expr_ref_vector nw_args(m);
        expr_ref chld(m);
        for (unsigned i = 0; i < N; i++) {
            chld = fml_app->get_arg(i);
            to_real(chld);
            nw_args.push_back(chld);
        }
        fml = m.mk_app(fml_app->get_family_id(), fml_app->get_decl_kind(),
                       nw_args.size(), nw_args.c_ptr());
    }
}

rational lemma_merge_generalizer::get_lcm(expr *e) {
    compute_lcm g(m);
    for_each_expr(g, e);
    TRACE("merge_dbg_verb",
          tout << "lcm of " << mk_pp(e, m) << " is " << g.m_val << "\n";);
    return g.m_val;
}

void lemma_merge_generalizer::mul_and_simp(expr_ref &fml, rational num) {
    SASSERT(m_arith.is_arith_expr(fml));
    SASSERT(num.is_pos());
    if (num.is_one()) return;

    TRACE("merge_dbg_verb",
          tout << "mul and simp called with " << mk_pp(fml, m) << "\n";);
    if (is_uninterp_const(fml)) {
        fml = m_arith.mk_mul(m_arith.mk_int(num), fml);
        TRACE("merge_dbg_verb",
              tout << "simplified to " << mk_pp(fml, m) << "\n";);
        return;
    }
    rational val;
    if (m_arith.is_numeral(fml, val)) {
        val = val * num;
        fml = m_arith.mk_int(val);
        return;
    }
    app *fml_app = to_app(fml);
    unsigned N = fml_app->get_num_args();
    expr_ref_vector nw_args(m);
    for (unsigned i = 0; i < N; i++) {
        expr *chld = fml_app->get_arg(i);
        if (m_arith.is_mul(chld)) {
            expr_ref numeral(to_app(chld)->get_arg(0), m);
            rational val;
            SASSERT(m_arith.is_numeral(numeral));
            m_arith.is_numeral(numeral, val);
            rational nw_coeff = val * num;
            numeral = m_arith.mk_int(nw_coeff);
            nw_args.push_back(
                m_arith.mk_mul(numeral, to_app(chld)->get_arg(1)));
        } else {
            nw_args.push_back(m_arith.mk_mul(m_arith.mk_int(num), chld));
        }
    }
    fml = m.mk_app(fml_app->get_family_id(), fml_app->get_decl_kind(),
                   nw_args.size(), nw_args.c_ptr());
    TRACE("merge_dbg_verb", tout << "simplified to " << mk_pp(fml, m) << "\n";);
}

void lemma_merge_generalizer::to_int(expr_ref &fml) {
    TRACE("merge_dbg_verb", tout << "to int " << mk_pp(fml, m) << "\n";);
    if (m_arith.is_to_real(fml)) {
        fml = to_app(fml)->get_arg(0);
        TRACE("merge_dbg_verb",
              tout << "to int finished " << mk_pp(fml, m) << "\n";);
        return;
    }

    SASSERT((!is_uninterp_const(fml)) || m_arith.is_int(fml));

    rational val;
    if (m_arith.is_numeral(fml, val)) {
        // If its not an integer, try constructing int from it
        fml = m_arith.mk_int(val);

        TRACE("merge_dbg_verb",
              tout << "to int finished " << mk_pp(fml, m) << "\n";);
        return;
    }
    SASSERT(m_arith.is_arith_expr(fml));
    app *fml_app = to_app(fml);
    unsigned N = fml_app->get_num_args();
    expr_ref_vector nw_args(m);
    for (unsigned i = 0; i < N; i++) {
        expr_ref chld(fml_app->get_arg(i), m);
        to_int(chld);
        nw_args.push_back(chld);
    }
    fml = m.mk_app(fml_app->get_family_id(), fml_app->get_decl_kind(),
                   nw_args.size(), nw_args.c_ptr());

    TRACE("merge_dbg_verb",
          tout << "to int finished " << mk_pp(fml, m) << "\n";);
}

void lemma_merge_generalizer::normalize(expr_ref &fml) {
    expr_ref_vector fml_vec(m), rw_fml(m);
    flatten_and(fml.get(), fml_vec);
    expr *s, *t;
    for (expr *e : fml_vec) {
        if (!(m_arith.is_arith_expr(e) || m.is_eq(e))) continue;
        app *e_app = to_app(e);
        SASSERT(e_app->get_num_args() == 2);
        expr_ref lhs(e_app->get_arg(0), m);
        expr_ref rhs(e_app->get_arg(1), m);
        // handle mod
        if (m_arith.is_mod(lhs, s, t)) {
            rational val;
            bool is_int = false;
            // if e is mod, it should already be in linear integer arithmetic
            if (!(m_arith.is_numeral(t, val, is_int) && is_int &&
                  get_lcm(s) == rational::one()))
                NOT_IMPLEMENTED_YET();
            // mod cannot be equal to a non-integer
            SASSERT(m_arith.is_numeral(rhs, val, is_int) && is_int);
            // since e is already in linear integer arithmetic, it is already
            // normalized
            rw_fml.push_back(e);
            continue;
        }

        // make sure that no child is a mod expression
        SASSERT(!has_mode(lhs));
        SASSERT(!has_mode(rhs));
        rational lcm = get_lcm(e);
        SASSERT(lcm != rational::zero());
        if (lcm != 1) {
            mul_and_simp(lhs, lcm);
            mul_and_simp(rhs, lcm);
            TRACE("merge_dbg_verb", tout << "mul and simp reduced lhs to "
                                         << mk_pp(lhs, m) << " and rhs to "
                                         << mk_pp(rhs, m) << "\n";);
        }
        to_int(lhs);
        to_int(rhs);
        app *norm_e =
            m.mk_app(e_app->get_family_id(), e_app->get_decl_kind(), lhs, rhs);
        rw_fml.push_back(to_expr(norm_e));
    }
    fml = mk_and(rw_fml);
}
void lemma_merge_generalizer::to_real(const expr_ref_vector &fml,
                                      expr_ref &nw_fml) {
    expr_ref lhs(m), rhs(m);
    expr_ref_vector rw_fml(m);
    for (auto &e : fml) {
        if (!(m.is_eq(e) || m_arith.is_arith_expr(e))) continue;
        app *e_app = to_app(e);
        SASSERT(to_app(e)->get_num_args() == 2);
        lhs = e_app->get_arg(0);
        rhs = e_app->get_arg(1);
        to_real(rhs);
        to_real(lhs);
        rw_fml.push_back(to_expr(m.mk_app(e_app->get_family_id(),
                                          e_app->get_decl_kind(), lhs, rhs)));
    }
    nw_fml = mk_and(rw_fml);
}

void lemma_merge_generalizer::add_dim_vars(const lemma_cluster &lc) {
    const expr_ref &pattern(lc.get_pattern());
    expr_offset r;
    std::pair<unsigned, unsigned> v;
    unsigned n_vars = get_num_vars(pattern);
    // temporary pointer to an existing expr
    expr_ref var(m);
    const lemma_info_vector &lemmas(lc.get_lemmas());
    const substitution &t_sub(lemmas[0].get_sub());
    for (unsigned j = 0; j < n_vars; j++) {
        // get var id
        t_sub.get_binding(j, v, r);
        // get variable
        var = m.mk_var(v.first, m_arith.mk_int());
        m_cvx_cls.set_dimension(j, var);
        m_dim_vars[j] = var;
        app_ref var_app(m);
        var_app = m.mk_fresh_const("mrg_cvx", m_arith.mk_int());
        // TODO: do we need two variables for a <= x <= b ?
        m_dim_frsh_cnsts[j] = var_app;
    }
}

void lemma_merge_generalizer::add_points(const lemma_cluster &lc) {
    vector<rational> point;
    unsigned n_vars = get_num_vars(lc.get_pattern());
    const lemma_info_vector &lemmas(lc.get_lemmas());
    expr_offset r;
    std::pair<unsigned, unsigned> v;
    for (const lemma_info &lemma : lemmas) {
        const substitution &sub(lemma.get_sub());
        point.reset();
        for (unsigned j = 0; j < n_vars; j++) {
            sub.get_binding(j, v, r);
            rational coeff;
            bool is_int = false;
            m_arith.is_numeral(r.get_expr(), coeff, is_int);
            SASSERT(is_int);
            point.push_back(coeff);
        }
        m_cvx_cls.push_back(point);
    }
}
void lemma_merge_generalizer::reset(unsigned n_vars) {
    // start convex closure computation
    m_cvx_cls.reset(n_vars);
    m_dim_vars.reset();
    m_dim_frsh_cnsts.reset();
    m_dim_frsh_cnsts.reserve(n_vars);
    m_dim_vars.reserve(n_vars);
    m_exact = true;
}

bool lemma_merge_generalizer::core(lemma_ref &lemma) {
    lemma_cluster *pt_cls = (&*lemma->get_pob())->pt().clstr_match(lemma);
    if (pt_cls == nullptr) return false;
    lemma_cluster lc(*pt_cls);

    lc.add_lemma(lemma, true);

    const expr_ref &pattern(lc.get_pattern());

    TRACE("merge_dbg",
          tout << "Start merging with lemma cube: " << lemma->get_cube()
               << "\n Discovered pattern: " << pattern << "\n";);

    if (has_nonlinear_var_mul(pattern, m)) {
        TRACE("merge_dbg",
              tout << "Found non linear pattern. Marked to split \n";);
        lemma->get_pob()->set_split_pat(pattern);
        lemma->get_pob()->set_split();
        return false;
    }

    // if subsumption removed all the other lemmas, there is nothing to
    // generalize
    if (lc.get_size() < 2) return false;

    unsigned n_vars = get_num_vars(pattern);
    SASSERT(n_vars > 0);
    reset(n_vars);
    // create and add dim vars
    add_dim_vars(lc);
    // add points
    add_points(lc);

    expr_ref_vector cls(m);
    m_exact = m_cvx_cls.closure(cls);
    CTRACE("merge_dbg_verb", !m_exact,
           tout << "Convex closure introduced new variables. Closure is"
                << mk_and(cls) << "\n";);

    if (!m_exact) {
        // Add the new variables to the list of variables to be eliminated
        const var_ref_vector &vars = m_cvx_cls.get_nw_vars();
        app_ref var_app(m);
        for (auto v : vars) {
            m_dim_vars.push_back(to_expr(v));
            var_app = m.mk_fresh_const("mrg_syn_cvx", m_arith.mk_real());
            m_dim_frsh_cnsts.push_back(var_app);
        }
    }

    cls.push_back(pattern.get());
    expr_ref cvx_pattern(m);
    var_to_const(mk_and(cls), cvx_pattern);

    model_ref mdl;

    // get a model for the lemma
    // TODO: replace with pob's model
    ref<solver> sol = mk_smt_solver(m, params_ref::get_empty(), symbol::null);
    expr_ref_vector pat(m);
    pat.push_back(cvx_pattern);
    sol->assert_expr(pat);
    lbool res = sol->check_sat(0, nullptr);
    VERIFY(res == l_true);
    sol->get_model(mdl);
    SASSERT(mdl.get() != nullptr);
    TRACE("merge_dbg_verb",
          tout << "calling mbp with " << cvx_pattern << "\n";);
    qe_project(m, m_dim_frsh_cnsts, cvx_pattern, *mdl.get(), true, true, true);
    TRACE("merge_dbg_verb", tout << "Pattern after mbp of computing cvx cls: "
                                 << cvx_pattern << "\n";);
    if (m_dim_frsh_cnsts.size() > 0) {
        TRACE("merge_dbg", tout << "could not eliminate all vars\n";);
        return false;
    }

    if (!m_exact) { normalize(cvx_pattern); }
    // check whether mbp over approximates cnx_cls
    // If not, remove literals from mbp till mbp overapproximates cnx_cls
    expr_ref_vector neg_mbp(m);
    pat.reset();
    flatten_and(cvx_pattern, pat);
    for (expr *e : pat) { neg_mbp.push_back(mk_not(m, e)); }
    expr_ref_vector asmpts(m);
    while (neg_mbp.size() > 0) {
        asmpts.reset();
        expr_ref asmpt(mk_or(neg_mbp), m);
        asmpts.push_back(asmpt);
        TRACE("merge_dbg_verb", tout << "checking neg mbp: " << asmpt << "\n";);
        res = sol->check_sat(1, asmpts.c_ptr());
        if (res == l_false) { return check_inductive_and_update(lemma, pat); }
        // remove all literals that are true in the model
        model_ref rslt;
        sol->get_model(rslt);
        expr_ref rslt_val(m);
        for (unsigned i = 0; i < neg_mbp.size(); i++) {
            if (rslt->is_true(neg_mbp.get(i))) {
                neg_mbp.erase(i);
                pat.erase(i);
                i--;
            }
        }
    }
    // could not find an over approximation
    TRACE("merge_dbg", tout << "mbp could not overapproximate cnx_cls\n";);
    return false;
}

void lemma_merge_generalizer::var_to_const(expr *pattern,
                                           expr_ref &rw_pattern) {
    expr_safe_replace s(m);
    obj_map<expr, expr *> sub;
    for (unsigned i = 0; i < m_dim_vars.size(); i++) {
        s.insert(m_dim_vars[i].get(), to_expr(m_dim_frsh_cnsts[i].get()));
    }
    s(pattern, rw_pattern);
    TRACE("merge_dbg_verb", tout << "Rewrote all vars into u_consts "
                                 << mk_pp(pattern, m) << " into " << rw_pattern
                                 << "\n";);

    expr_ref_vector nw_pattern(m);
    flatten_and(rw_pattern, nw_pattern);

    if (m_exact) {
        TRACE("merge_dbg_verb", tout << "Rewrote " << mk_pp(pattern, m)
                                     << " into " << rw_pattern << "\n";);
        return;
    }

    to_real(nw_pattern, rw_pattern);
    TRACE("merge_dbg_verb", tout << "To real produced " << rw_pattern << "\n";);
    for (unsigned i = 0; i < m_dim_vars.size(); i++) {
        if (m_arith.is_real(m_dim_frsh_cnsts[i].get())) continue;
        app_ref var_app(m);
        var_app = m_arith.mk_to_real(m_dim_frsh_cnsts[i].get());
        m_dim_frsh_cnsts[i] = var_app;
    }
    TRACE("merge_dbg_verb", tout << "Rewrote " << mk_pp(pattern, m) << " into "
                                 << rw_pattern << "\n";);
}

/* core lemma update function*/
bool lemma_merge_generalizer::check_inductive_and_update(
    lemma_ref &lemma, expr_ref_vector &conj) {
    TRACE("merge_dbg", tout << "Attempt to update lemma with: " << conj << "\n"
                            << "at level " << lemma->level() << "\n";);
    pred_transformer &pt = lemma->get_pob()->pt();
    pob_ref pob = lemma->get_pob();
    unsigned uses_level = 0;
    if (pt.check_inductive(infty_level(), conj, uses_level,
                           lemma->weakness()) ||
        pt.check_inductive(lemma->level(), conj, uses_level,
                           lemma->weakness())) {
        TRACE("merge_dbg", tout << "POB blocked using merge at level "
                                << uses_level << "\n";);
        lemma->update_cube(lemma->get_pob(), conj);
        lemma->set_level(uses_level);
        return true;
    }

    if (pob->get_merge_atmpts() > 1) {
        pob->set_merge_conj(conj);
        pob->set_refine();
        TRACE("merge_dbg", tout << "merge conjecture  " << mk_and(conj)
                                << " set on pob " << mk_pp(pob->post(), m)
                                << "\n";);
    }
    // keep track of failed merge attempts
    pob->bump_merge_atmpts();
    return false;
}
void lemma_merge_generalizer::collect_statistics(statistics &st) const {
    st.update("time.spacer.solve.reach.gen.merge", m_st.watch.get_seconds());
    st.update("time.spacer.solve.reach.gen.wide", m_st.watch.get_seconds());
    st.update("SPACER wide attmpts", m_st.wide_atmpts);
    st.update("SPACER wide success", m_st.wide_sucess);
    m_cvx_cls.collect_statistics(st);
}

bool lemma_merge_generalizer::should_apply(const expr *lit, rational val,
                                           rational n) {
    // the only case in which negation and non negation agree
    if (val == n) return false;

    // negation is the actual negation modulo val == n
    expr *neg_lit;
    if (m.is_not(lit, neg_lit)) { return !should_apply(neg_lit, val, n); }

    SASSERT(val != n);
    if (m.is_eq(lit)) return true;
    switch (to_app(lit)->get_decl_kind()) {
    case OP_LE:
        return n > val;
    case OP_LT:
        return n > val;
    case OP_GT:
        return n < val;
    case OP_GE:
        return n < val;
    default:
        return false;
    }
}

void lemma_merge_generalizer::substitute(expr *var, rational n, expr *fml,
                                         expr_ref &sub) {
    expr_safe_replace s(m);
    sub.reset();
    s.insert(var, m_arith.mk_int(n));
    expr_ref f(fml, m);
    s(f, sub);
}

bool lemma_merge_generalizer::apply_widen(lemma_ref &lemma, expr *lit,
                                          expr_ref_vector &conj,
                                          expr_ref &nw_bnd) {
    SASSERT(!conj.contains(lit));
    TRACE("merge_dbg", tout << "Applying widening on " << conj
                            << " with literal " << mk_pp(lit, m) << "\n";);
    SASSERT(to_app(lit)->get_num_args() == 2);
    expr *num = to_app(lit)->get_arg(1);
    rational val;
    bool is_int = false;
    SASSERT(m_arith.is_numeral(num));
    m_arith.is_numeral(num, val, is_int);
    expr_ref n_lit(m);
    if (!is_int) return false;
    bool success = false;
    for (rational n : m_consts) {
        if (should_apply(lit, val, n)) {
            m_st.wide_atmpts++;
            substitute(num, n, lit, n_lit);
            conj.push_back(n_lit);
            unsigned uses_level = 0;
            TRACE("merge_dbg_verb",
                  tout << "Attempting to update lemma with " << conj << "\n";);
            bool is_ind = (lemma->get_pob())
                              ->pt()
                              .check_inductive(lemma->level(), conj, uses_level,
                                               -lemma->weakness());

            if (is_ind) {
                m_st.wide_sucess++;
                lemma->update_cube(lemma->get_pob(), conj);
                lemma->set_level(uses_level);
                val = n;
                TRACE("merge_dbg",
                      tout << "widening succeeded with " << n << "\n";);
                success = true;
                nw_bnd = n_lit;
            }
            conj.pop_back();
        }
    }
    return success;
}
} // namespace spacer
