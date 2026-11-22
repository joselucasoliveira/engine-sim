#ifndef ENGINE_SIM_CHECKED_SOLVER_H
#define ENGINE_SIM_CHECKED_SOLVER_H

#include "simulation_fault.h"
#include "scs.h"

namespace engine_sim {
class CheckedSolver : public atg_scs::GaussSeidelSleSolver {
public:
    bool solveWithLimits(atg_scs::SparseMatrix<3> &jacobian, atg_scs::Matrix &mass,
            atg_scs::Matrix &right, atg_scs::Matrix &limits, atg_scs::Matrix *result,
            atg_scs::Matrix *previous) override {
        if (!GaussSeidelSleSolver::solveWithLimits(jacobian, mass, right, limits, result, previous)) {
            throw SimulationFault("constraint solver did not converge");
        }
        return true;
    }
};
}
#endif
