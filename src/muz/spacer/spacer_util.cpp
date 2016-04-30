/**
Copyright (c) 2011 Microsoft Corporation

Module Name:

    spacer_util.cpp

Abstract:

    Utility functions for SPACER.

Author:

    Krystof Hoder (t-khoder) 2011-8-19.

Revision History:

    Modified by Anvesh Komuravelli

Notes: 
    

--*/

#include <sstream>
#include "arith_simplifier_plugin.h"
#include "array_decl_plugin.h"
#include "ast_pp.h"
#include "basic_simplifier_plugin.h"
#include "bv_simplifier_plugin.h"
#include "bool_rewriter.h"
#include "dl_util.h"
#include "for_each_expr.h"
#include "smt_params.h"
#include "model.h"
#include "model_evaluator.h"
#include "ref_vector.h"
#include "rewriter.h"
#include "rewriter_def.h"
#include "util.h"
#include "spacer_manager.h"
#include "spacer_util.h"
#include "arith_decl_plugin.h"
#include "expr_replacer.h"
#include "model_smt2_pp.h"
#include "scoped_proof.h"
#include "qe_lite.h"
#include "qe_project.h"
#include "model_pp.h"
#include "expr_safe_replace.h"

#include "datatype_decl_plugin.h"
#include "bv_decl_plugin.h"

#include "old_mev.h"
#include "qe_mbp.h"

/**
   \brief replaces arithmetic disequalities with a strict inequality true in the model
*/
static void reduce_arith_disequalities (spacer::model_evaluator_util &mev,
                                        expr_ref_vector &fml) {
    expr *e, *ne, *e1, *e2;
    
    ast_manager &m = fml.get_manager ();
    arith_util arith(m);
    expr_ref v1(m), v2(m);
    rational n1, n2;
    for (unsigned i = 0, sz = fml.size (); i < sz; ++i) {
        e = fml.get (i);
        if (m.is_not(e, ne) && m.is_eq(ne, e1, e2) && arith.is_int_real(e1) &&
            mev.eval(e1, v1, true) && arith.is_numeral(v1, n1) &&
            mev.eval(e2, v2, true) && arith.is_numeral(v2, n2)) {
            if (n1 < n2) fml[i] = arith.mk_lt(e1, e2);
            else if (n1 > n2) fml[i] = arith.mk_lt(e2, e1);
            else {
                fml.reset();
                fml.push_back(m.mk_false ());
                return;
            }
        }
    }
}

namespace spacer {

    /////////////////////////
    // model_evaluator_util
    //
    
    model_evaluator_util::model_evaluator_util(ast_manager& m) : 
        m(m), m_mev(NULL) 
    { reset (NULL); }

    model_evaluator_util::~model_evaluator_util() {reset (NULL);}
    
    
    void model_evaluator_util::reset (model* model) {
        if (m_mev) {
            dealloc(m_mev);
            m_mev = NULL;
        }
        m_model = model;
        if (!m_model) return;
        m_mev = alloc(model_evaluator, *m_model);
    }
  
    bool model_evaluator_util::eval (expr *e, expr_ref &result, bool model_completion) {
        m_mev->set_model_completion (model_completion);
        try {
            m_mev->operator() (e, result);
            return true;
        } catch (model_evaluator_exception &ex) {
            (void)ex;
            TRACE("spacer_model_evaluator", tout << ex.msg () << "\n";);
            return false;
        }
    }
    
    bool model_evaluator_util::eval_as_and(const expr_ref_vector &v, bool complete) {
        expr_ref e(m);
        expr_ref res(m);
        e = mk_and (v);
        return eval(e, res, complete);
    }
  
    bool model_evaluator_util::is_false (expr *x) {
        expr_ref res(m);
        return eval(x, res, false) && m.is_false (res);
    }
    bool model_evaluator_util::is_true (expr *x) {
        expr_ref res(m);
        return eval(x, res, false) && m.is_true (res);
    }
  
  
    void reduce_disequalities(model& model, unsigned threshold, expr_ref& fml) {
        ast_manager& m = fml.get_manager();
        expr_ref_vector conjs(m);
        flatten_and(fml, conjs);
        obj_map<expr, unsigned> diseqs;
        expr* n, *lhs, *rhs;
        for (unsigned i = 0; i < conjs.size(); ++i) {
            if (m.is_not(conjs[i].get(), n) &&
                m.is_eq(n, lhs, rhs)) {
                if (!m.is_value(rhs)) {
                    std::swap(lhs, rhs);
                }
                if (!m.is_value(rhs)) {
                    continue;
                }
                diseqs.insert_if_not_there2(lhs, 0)->get_data().m_value++;
            }
        }
        expr_substitution sub(m);

        unsigned orig_size = conjs.size();
        unsigned num_deleted = 0;
        expr_ref val(m), tmp(m);
        proof_ref pr(m);
        pr = m.mk_asserted(m.mk_true());
        obj_map<expr, unsigned>::iterator it  = diseqs.begin();
        obj_map<expr, unsigned>::iterator end = diseqs.end();
        for (; it != end; ++it) {
            if (it->m_value >= threshold) {
                model.eval(it->m_key, val);
                sub.insert(it->m_key, val, pr);
                conjs.push_back(m.mk_eq(it->m_key, val));
                num_deleted += it->m_value;
            }
        }
        if (orig_size < conjs.size()) {
            scoped_ptr<expr_replacer> rep = mk_expr_simp_replacer(m);
            rep->set_substitution(&sub);
            for (unsigned i = 0; i < orig_size; ++i) {
                tmp = conjs[i].get();
                (*rep)(tmp);
                if (m.is_true(tmp)) {
                    conjs[i] = conjs.back();
                    SASSERT(orig_size <= conjs.size());
                    conjs.pop_back();
                    SASSERT(orig_size <= 1 + conjs.size());
                    if (i + 1 == orig_size) {
                        // no-op.
                    }
                    else if (orig_size <= conjs.size()) {
                        // no-op
                    }
                    else {
                        SASSERT(orig_size == 1 + conjs.size());
                        --orig_size;
                        --i;
                    }
                }
                else {
                    conjs[i] = tmp;
                }
            }            
            IF_VERBOSE(2, verbose_stream() << "Deleted " << num_deleted << " disequalities " << conjs.size() << " conjuncts\n";);
        }
        fml = m.mk_and(conjs.size(), conjs.c_ptr());        
    }

    // 
    // (f (if c1 (if c2 e1 e2) e3) b c) -> 
    // (if c1 (if c2 (f e1 b c)

    class ite_hoister {
        ast_manager& m;
    public:
        ite_hoister(ast_manager& m): m(m) {}

        br_status mk_app_core(func_decl* f, unsigned num_args, expr* const* args, expr_ref& result) {
            if (m.is_ite(f)) {
                return BR_FAILED;
            }
            for (unsigned i = 0; i < num_args; ++i) {
                expr* c, *t, *e;
                if (!m.is_bool(args[i]) && m.is_ite(args[i], c, t, e)) {
                    expr_ref e1(m), e2(m);
                    ptr_vector<expr> args1(num_args, args);
                    args1[i] = t;
                    e1 = m.mk_app(f, num_args, args1.c_ptr());
                    if (t == e) {
                        result = e1;
                        return BR_REWRITE1;
                    }
                    args1[i] = e;
                    e2 = m.mk_app(f, num_args, args1.c_ptr());
                    result = m.mk_app(f, num_args, args);
                    result = m.mk_ite(c, e1, e2);
                    return BR_REWRITE3;
                }
            }
            return BR_FAILED;
        }
    };

    struct ite_hoister_cfg: public default_rewriter_cfg {
        ite_hoister m_r;
        bool rewrite_patterns() const { return false; }
        br_status reduce_app(func_decl * f, unsigned num, expr * const * args, expr_ref & result, proof_ref & result_pr) {
            return m_r.mk_app_core(f, num, args, result);
        }
        ite_hoister_cfg(ast_manager & m, params_ref const & p):m_r(m) {}        
    };

    class ite_hoister_star : public rewriter_tpl<ite_hoister_cfg> {
        ite_hoister_cfg m_cfg;
    public:
        ite_hoister_star(ast_manager & m, params_ref const & p):
            rewriter_tpl<ite_hoister_cfg>(m, false, m_cfg),
            m_cfg(m, p) {}
    };

    void hoist_non_bool_if(expr_ref& fml) {
        ast_manager& m = fml.get_manager();
        scoped_no_proof _sp(m);
        params_ref p;
        ite_hoister_star ite_rw(m, p);
        expr_ref tmp(m);
        ite_rw(fml, tmp);
        fml = tmp;        
    }

    class test_diff_logic {
        ast_manager& m;
        arith_util a;
        bv_util    bv;
        bool m_is_dl;
        bool m_test_for_utvpi;

        bool is_numeric(expr* e) const {
            if (a.is_numeral(e)) {
                return true;
            }
            expr* cond, *th, *el;
            if (m.is_ite(e, cond, th, el)) {
                return is_numeric(th) && is_numeric(el);
            }
            return false;
        }
        
        bool is_arith_expr(expr *e) const {
            return is_app(e) && a.get_family_id() == to_app(e)->get_family_id();
        }

        bool is_offset(expr* e) const {
            if (a.is_numeral(e)) {
                return true;
            }
            expr* cond, *th, *el, *e1, *e2;
            if (m.is_ite(e, cond, th, el)) {
                return is_offset(th) && is_offset(el);
            }
            // recognize offsets.
            if (a.is_add(e, e1, e2)) {
                if (is_numeric(e1)) {
                    return is_offset(e2);
                }
                if (is_numeric(e2)) {
                    return is_offset(e1);
                }
                return false;
            }
            if (m_test_for_utvpi) {
                if (a.is_mul(e, e1, e2)) {
                    if (is_minus_one(e1)) {
                        return is_offset(e2);
                    }
                    if (is_minus_one(e2)) {
                        return is_offset(e1);
                    }
                }
            }
            return !is_arith_expr(e);
        }

        bool is_minus_one(expr const * e) const { 
            rational r; return a.is_numeral(e, r) && r.is_minus_one(); 
        }

        bool test_ineq(expr* e) const {
            SASSERT(a.is_le(e) || a.is_ge(e) || m.is_eq(e));
            SASSERT(to_app(e)->get_num_args() == 2);
            expr * lhs = to_app(e)->get_arg(0);
            expr * rhs = to_app(e)->get_arg(1);
            if (is_offset(lhs) && is_offset(rhs)) 
                return true;    
            if (!is_numeric(rhs)) 
                std::swap(lhs, rhs);
            if (!is_numeric(rhs)) 
                return false;    
            // lhs can be 'x' or '(+ x (* -1 y))'
            if (is_offset(lhs))
                return true;
            expr* arg1, *arg2;
            if (!a.is_add(lhs, arg1, arg2)) 
                return false;    
            // x
            if (m_test_for_utvpi) {
                return is_offset(arg1) && is_offset(arg2);
            }
            if (is_arith_expr(arg1)) 
                std::swap(arg1, arg2);
            if (is_arith_expr(arg1))
                return false;
            // arg2: (* -1 y)
            expr* m1, *m2;
            if (!a.is_mul(arg2, m1, m2))
                return false;
            return is_minus_one(m1) && is_offset(m2);
        }

        bool test_eq(expr* e) const {
            expr* lhs, *rhs;
            VERIFY(m.is_eq(e, lhs, rhs));
            if (!a.is_int_real(lhs)) {
                return true;
            }
            if (a.is_numeral(lhs) || a.is_numeral(rhs)) {
                return test_ineq(e);
            }
            return 
                test_term(lhs) && 
                test_term(rhs) &&
                !a.is_mul(lhs) &&
                !a.is_mul(rhs);
        }

        bool test_term(expr* e) const {
            if (m.is_bool(e)) {
                return true;
            }
            if (a.is_numeral(e)) {
                return true;
            }
            if (is_offset(e)) {
                return true;
            }
            expr* lhs, *rhs;
            if (a.is_add(e, lhs, rhs)) {
                if (!a.is_numeral(lhs)) {
                    std::swap(lhs, rhs);
                }
                return a.is_numeral(lhs) && is_offset(rhs);
            }
            if (a.is_mul(e, lhs, rhs)) {
                return is_minus_one(lhs) || is_minus_one(rhs);
            }
            return false;
        }

        bool is_non_arith_or_basic(expr* e) {
            if (!is_app(e)) {
                return false;
            }
            family_id fid = to_app(e)->get_family_id();

            if (fid == null_family_id && 
                !m.is_bool(e) && 
                to_app(e)->get_num_args() > 0) {
                return true;
            }
            return 
                fid != m.get_basic_family_id() &&
                fid != null_family_id &&
                fid != a.get_family_id() &&
                fid != bv.get_family_id();
        }

    public:
        test_diff_logic(ast_manager& m): m(m), a(m), bv(m), m_is_dl(true), m_test_for_utvpi(false) {}
       
        void test_for_utvpi() { m_test_for_utvpi = true; }

        void operator()(expr* e) {
            if (!m_is_dl) {
                return;
            }
            if (a.is_le(e) || a.is_ge(e)) {
                m_is_dl = test_ineq(e);
            }
            else if (m.is_eq(e)) {
                m_is_dl = test_eq(e);
            }
            else if (is_non_arith_or_basic(e)) {
                m_is_dl = false;
            }
            else if (is_app(e)) {
                app* a = to_app(e);                
                for (unsigned i = 0; m_is_dl && i < a->get_num_args(); ++i) {
                    m_is_dl = test_term(a->get_arg(i));
                }
            }

            if (!m_is_dl) {
                char const* msg = "non-diff: ";
                if (m_test_for_utvpi) {
                    msg = "non-utvpi: ";
                }
                IF_VERBOSE(1, verbose_stream() << msg << mk_pp(e, m) << "\n";);
            }
        }

        bool is_dl() const { return m_is_dl; }
    };

    bool is_difference_logic(ast_manager& m, unsigned num_fmls, expr* const* fmls) {
        test_diff_logic test(m);
        expr_fast_mark1 mark;
        for (unsigned i = 0; i < num_fmls; ++i) {
            quick_for_each_expr(test, mark, fmls[i]);
        } 
        return test.is_dl();
    }  

    bool is_utvpi_logic(ast_manager& m, unsigned num_fmls, expr* const* fmls) {
        test_diff_logic test(m);
        test.test_for_utvpi();
        expr_fast_mark1 mark;
        for (unsigned i = 0; i < num_fmls; ++i) {
            quick_for_each_expr(test, mark, fmls[i]);
        } 
        return test.is_dl();
    }  


    void subst_vars (ast_manager& m, app_ref_vector const& vars, 
                     model* M, expr_ref& fml) {
        expr_safe_replace sub (m);
        model_evaluator_util mev (m);
        mev.set_model(*M);
        for (unsigned i = 0; i < vars.size (); i++) {
            app* v = vars.get (i);
            expr_ref val (m);
            VERIFY(mev.eval (v, val, true));
            sub.insert (v, val);
        }
        sub (fml);
    }

    /*
     * eliminate simple equalities using qe_lite
     * then, MBP for Booleans (substitute), reals (based on LW), ints (based on Cooper), and arrays
     */
    void qe_project (ast_manager& m, app_ref_vector& vars, expr_ref& fml, 
                     const model_ref& M, bool reduce_all_selects, bool use_native_mbp) {
        th_rewriter rw (m);
        TRACE ("spacer_mbp",
                tout << "Before projection:\n";
                tout << mk_pp (fml, m) << "\n";
                tout << "Vars:\n";
                for (unsigned i = 0; i < vars.size(); ++i) {
                    tout << mk_pp(vars.get (i), m) << "\n";
                }
              );

        {
          // Ensure that top-level AND of fml is flat
          expr_ref_vector flat(m);
          flatten_and (fml, flat);
          if (flat.size () == 1)
            fml = flat.get (0);
          else if (flat.size () > 1)
            fml = m.mk_and (flat.size (), flat.c_ptr ());
        }
        
        app_ref_vector arith_vars (m);
        app_ref_vector array_vars (m);
        array_util arr_u (m);
        arith_util ari_u (m);
        expr_safe_replace bool_sub (m);
        expr_ref bval (m);

        while (true) {
            qe_lite qe (m);
            qe (vars, fml);
            rw (fml); 

            TRACE ("spacer_mbp",
                    tout << "After qe_lite:\n";
                    tout << mk_pp (fml, m) << "\n";
                    tout << "Vars:\n";
                    for (unsigned i = 0; i < vars.size(); ++i) {
                        tout << mk_pp(vars.get (i), m) << "\n";
                    }
                  );

            bool has_bool_vars = false;

            // sort out vars into bools, arith (int/real), and arrays
            for (unsigned i = 0; i < vars.size (); i++) {
                if (m.is_bool (vars.get (i))) {
                    // obtain the interpretation of the ith var using model completion
                    VERIFY (M->eval (vars.get (i), bval, true));
                    bool_sub.insert (vars.get (i), bval);
                    has_bool_vars = true;
                }
                else if (arr_u.is_array (vars.get (i))) {
                    array_vars.push_back (vars.get (i));
                }
                else {
                    SASSERT (ari_u.is_int (vars.get (i)) || ari_u.is_real (vars.get (i)));
                    arith_vars.push_back (vars.get (i));
                }
            }

            // substitute Booleans
            if (has_bool_vars) {
                bool_sub (fml);
                TRACE ("spacer_mbp",
                        tout << "Projected Booleans:\n" << mk_pp (fml, m) << "\n";
                      );
                bool_sub.reset ();
            }

            TRACE ("spacer_mbp",
                    tout << "Array vars:\n";
                    for (unsigned i = 0; i < array_vars.size (); ++i) {
                        tout << mk_pp (array_vars.get (i), m) << "\n";
                    }
                  );

            vars.reset ();

            // project arrays
            {
                scoped_no_proof _sp (m);
                qe::array_project (*M.get (), array_vars, fml, vars, reduce_all_selects);
                SASSERT (array_vars.empty ());
            }

            TRACE ("spacer_mbp",
                    tout << "extended model:\n";
                    model_pp (tout, *M);
                    tout << "Auxiliary variables of index and value sorts:\n";
                    for (unsigned i = 0; i < vars.size (); i++) {
                        tout << mk_pp (vars.get (i), m) << "\n";
                    }
                  );

            if (vars.empty ()) break;
        }

        // project reals and ints
        if (!arith_vars.empty ()) {
            TRACE ("spacer_mbp",
                    tout << "Arith vars:\n";
                    for (unsigned i = 0; i < arith_vars.size (); ++i) {
                    tout << mk_pp (arith_vars.get (i), m) << "\n";
                    }
                  );
            if (use_native_mbp)
            {
              qe::mbp mbp (m);
              expr_ref_vector fmls(m);
              flatten_and (fml, fmls);
              
              mbp (true, arith_vars, *M.get (), fmls);
              fml = mk_and (fmls);
              SASSERT (arith_vars.empty ());
            }
            else
            {
                scoped_no_proof _sp (m);
                qe::arith_project (*M.get (), arith_vars, fml);
            }

            TRACE ("spacer_mbp",
                    tout << "Projected arith vars:\n" << mk_pp (fml, m) << "\n";
                    tout << "Remaining arith vars:\n";
                    for (unsigned i = 0; i < arith_vars.size (); i++) {
                        tout << mk_pp (arith_vars.get (i), m) << "\n";
                    }
                  );
        }

        // substitute any remaining arith vars
        if (!arith_vars.empty ()) {
            subst_vars (m, arith_vars, M.get(), fml);
            TRACE ("spacer_mbp",
                    tout << "After substituting remaining arith vars:\n";
                    tout << mk_pp (fml, m) << "\n";
                  );
            // an extra round of simplification because subst_vars is not simplifying
            rw(fml);
        }

        DEBUG_CODE (
            model_evaluator_util mev (m);
            expr_ref v(m);
            mev.set_model(*M.get());
            SASSERT (mev.eval (fml, v, false));
            SASSERT (m.is_true (v));
        );

        vars.reset ();
    }
  
  
    static expr* apply_accessor(ast_manager &m,
                                ptr_vector<func_decl> const& acc,
                                unsigned j,
                                func_decl* f,
                                expr* c) {
        if (is_app(c) && to_app(c)->get_decl() == f) {
            return to_app(c)->get_arg(j);
        }
        else {
            return m.mk_app(acc[j], c);
        }
    }

    void expand_literals(ast_manager &m, expr_ref_vector& conjs) {
        if (conjs.empty ()) return;
        arith_util arith(m);
        datatype_util dt(m);
        bv_util       bv(m);
        expr* e1, *e2, *c, *val;
        rational r;
        unsigned bv_size;

        TRACE("spacer_expand", 
                tout << "begin expand\n";
                for (unsigned i = 0; i < conjs.size(); ++i) {
                    tout << mk_pp(conjs[i].get(), m) << "\n";
                });

        for (unsigned i = 0; i < conjs.size(); ++i) {
            expr* e = conjs[i].get();
            if (m.is_eq(e, e1, e2) && arith.is_int_real(e1)) {
                conjs[i] = arith.mk_le(e1,e2);
                if (i+1 == conjs.size()) {
                    conjs.push_back(arith.mk_ge(e1, e2));
                }
                else {
                    conjs.push_back(conjs[i+1].get());
                    conjs[i+1] = arith.mk_ge(e1, e2);
                }
                ++i;
            }
            else if ((m.is_eq(e, c, val) && is_app(val) && dt.is_constructor(to_app(val))) ||
                    (m.is_eq(e, val, c) && is_app(val) && dt.is_constructor(to_app(val)))){
                func_decl* f = to_app(val)->get_decl();
                func_decl* r = dt.get_constructor_recognizer(f);
                conjs[i] = m.mk_app(r, c);
                ptr_vector<func_decl> const& acc = *dt.get_constructor_accessors(f);
                for (unsigned j = 0; j < acc.size(); ++j) {
                    conjs.push_back(m.mk_eq(apply_accessor(m, acc, j, f, c), to_app(val)->get_arg(j)));
                }
            }
            else if ((m.is_eq(e, c, val) && bv.is_numeral(val, r, bv_size)) ||
                    (m.is_eq(e, val, c) && bv.is_numeral(val, r, bv_size))) {
                rational two(2);
                for (unsigned j = 0; j < bv_size; ++j) {
                    parameter p(j);
                    //expr* e = m.mk_app(bv.get_family_id(), OP_BIT2BOOL, 1, &p, 1, &c);
                    expr* e = m.mk_eq(m.mk_app(bv.get_family_id(), OP_BIT1), bv.mk_extract(j, j, c));
                    if ((r % two).is_zero()) {
                        e = m.mk_not(e);
                    }
                    r = div(r, two);
                    if (j == 0) {
                        conjs[i] = e;
                    }
                    else {
                        conjs.push_back(e);
                    }
                }
            }
        }
        TRACE("spacer_expand", 
                tout << "end expand\n";
                for (unsigned i = 0; i < conjs.size(); ++i) {
                    tout << mk_pp(conjs[i].get(), m) << "\n";
                });
    }

  namespace
  {
    class implicant_picker
    {
        model_evaluator_util &m_mev;
        ast_manager &m;
        arith_util m_arith;
      
        expr_ref_vector m_todo;
        expr_mark m_visited;
      
      
        void add_literal (expr *e, expr_ref_vector &out)
        {
            SASSERT (m.is_bool (e));
        
            expr_ref res (m), v(m);
            m_mev.eval (e, v, false);
            SASSERT (m.is_true (v) || m.is_false (v));
        
            res = m.is_false (v) ? m.mk_not (e) : e;
            
            if (m.is_distinct (res)) {
                // -- (distinct a b) == (not (= a b))
                if (to_app(res)->get_num_args() == 2) {
                    res = m.mk_eq (to_app(res)->get_arg(0), to_app(res)->get_arg(1));
                    res = m.mk_not (res);
                }
            }
        
            expr *nres, *f1, *f2;
            if (m.is_not(res, nres)) {
                // -- (not (xor a b)) == (= a b)
                if (m.is_xor(nres, f1, f2))
                    res = m.mk_eq(f1, f2);
            
                // -- split arithmetic inequality
                else if (m.is_eq (nres, f1, f2) && m_arith.is_int_real (f1)) {
                    expr_ref u(m);
                    u = m_arith.mk_lt(f1, f2);
                    if (m_mev.eval (u, v, false) && m.is_true (v))
                        res = u;
                    else
                        res = m_arith.mk_lt(f2,f1);
                }
            }
        
            if (!m_mev.is_true (res))
                verbose_stream() << "Bad literal: " << mk_pp(res, m) << "\n";
            SASSERT (m_mev.is_true (res));
            out.push_back (res);
        }        
        
        void process_app (app *a, expr_ref_vector &out) {
            if (m_visited.is_marked(a)) return;
            SASSERT (m.is_bool (a));
            expr_ref v(m);
            m_mev.eval (a, v, false);
            
            if (!m.is_true(v) && !m.is_false(v)) return;
            
            expr *na, *f1, *f2, *f3;
            
            if (a->get_family_id() != m.get_basic_family_id())
                add_literal(a, out);
            else if (is_uninterp_const(a))
                add_literal(a, out);
            else if (m.is_not(a, na) && m.is_not(na, na))
                m_todo.push_back(na);
            else if (m.is_not(a, na))
                m_todo.push_back(na);
            else if (m.is_distinct(a)) {
                if (m.is_false(v))
                    m_todo.push_back
                        (qe::project_plugin::pick_equality(m, *m_mev.get_model(), a));
                else if (a->get_num_args() == 2)
                    add_literal(a, out);
                else
                    m_todo.push_back(m.mk_distinct_expanded(a->get_num_args(),
                                                            a->get_args()));
            }
            else if (m.is_and(a)) {
                if (m.is_true(v))
                    m_todo.append(a->get_num_args(), a->get_args());
                else if (m.is_false(v)) {
                    for (unsigned i = 0, sz = a->get_num_args (); i < sz; ++i) {
                        if (m_mev.is_false(a->get_arg(i))) {
                            m_todo.push_back(a->get_arg(i));
                            break;
                        }
                    }
                }
            }
            else if (m.is_or(a)) {
                if (m.is_false(v))
                    m_todo.append (a->get_num_args(), a->get_args());
                else if (m.is_true(v)) {
                    for (unsigned i = 0, sz = a->get_num_args(); i < sz; ++i) {
                        if (m_mev.is_true(a->get_arg(i))) {
                            m_todo.push_back(a->get_arg(i));
                            break;
                        }
                    }
                }
            }
            else if (m.is_iff(a, f1, f2) || m.is_eq (a, f1, f2) ||
                     (m.is_true(v) && m.is_not(a, na) && m.is_xor (na, f1, f2))) {
                if (!m.are_equal(f1, f2) && !m.are_distinct(f1, f2)) {
                    if (m.is_bool(f1) &&
                        (!is_uninterp_const(f1) || !is_uninterp_const(f2)))
                        m_todo.append(a->get_num_args(), a->get_args());
                    else
                        add_literal(a, out);
                }
            }
            else if (m.is_ite (a, f1, f2, f3)) {
                if (m.are_equal (f2, f3)) m_todo.push_back (f2);
                else if (m_mev.is_true (f2) && m_mev.is_true (f3)) {
                    m_todo.push_back(f2); m_todo.push_back(f3);
                }
                else if (m_mev.is_false(f2) && m_mev.is_false(f3)) {
                    m_todo.push_back(f2); m_todo.push_back(f3);
                }
                else if (m_mev.is_true(f1)) {
                    m_todo.push_back(f1); m_todo.push_back(f2);
                }
                else if (m_mev.is_false(f1)) {
                    m_todo.push_back(f1); m_todo.push_back(f3);
                }
            }
            else if (m.is_xor(a, f1, f2))
                m_todo.append(a->get_num_args(), a->get_args());
            else if (m.is_implies(a, f1, f2)) {
                if (m.is_true (v)) {
                    if (m_mev.is_true(f2)) m_todo.push_back(f2);
                    else if (m_mev.is_false(f1)) m_todo.push_back(f1);
                } else if (m.is_false(v)) 
                    m_todo.append (a->get_num_args(), a->get_args());
            }
            else
                UNREACHABLE();
        }
        
        void pick_literals (expr *e, expr_ref_vector &out) {
            SASSERT(m_todo.empty());
            if (m_visited.is_marked(e)) return;
            SASSERT(is_app(e));
            
            m_todo.push_back(e);
            do {
                app *a = to_app(m_todo.back());
                m_todo.pop_back();
                process_app(a, out);
                m_visited.mark(a, true);
            } while (!m_todo.empty());
        }
        
        bool pick_implicant (const expr_ref_vector &in, expr_ref_vector &out) {
            m_visited.reset();
            expr_ref e(m);
            e = mk_and (in);
            bool is_true = m_mev.is_true (e);
            
            for (unsigned i = 0, sz = in.size (); i < sz; ++i) {
                if (is_true || m_mev.is_true(in.get(i)))
                    pick_literals(in.get(i), out);
            }
            
            m_visited.reset ();
            return is_true;
        }
        
    public:
        implicant_picker (model_evaluator_util &mev) : 
            m_mev (mev), m (m_mev.get_ast_manager ()), m_arith(m), m_todo(m) {}
      
        void operator() (expr_ref_vector &in, expr_ref_vector& out)
        {pick_implicant (in, out);}
    };
  }
  
  void compute_implicant_literals (model_evaluator_util &mev, expr_ref_vector &formula, 
                                   expr_ref_vector &res)
  {
      // XXX what is the point of flattening?
      flatten_and (formula);
      if (formula.empty ()) return;
    
      implicant_picker ipick (mev);
      ipick (formula, res);
  }
  
}

template class rewriter_tpl<spacer::ite_hoister_cfg>;



