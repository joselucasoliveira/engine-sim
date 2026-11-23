#ifndef ENGINE_SIM_CHECKED_SOLVER_H
#define ENGINE_SIM_CHECKED_SOLVER_H

#include "simulation_fault.h"
#include "scs.h"

namespace engine_sim {
class CheckedSolver : public atg_scs::GaussSeidelSleSolver {
public:
    static void initializeSparseWorkspace(atg_scs::SparseMatrix<3> &matrix, int width, int rows) {
        matrix.initialize(width, rows);
        for (int row = 0; row < rows; ++row) {
            matrix.setEmpty(row, 0);
            matrix.setEmpty(row, 1);
        }
    }

    void prepareFixedWorkspace(int coordinates, int rows) {
        m_M.initialize(rows, rows);
        initializeSparseWorkspace(m_reg, coordinates, rows);
    }

    static bool sparseWorkspaceStartsClear(atg_scs::SparseMatrix<3> &matrix, int rows) {
        for (int row = 0; row < rows; ++row)
            for (int entry = 0; entry < 2; ++entry)
                for (int lane = 0; lane < 3; ++lane)
                    if (matrix.get(row, entry, lane) != 0.0) return false;
        return true;
    }

    bool sparseWorkspaceStartsClear(int rows) { return sparseWorkspaceStartsClear(m_reg, rows); }

};
}
#endif
