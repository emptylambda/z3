/*

  Suite of merging strategies.

*/
#include "muz/spacer/spacer_context.h"
#include "muz/spacer/spacer_util.h"
#include "muz/spacer/spacer_generalizers.h"
#include "muz/spacer/spacer_manager.h"
#include "ast/arith_decl_plugin.h"
#include "ast/ast_util.h"


using namespace spacer;
namespace spacer{
    lemma_merge_generalizer::lemma_merge_generalizer(context &ctx, int th) :
        lemma_generalizer(ctx), m(ctx.get_ast_manager()), m_arith(m){
        threshold = th;
    }

    void lemma_merge_generalizer::operator()(lemma_ref &lemma){
        expr_ref_vector neighbours = lemma->get_neighbours();
        if(neighbours.size() > 0){
            substitution subs_newLemma(m), subs_oldLemma(m);
            expr_ref cube(m), normalizedCube(m), out(m);
            cube = mk_and(lemma->get_cube());
            normalize_order(cube, normalizedCube);
            TRACE("merge_dbg",
                  tout << "Start merging with lemma cube: " << mk_pp(normalizedCube, m) << "\n"
                  "Discovered pattern: " << mk_pp(neighbours.get(0), m) << "\n"
                  "Neighbours: " << mk_pp(neighbours.get(1), m) << "\n"
                  ;);

            if(monotonic_coeffcient(cube, to_app(neighbours.get(0)), out)){
                TRACE("merge_dbg", tout << "mono coeff found a conjecture...\n"
                      << mk_pp(out, m) << "\n";);
                expr_ref_vector conj(m);
                conj.push_back(out);
                if(check_inductive_and_update(lemma, conj))
                    return;
            }

            if(merge_halfspaces(normalizedCube, to_app(neighbours.get(0)), out)){
                TRACE("merge_dbg", tout << "merge halfplanes found a conjecture...\n"
                      << mk_pp(out, m) << "\n";);
                expr_ref_vector conj(m);
                conj.push_back(out);
                if(check_inductive_and_update(lemma, conj))
                    return;
            }

            if(leq_monotonic_k(normalizedCube, to_app(neighbours.get(0)), out)){
                TRACE("merge_dbg", tout << "leq monotoinc k found a conjecture...\n"
                      << mk_pp(out, m) << "\n";);
                expr_ref_vector conj(m);
                conj.push_back(out);
                if(check_inductive_and_update(lemma, conj))
                    return;
            }

            // res = neighbour_equality(normalizedCube, to_app(neighbours.get(0)), neighbours, out);
            // if(res){
            //     TRACE("merge_dbg", tout << "neighbour equality found a conjecture...\n"
            //           << mk_pp(out, m););
            //     expr_ref_vector conj(m);
            //     conj.push_back(out);
            //     if(check_inductive_and_update(lemma, conj))
            //         return;
            // }

            TRACE("merge_dbg", tout << "Tried all merge strategies\n";);
            return;
        }
    }
    /*
      TODO cluster statistics / conjecture effective statistics
      TODO formalize guards
      TODO frame this as strategies
      TODO problem classification: linear pattern / non-linear patterns
      TODO guard normalization
    */

    /* with t <= k
       conjecture t <= infinite */
    bool lemma_merge_generalizer::leq_monotonic_k(expr_ref &literal, app *pattern, expr_ref &out){
        if(m_arith.is_le(pattern) && is_var(pattern->get_arg(1))){
            if(num_vars(pattern->get_arg(0)) == 0){
                out = m_arith.mk_eq(pattern->get_arg(0), m_arith.mk_int(0));
                return true;
            }
        }
        return false;
    }

    /* with t <= k , k < 0
       conjecture t <= 0 */
    bool lemma_merge_generalizer::leq_monotonic_neg_k(expr_ref &literal, app *pattern, expr_ref &out){
        if(m_arith.is_le(pattern) && is_var(pattern->get_arg(1))){
            SASSERT(is_app(literal));
            SASSERT(m_arith.is_numeral(to_app(literal)->get_arg(1)));
            rational r;
            m_arith.is_numeral(to_app(literal)->get_arg(1), r);
            if(r < 0){
                out = m_arith.mk_lt(pattern->get_arg(0), m_arith.mk_int(0));
                return true;
            } else if (r <= 0){
                out = m_arith.mk_le(pattern->get_arg(0), m_arith.mk_int(0));
                return true;
            }
        }
        return false;
    }

    /* with t1 <= k1 && k2 <= t2 , k1 + c = k2
       conjecture t1 + c' <= t2 where 0 <= c' <= c */
    // XXX potentially return expr_ref_vector for c' from 0 to c
    bool lemma_merge_generalizer::merge_halfspaces(expr_ref &literal, app *pattern, expr_ref &out){
        if(m.is_and(pattern) && pattern->get_num_args() == 2){
            app * concrete_fst = to_app(to_app(literal)->get_arg(0));
            app * concrete_snd = to_app(to_app(literal)->get_arg(1));
            app * fst = to_app(pattern->get_arg(0));
            app * snd = to_app(pattern->get_arg(1));
            TRACE("spacer_diverge_dbg",
                  tout << " fst : " << mk_pp(fst, m) << "\n"
                  << " snd : " << mk_pp(snd, m) << "\n"
                  << " c_fst : " << mk_pp(concrete_fst, m) << "\n"
                  << " c_snd : " << mk_pp(concrete_snd, m) << "\n"
                  ;);
            if(m_arith.is_ge(fst) && m_arith.is_le(snd)){
                rational n1, n2;
                TRACE("merge_dbg", tout << "GOT HERE >= & <=\n";);
                if(m_arith.is_numeral(concrete_fst->get_arg(1), n1) &&
                   m_arith.is_numeral(concrete_snd->get_arg(1), n2)){
                    if(n1 > n2){
                        out = m_arith.mk_gt(fst->get_arg(0), snd->get_arg(0));
                        return true;
                    }
                }
            }

            if(m_arith.is_le(fst) && m_arith.is_ge(snd)){
                rational n1, n2;
                TRACE("merge_dbg", tout << "GOT HERE <= & >=\n";);
                if(m_arith.is_numeral(concrete_fst->get_arg(1), n1) &&
                   m_arith.is_numeral(concrete_snd->get_arg(1), n2)){
                    if(n1 < n2){
                        out = m_arith.mk_lt(fst->get_arg(0), snd->get_arg(0));
                        return true;
                    }
                }
            }

        }
        return false;
        // rational k1, k2;
        // expr_ref t1(m), t2(m);
        // out = m_arith.mk_le(t1, t2);
        // // out = m_arith.mk_le(m_arith.mk_add(t1, m_arith.mk_int(k2 - k1)), t2);
        // return false;
    }

    /* with t1 = k1 && t2 = k2 , k1 + c = k2
       conjecture t1 + c' <= t2 where 0 <= c' <= c */
    // XXX should the lemma be t1 = k1 && t2 = k2 or we have to scan for all equalities?
    // XXX alternatively we can have another merge (and eq1 eq2 ... eqn)
    bool lemma_merge_generalizer::merge_lines(expr_ref &literal, app *pattern, expr_ref &out){
        rational k1, k2;
        expr_ref t1(m), t2(m);
        // out = m_arith.mk_le(t1, t2);
        out = m_arith.mk_eq(m_arith.mk_add(t1, m_arith.mk_int(k2 - k1)), t2);
        return false;
    }

    /*
       with k1 * t1 + k2 * t2 >= t3 , k1 > 0 , k2 > 0
       conjecture t1 + t2 >= 0
     */
    bool lemma_merge_generalizer::monotonic_coeffcient(expr_ref &literal, app *pattern, expr_ref &out){
        expr_ref_vector uni_consts(m), var_coeff(m);
        coeff_uninterpC_vec coeff_uniC;

        if(m_arith.is_ge(pattern) || m_arith.is_gt(pattern)){

            expr_ref_vector neg_coeff_uniCs(m), pos_coeff_uniCs(m);
            uninterp_consts_with_pos_coeff(to_app(literal), pos_coeff_uniCs);
            uninterp_consts_with_neg_coeff(to_app(literal), neg_coeff_uniCs);

            uninterp_consts_with_var_coeff(pattern, var_coeff, false);
            // XXX This check is necessary! arith.mk_add doesn't fallback gracefully with 0 as first argument

            if(var_coeff.size() > 0){
                expr_ref sum(m);
                sum = m_arith.mk_add(var_coeff.size(), var_coeff.c_ptr());
                // XXX TODO In case of mix signs on coeff, we need to spread on both sides of compare
                // if(neg_coeff_uniCs.size() > 0 && pos_coeff_uniCs.size() > 0) { return false; }
                // if the coefficients are negative we write positive coeff on out but with a sign flip on compare
                if(!neg_coeff_uniCs.empty()){
                    func_decl * f_decl = pattern->get_decl();
                    decl_kind dk = f_decl->get_decl_kind();
                    // TODO m.mk_app(m_faid, dk, sum, m_arith.mk_int(0))
                    out = m_arith.mk_lt(sum, m_arith.mk_int(0));
                }
                else if(!pos_coeff_uniCs.empty()){
                    out = m_arith.mk_lt(sum, m_arith.mk_int(0));
                }
                else { return false; }
                TRACE("merge_dbg", tout << "Mono coeff!\n"
                      << "Pattern: " << mk_pp(pattern, m) << "\n"
                      << "Cube: " << literal << "\n"
                      << "Out: " << mk_pp(out, m) << "\n"
                      << "Pos_coeff: " << pos_coeff_uniCs << "\n"
                      << "Neg_coeff: " << neg_coeff_uniCs << "\n"
                      ;);
                return true;
            }

        }
        return false;
    }

    /* merging over neighbours
       if we know a < b + k and b < a + k
       we can conjecture a == b
     */
    // XXX possibly the only merge without using pattern at all!
    bool lemma_merge_generalizer::neighbour_equality(expr_ref &literal, app *pattern, expr_ref_vector &neighbour, expr_ref &out){
        if( m_arith.is_ge(pattern) && num_uninterp_const(pattern) == 0){
            // for 0 <= i < n :: check literal.uninterp_consts[i] == neighbour[1].uninterp_consts[n-i]
            TRACE("merge_dbg", tout << "Enter neighbour eq\n";);
            expr_ref_vector uc1(m), uc2(m);
            uninterp_consts(to_app(literal), uc1);
            uninterp_consts(to_app(neighbour.get(1)), uc2);
            bool mismatch = false;
            TRACE("merge_dbg",
                  tout << "pattern:\n"
                  << mk_pp(pattern, m) << "\n"
                  << "uc1:\n"
                  << uc1 << "\n"
                  << "uc2:\n"
                  << uc1 << "\n"
                  ;);
            // if(uc1.size() == uc2.size() || uc1.size() == 0 || uc2.size() == 0){ return false; }
            out = m.mk_eq(uc1.get(0), uc2.get(0));
            return true;
            // int n = uc2.size();
            // for(int i = 0; i < n; i++){
            //     if(uc1.get(i) != uc2.get(n - 1 - i)){
            //         TRACE("merge_dbg", tout << "Mismatched!\n";);
            //         mismatch = true;
            //         break;
            //     }
            // }
            // if(!mismatch){
            //     out = m.mk_eq(uc1.get(0), uc2.get(0));
            //     return true;
            // }
        }
        return false;
    }



    /* core lemma update function*/
    bool lemma_merge_generalizer::check_inductive_and_update(lemma_ref &lemma, expr_ref_vector conj){
        TRACE("merge_dbg", tout << "Attempt to update lemma with: "
              << mk_pp(conj.back(), m) << "\n";);
        pred_transformer &pt = lemma->get_pob()->pt();
        lemma_ref_vector all_lemmas;
        pt.get_all_lemmas(all_lemmas, false);
        unsigned uses_level = 0;
        for(auto &l:all_lemmas) {
            if(m.are_equal(mk_and(l->get_cube()), mk_and(conj))){
                TRACE("merge_dbg", tout << "Already discovered lemma!" << "\n";);
                return false;
            }
        }
        if(pt.check_inductive(lemma->level(), conj, uses_level, lemma->weakness())){
            TRACE("merge_dbg", tout << "Inductive!" << "\n";);
            lemma->update_cube(lemma->get_pob(), conj);
            lemma->set_level(uses_level);
            return true;
        } else {
            TRACE("merge_dbg_", tout << "Not inductive!" << "\n";);
            return false;
        }
    }

    /* MISC functions */

    void lemma_merge_generalizer::uninterp_consts_with_var_coeff(app *a,
                                                                 expr_ref_vector &out,
                                                                 bool has_var_coeff)
    {
        for(expr *e : *a){
            if(is_uninterp_const(e) && has_var_coeff){
                out.push_back(e);
            }
            else if(is_app(e)){
                uninterp_consts_with_var_coeff(to_app(e), out, m_arith.is_mul(e) && (num_vars(e)>=1) );
            }
        }
    }

    void lemma_merge_generalizer::uninterp_consts_with_pos_coeff(app *a, expr_ref_vector &out)
    {
        for(expr *e : *a){
            if(m_arith.is_mul(e) && num_uninterp_const(to_app(e)) > 0){
                expr_ref_vector args(m);
                args.append(to_app(e)->get_num_args(), to_app(e)->get_args());
                for(auto &arg : args){
                    rational r;
                    if(m_arith.is_numeral(arg, r) && r > 0){
                        expr_ref_vector uninterpCs(m);
                        uninterp_consts(to_app(e), uninterpCs);
                        out.append(uninterpCs);
                    }
                }
            }
            else if(is_app(e)){
                uninterp_consts_with_pos_coeff(to_app(e), out);
            }
        }
    }

    void lemma_merge_generalizer::uninterp_consts_with_neg_coeff(app *a, expr_ref_vector &out)
    {
        for(expr *e : *a){
            if(m_arith.is_mul(e) && num_uninterp_const(to_app(e)) > 0){
                expr_ref_vector args(m);
                args.append(to_app(e)->get_num_args(), to_app(e)->get_args());
                for(auto &arg : args){
                    rational r;
                    if(m_arith.is_numeral(arg, r) && r < 0){
                        expr_ref_vector uninterpCs(m);
                        uninterp_consts(to_app(e), uninterpCs);
                        out.append(uninterpCs);
                    }
                }
            }
            else if(is_app(e)){
                uninterp_consts_with_neg_coeff(to_app(e), out);
            }
        }
    }


    void lemma_merge_generalizer::get_uninterp_consts(app_ref a, coeff_uninterpC_vec &out){
        ast_manager m = a.get_manager();
        arith_util m_arith(m);
    }
}
