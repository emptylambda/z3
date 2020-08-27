#pragma once
/*++
Copyright (c) 2020 Arie Gurfinkel

Module Name:

  spacer_global_generalizer.h

Abstract:

  Global Guidance for Spacer

Author:

  Hari Govind V K
  Arie Gurfinkel


--*/

#include "muz/spacer/spacer_context.h"
#include "muz/spacer/spacer_convex_closure.h"

namespace spacer {

class lemma_global_generalizer : public lemma_generalizer {
    struct stats {
        unsigned m_num_cls_ofg;
        unsigned m_num_syn_cls;
        unsigned m_num_mbp_failed;
        unsigned m_num_non_lin;
        unsigned m_num_no_ovr_approx;
        unsigned m_num_cant_abs;

        stopwatch watch;
        stats() { reset(); }
        void reset() {
            watch.reset();
            m_num_cls_ofg = 0;
            m_num_non_lin = 0;
            m_num_syn_cls = 0;
            m_num_mbp_failed = 0;
            m_num_no_ovr_approx = 0;
            m_num_cant_abs = 0;
        }
    };
    stats m_st;

    ast_manager &m;
    arith_util m_arith;
    array_util m_array;
    bv_util m_bv;

    // convex closure interface
    convex_closure m_cvx_cls;
    // save fresh constants for mbp
    app_ref_vector m_dim_frsh_cnsts;
    // save vars from cluster pattern
    var_ref_vector m_dim_vars;

    // solver to get model for computing mbp and to check whether
    // cvx_cls  ==> mbp
    ref<solver> m_solver;

    /// Prepare internal state for computing subsumption
    void setup_subsume(const lemma_cluster &lc);

    /// Returns false if subsumption is not supported for \p lc
    bool is_handled(const lemma_cluster &lc);

    /// Compute a cube \p res such that \neg p subsumes all the lemmas in \p lc
    /// \p cnsts is a set of constants that can be used to make \p res ground
    bool subsume(const lemma_cluster& lc, expr_ref_vector &res, app_ref_vector &cnsts);

    /// Skolemize m_dim_frsh_cnsts in \p f
    ///
    /// The newly created skolem constants are added to \p cnsts
    void skolemize(expr_ref &f, app_ref_vector &cnsts);

    /// Decide global guidance based on lemma
    void core(lemma_ref &lemma);

    /// Create new vars to compute convex cls
    void add_dim_vars(const lemma_cluster &lc);

    /// Coerce LIA constants in \p m_dim_frsh_cnsts to LRA constants
    void to_real_cnsts();

    /// Populate \p m_cvx_cls
    ///
    /// 1. Collect all substitutions in the cluster \p lc
    /// 2. Convert all substitutions to integer numerals
    void populate_cvx_cls(const lemma_cluster &lc);

    void reset(unsigned n_vars);

    /// Attempt to set a conjecture on \p pob n by dropping literal \p lit from
    /// its post \p lvl is level for conjecture pob \p gas is the gas for the
    /// conjecture pob returns true if conjecture is set
    bool do_conjecture(pob_ref n, expr_ref lit, unsigned lvl, unsigned gas);

    /// Replace bound vars in \p fml with uninterpreted constants
    /// AG: isn't this just ground?
    void var_to_const(expr *fml, expr_ref &rw_fml);

    /// Weaken a such that b ==> (and a)
    bool over_approximate(expr_ref_vector &a, const expr_ref b);

    ///\p a is a hard constraint and \p b is a soft constraint that have to be satisfied by mdl
    bool maxsat_with_model(const expr_ref a, const expr_ref b, model_ref &mdl);

  public:
    lemma_global_generalizer(context &ctx);
    ~lemma_global_generalizer() override {}
    void operator()(lemma_ref &lemma) override;
    void collect_statistics(statistics &st) const override;
    void reset_statistics() override { m_st.reset(); }
};
} // namespace spacer
