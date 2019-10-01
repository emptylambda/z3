#pragma once
#include "ast/arith_decl_plugin.h"
#include "ast/ast.h"
#include "ast/ast_util.h"
#include "muz/spacer/spacer_arith_kernel.h"
#include "muz/spacer/spacer_matrix.h"
#include "muz/spacer/spacer_sage_interface.h"
#include "muz/spacer/spacer_util.h"
namespace spacer {
class convex_closure {
    ast_manager &m;
    arith_util m_arith;
    unsigned m_dim;
    bool m_use_sage;
    spacer_matrix m_data;
    bool is_int_points() const;
    vector<expr *> m_dim_vars;
    arith_kernel *m_kernel;
    unsigned reduce_dim();
    void rewrite_lin_deps();

  public:
    convex_closure(ast_manager &man, bool use_sage)
        : m(man), m_arith(m), m_dim(0), m_use_sage(use_sage), m_data(0, 0) {
        if (m_use_sage) m_kernel = new Sage_kernel(0, 0);
    }
    ~convex_closure() {
        if (m_use_sage) delete m_kernel;
    }
    void reset(unsigned n_rows, unsigned n_cols) {
        m_data = spacer_matrix(n_rows, n_cols);
        m_dim_vars.reset();
        m_dim = n_cols;
        m_dim_vars.reserve(m_dim);
        m_kernel->reset(n_rows, n_cols);
    }
    /// Incremental interface

    /// \brief Name a dimension
    void set_dimension(unsigned i, expr *v) {
        SASSERT(i < dims());
        SASSERT(m_dim_vars[i] == nullptr);
        m_dim_vars[i] = v;
    }
    /// \brief Return number of dimensions of each point
    unsigned dims() { return m_dim; }

    /// \brief add one-dimensional point
    void push_back(rational x) {
        SASSERT(dims() == 1);
        // Insert into m_kernel as well
        NOT_IMPLEMENTED_YET();
    }
    /// \brief add two-dimensional point
    void push_back(rational x, rational y) { NOT_IMPLEMENTED_YET(); }
    /// \brief add three-dimensional point
    void push_back(rational x, rational y, rational z) {
        NOT_IMPLEMENTED_YET();
    }
    /// \brief add n-dimensional point
    void push_back(vector<rational> &point) { NOT_IMPLEMENTED_YET(); };

    /// \brief compute convex closure of current set of points
    /// return true if it was possible to compute the closure
    bool closure(expr_ref &res);
};
} // namespace spacer
