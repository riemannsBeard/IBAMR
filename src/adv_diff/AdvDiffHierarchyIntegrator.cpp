// Filename: AdvDiffHierarchyIntegrator.cpp
// Created on 21 May 2012 by Boyce Griffith
//
// Copyright (c) 2002-2014, Boyce Griffith
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of The University of North Carolina nor the names of
//      its contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

/////////////////////////////// INCLUDES /////////////////////////////////////

#include <stddef.h>
#include <algorithm>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "SAMRAI/hier/PatchHierarchy.h"
#include "SAMRAI/hier/Box.h"
#include "SAMRAI/geom/CartesianGridGeometry.h"
#include "SAMRAI/geom/CartesianPatchGeometry.h"
#include "SAMRAI/pdat/CellDataFactory.h"
#include "SAMRAI/pdat/CellVariable.h"
#include "SAMRAI/xfer/CoarsenAlgorithm.h"
#include "SAMRAI/hier/CoarsenOperator.h"
#include "SAMRAI/pdat/FaceData.h"
#include "SAMRAI/pdat/FaceVariable.h"
#include "SAMRAI/mesh/GriddingAlgorithm.h"
#include "SAMRAI/math/HierarchyCellDataOpsReal.h"
#include "SAMRAI/math/HierarchyDataOpsManager.h"
#include "SAMRAI/math/HierarchyDataOpsReal.h"
#include "SAMRAI/math/HierarchySideDataOpsReal.h"
#include "IBAMR_config.h"
#include "SAMRAI/hier/Index.h"
#include "SAMRAI/hier/IntVector.h"

#include "SAMRAI/hier/Patch.h"
#include "SAMRAI/hier/PatchHierarchy.h"
#include "SAMRAI/hier/PatchLevel.h"
#include "SAMRAI/solv/SAMRAIVectorReal.h"
#include "SAMRAI/pdat/SideDataFactory.h"
#include "SAMRAI/pdat/SideVariable.h"
#include "SAMRAI/hier/Variable.h"
#include "SAMRAI/hier/VariableContext.h"
#include "SAMRAI/hier/VariableDatabase.h"
#include "SAMRAI/appu/VisItDataWriter.h"
#include "ibamr/AdvDiffHierarchyIntegrator.h"
#include "ibamr/ibamr_enums.h"
#include "ibamr/ibamr_utilities.h"
#include "ibamr/namespaces.h" // IWYU pragma: keep
#include "ibtk/CCLaplaceOperator.h"
#include "ibtk/CCPoissonSolverManager.h"
#include "ibtk/CartGridFunction.h"
#include "ibtk/CartGridFunctionSet.h"
#include "ibtk/HierarchyGhostCellInterpolation.h"
#include "ibtk/HierarchyIntegrator.h"
#include "ibtk/HierarchyMathOps.h"
#include "ibtk/LaplaceOperator.h"
#include "ibtk/PoissonSolver.h"
#include "SAMRAI/tbox/Database.h"
#include "SAMRAI/tbox/MathUtilities.h"
#include "SAMRAI/tbox/MemoryDatabase.h"
#include "SAMRAI/tbox/PIO.h"

#include "SAMRAI/tbox/RestartManager.h"
#include "SAMRAI/tbox/Utilities.h"

namespace SAMRAI
{
namespace solv
{
class RobinBcCoefStrategy;
} // namespace solv
} // namespace SAMRAI

// FORTRAN ROUTINES
#if (NDIM == 2)
#define ADVECT_STABLEDT_FC IBAMR_FC_FUNC_(advect_stabledt2d, ADVECT_STABLEDT2D)
#endif

#if (NDIM == 3)
#define ADVECT_STABLEDT_FC IBAMR_FC_FUNC_(advect_stabledt3d, ADVECT_STABLEDT3D)
#endif

extern "C" {
void ADVECT_STABLEDT_FC(const double*,
#if (NDIM == 2)
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const double*,
                        const double*,
#endif
#if (NDIM == 3)
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const int&,
                        const double*,
                        const double*,
                        const double*,
#endif
                        double&);
}

/////////////////////////////// NAMESPACE ////////////////////////////////////

namespace IBAMR
{
/////////////////////////////// STATIC ///////////////////////////////////////

namespace
{
// Number of ghosts cells used for each variable quantity.
static const int CELLG = 1;
static const int FACEG = 1;

// Types of refining and coarsening to perform prior to setting coarse-fine
// boundary and physical boundary ghost cell values.
static const std::string DATA_REFINE_TYPE = "NONE";
static const bool USE_CF_INTERPOLATION = true;
static const std::string DATA_COARSEN_TYPE = "CUBIC_COARSEN";

// Type of extrapolation to use at physical boundaries.
static const std::string BDRY_EXTRAP_TYPE = "LINEAR";

// Whether to enforce consistent interpolated values at Type 2 coarse-fine
// interface ghost cells.
static const bool CONSISTENT_TYPE_2_BDRY = false;

// Version of AdvDiffHierarchyIntegrator restart file data.
static const int ADV_DIFF_HIERARCHY_INTEGRATOR_VERSION = 3;
}

/////////////////////////////// PUBLIC ///////////////////////////////////////

AdvDiffHierarchyIntegrator::~AdvDiffHierarchyIntegrator()
{
    d_helmholtz_solvers.clear();
    d_helmholtz_rhs_ops.clear();
    return;
} // ~AdvDiffHierarchyIntegrator

void
AdvDiffHierarchyIntegrator::setDefaultDiffusionTimeSteppingType(TimeSteppingType default_diffusion_time_stepping_type)
{
    d_default_diffusion_time_stepping_type = default_diffusion_time_stepping_type;
    return;
} // setDefaultDiffusionTimeSteppingType

TimeSteppingType AdvDiffHierarchyIntegrator::getDefaultDiffusionTimeSteppingType() const
{
    return d_default_diffusion_time_stepping_type;
} // getDefaultDiffusionTimeSteppingType

void AdvDiffHierarchyIntegrator::setDefaultConvectiveDifferencingType(
    ConvectiveDifferencingType default_convective_difference_form)
{
    d_default_convective_difference_form = default_convective_difference_form;
    return;
} // setDefaultConvectiveDifferencingType

ConvectiveDifferencingType AdvDiffHierarchyIntegrator::getDefaultConvectiveDifferencingType() const
{
    return d_default_convective_difference_form;
} // getDefaultConvectiveDifferencingType

void AdvDiffHierarchyIntegrator::registerAdvectionVelocity(boost::shared_ptr<FaceVariable<double> > u_var)
{
    TBOX_ASSERT(u_var);
    TBOX_ASSERT(std::find(d_u_var.begin(), d_u_var.end(), u_var) == d_u_var.end());
    d_u_var.push_back(u_var);

    // Set default values.
    d_u_is_div_free[u_var] = true;
    d_u_fcn[u_var] = NULL;
    return;
} // registerAdvectionVelocity

void AdvDiffHierarchyIntegrator::setAdvectionVelocityIsDivergenceFree(boost::shared_ptr<FaceVariable<double> > u_var,
                                                                      const bool is_div_free)
{
    TBOX_ASSERT(std::find(d_u_var.begin(), d_u_var.end(), u_var) != d_u_var.end());
    d_u_is_div_free[u_var] = is_div_free;
    return;
} // setAdvectionVelocityIsDivergenceFree

bool
AdvDiffHierarchyIntegrator::getAdvectionVelocityIsDivergenceFree(boost::shared_ptr<FaceVariable<double> > u_var) const
{
    TBOX_ASSERT(std::find(d_u_var.begin(), d_u_var.end(), u_var) != d_u_var.end());
    return d_u_is_div_free.find(u_var)->second;
} // getAdvectionVelocityIsDivergenceFree

void AdvDiffHierarchyIntegrator::setAdvectionVelocityFunction(boost::shared_ptr<FaceVariable<double> > u_var,
                                                              boost::shared_ptr<IBTK::CartGridFunction> u_fcn)
{
    TBOX_ASSERT(std::find(d_u_var.begin(), d_u_var.end(), u_var) != d_u_var.end());
    d_u_fcn[u_var] = u_fcn;
    return;
} // setAdvectionVelocityFunction

boost::shared_ptr<IBTK::CartGridFunction>
AdvDiffHierarchyIntegrator::getAdvectionVelocityFunction(boost::shared_ptr<FaceVariable<double> > u_var) const
{
    TBOX_ASSERT(std::find(d_u_var.begin(), d_u_var.end(), u_var) != d_u_var.end());
    return d_u_fcn.find(u_var)->second;
} // getAdvectionVelocityFunction

void AdvDiffHierarchyIntegrator::registerSourceTerm(boost::shared_ptr<CellVariable<double> > F_var)
{
    TBOX_ASSERT(F_var);
    TBOX_ASSERT(std::find(d_F_var.begin(), d_F_var.end(), F_var) == d_F_var.end());
    d_F_var.push_back(F_var);

    // Set default values.
    d_F_fcn[F_var] = NULL;
    return;
} // registerSourceTerm

void AdvDiffHierarchyIntegrator::setSourceTermFunction(boost::shared_ptr<CellVariable<double> > F_var,
                                                       boost::shared_ptr<IBTK::CartGridFunction> F_fcn)
{
    TBOX_ASSERT(std::find(d_F_var.begin(), d_F_var.end(), F_var) != d_F_var.end());
    if (d_F_fcn[F_var])
    {
        const std::string& F_var_name = F_var->getName();
        boost::shared_ptr<CartGridFunctionSet> p_F_fcn = d_F_fcn[F_var];
        if (!p_F_fcn)
        {
            pout << d_object_name << "::setSourceTermFunction(): WARNING:\n"
                 << "  source term function for source term variable " << F_var_name << " has already been set.\n"
                 << "  functions will be evaluated in the order in which they were registered "
                    "with "
                    "the solver\n"
                 << "  when evaluating the source term value.\n";
            p_F_fcn =
                boost::make_shared<CartGridFunctionSet>(d_object_name + "::" + F_var_name + "::source_function_set");
            p_F_fcn->addFunction(d_F_fcn[F_var]);
        }
        p_F_fcn->addFunction(F_fcn);
    }
    else
    {
        d_F_fcn[F_var] = F_fcn;
    }
    return;
} // setSourceTermFunction

boost::shared_ptr<IBTK::CartGridFunction>
AdvDiffHierarchyIntegrator::getSourceTermFunction(boost::shared_ptr<CellVariable<double> > F_var) const
{
    TBOX_ASSERT(std::find(d_F_var.begin(), d_F_var.end(), F_var) != d_F_var.end());
    return d_F_fcn.find(F_var)->second;
} // getSourceTermFunction

void AdvDiffHierarchyIntegrator::registerTransportedQuantity(boost::shared_ptr<CellVariable<double> > Q_var)
{
    TBOX_ASSERT(Q_var);
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) == d_Q_var.end());
    d_Q_var.push_back(Q_var);
    const int Q_depth = Q_var->getDepth();
    auto Q_rhs_var = boost::make_shared<CellVariable<double> >(DIM, Q_var->getName() + "::Q_rhs", Q_depth);

    // Set default values.
    d_Q_u_map[Q_var] = NULL;
    d_Q_F_map[Q_var] = NULL;
    d_Q_rhs_var.push_back(Q_rhs_var);
    d_Q_Q_rhs_map[Q_var] = Q_rhs_var;
    d_Q_diffusion_time_stepping_type[Q_var] = d_default_diffusion_time_stepping_type;
    d_Q_difference_form[Q_var] = d_default_convective_difference_form;
    d_Q_diffusion_coef[Q_var] = 0.0;
    d_Q_diffusion_coef_variable[Q_var] = NULL;
    d_Q_is_diffusion_coef_variable[Q_var] = false;
    d_Q_damping_coef[Q_var] = 0.0;
    d_Q_init[Q_var] = NULL;
    d_Q_bc_coef[Q_var] = std::vector<RobinBcCoefStrategy*>(Q_depth, static_cast<RobinBcCoefStrategy*>(NULL));
    return;
} // registerTransportedQuantity

void AdvDiffHierarchyIntegrator::setAdvectionVelocity(boost::shared_ptr<CellVariable<double> > Q_var,
                                                      boost::shared_ptr<FaceVariable<double> > u_var)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    TBOX_ASSERT(std::find(d_u_var.begin(), d_u_var.end(), u_var) != d_u_var.end());
    d_Q_u_map[Q_var] = u_var;
    return;
} // setAdvectionVelocity

boost::shared_ptr<FaceVariable<double> >
AdvDiffHierarchyIntegrator::getAdvectionVelocity(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_u_map.find(Q_var)->second;
} // getAdvectionVelocity

void AdvDiffHierarchyIntegrator::setSourceTerm(boost::shared_ptr<CellVariable<double> > Q_var,
                                               boost::shared_ptr<CellVariable<double> > F_var)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    TBOX_ASSERT(std::find(d_F_var.begin(), d_F_var.end(), F_var) != d_F_var.end());
    d_Q_F_map[Q_var] = F_var;
    return;
} // setSourceTerm

boost::shared_ptr<CellVariable<double> >
AdvDiffHierarchyIntegrator::getSourceTerm(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_F_map.find(Q_var)->second;
} // getSourceTerm

void AdvDiffHierarchyIntegrator::setDiffusionTimeSteppingType(boost::shared_ptr<CellVariable<double> > Q_var,
                                                              const TimeSteppingType diffusion_time_stepping_type)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    d_Q_diffusion_time_stepping_type[Q_var] = diffusion_time_stepping_type;
    return;
} // setDiffusionTimeSteppingType

TimeSteppingType
AdvDiffHierarchyIntegrator::getDiffusionTimeSteppingType(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_diffusion_time_stepping_type.find(Q_var)->second;
} // getDiffusionTimeSteppingType

void AdvDiffHierarchyIntegrator::setConvectiveDifferencingType(boost::shared_ptr<CellVariable<double> > Q_var,
                                                               const ConvectiveDifferencingType difference_form)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    d_Q_difference_form[Q_var] = difference_form;
    return;
} // setConvectiveDifferencingType

ConvectiveDifferencingType
AdvDiffHierarchyIntegrator::getConvectiveDifferencingType(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_difference_form.find(Q_var)->second;
} // getConvectiveDifferencingType

void AdvDiffHierarchyIntegrator::setDiffusionCoefficient(boost::shared_ptr<CellVariable<double> > Q_var,
                                                         const double kappa)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    d_Q_diffusion_coef[Q_var] = kappa;
    // indicate that the diffusion coefficient associated to Q_var is constant.
    d_Q_is_diffusion_coef_variable[Q_var] = false;
    // if there is already a variable diffusion coefficient associated to Q_var
    if (d_Q_diffusion_coef_variable[Q_var])
    {
        const std::string& Q_var_name = Q_var->getName();
        boost::shared_ptr<SideVariable<double> > D_var = d_Q_diffusion_coef_variable[Q_var];
        // print a warning.
        pout << d_object_name << "::setDiffusionCoefficient(boost::shared_ptr<CellVariable<double> > "
                                 "Q_var, const double kappa): WARNING: \n"
             << "   a variable diffusion coefficient for the variable " << Q_var_name << " has already been set.\n"
             << "   this variable coefficient will be overriden by the constant diffusion "
                "coefficient "
             << "kappa = " << kappa << "\n";
        // erase entries from maps with key D_var
        d_diffusion_coef_fcn.erase(D_var);
        d_diffusion_coef_rhs_map.erase(D_var);
        // set a null entry in the map for variable diffusion coefficients.
        d_Q_diffusion_coef_variable[Q_var] = NULL;
    }
    return;
} // setDiffusionCoefficient

double AdvDiffHierarchyIntegrator::getDiffusionCoefficient(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_diffusion_coef.find(Q_var)->second;
} // getDiffusionCoefficient

void AdvDiffHierarchyIntegrator::registerDiffusionCoefficientVariable(boost::shared_ptr<SideVariable<double> > D_var)
{
    TBOX_ASSERT(D_var);
    TBOX_ASSERT(std::find(d_diffusion_coef_var.begin(), d_diffusion_coef_var.end(), D_var) ==
                d_diffusion_coef_var.end());
    d_diffusion_coef_var.push_back(D_var);
    const int D_depth = D_var->getDepth();
    auto D_rhs_var = boost::make_shared<SideVariable<double> >(DIM, D_var->getName() + "::D_rhs", D_depth);

    // Set default values.
    d_diffusion_coef_fcn[D_var] = NULL;
    d_diffusion_coef_rhs_map[D_var] = D_rhs_var;
    // Also register D_rhs_var
    d_diffusion_coef_rhs_var.push_back(D_rhs_var);
    return;
} // registerDiffusionCoefficientVariable

void AdvDiffHierarchyIntegrator::setDiffusionCoefficientFunction(boost::shared_ptr<SideVariable<double> > D_var,
                                                                 boost::shared_ptr<IBTK::CartGridFunction> D_fcn)
{
    TBOX_ASSERT(std::find(d_diffusion_coef_var.begin(), d_diffusion_coef_var.end(), D_var) !=
                d_diffusion_coef_var.end());
    if (d_diffusion_coef_fcn[D_var])
    {
        const std::string& D_var_name = D_var->getName();
        boost::shared_ptr<CartGridFunctionSet> p_D_fcn = d_diffusion_coef_fcn[D_var];
        if (!p_D_fcn)
        {
            pout << d_object_name << "::setDiffusionCoefficientFunction(): WARNING:\n"
                 << "  diffusion coefficient function for diffusion coefficient variable " << D_var_name
                 << " has already been set.\n"
                 << "  functions will be evaluated in the order in which they were registered "
                    "with "
                    "the solver\n"
                 << "  when evaluating the source term value.\n";
            p_D_fcn = boost::make_shared<CartGridFunctionSet>(d_object_name + "::" + D_var_name +
                                                              "::diffusion_coef_function_set");
            p_D_fcn->addFunction(d_diffusion_coef_fcn[D_var]);
        }
        p_D_fcn->addFunction(D_fcn);
    }
    else
    {
        d_diffusion_coef_fcn[D_var] = D_fcn;
    }
    return;
} // setDiffusionCoefficientFunction

boost::shared_ptr<IBTK::CartGridFunction>
AdvDiffHierarchyIntegrator::getDiffusionCoefficientFunction(boost::shared_ptr<SideVariable<double> > D_var) const
{
    TBOX_ASSERT(std::find(d_diffusion_coef_var.begin(), d_diffusion_coef_var.end(), D_var) !=
                d_diffusion_coef_var.end());
    return d_diffusion_coef_fcn.find(D_var)->second;
} // getDiffusionCoefficientFunction

void AdvDiffHierarchyIntegrator::setDiffusionCoefficientVariable(boost::shared_ptr<CellVariable<double> > Q_var,
                                                                 boost::shared_ptr<SideVariable<double> > D_var)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    TBOX_ASSERT(std::find(d_diffusion_coef_var.begin(), d_diffusion_coef_var.end(), D_var) !=
                d_diffusion_coef_var.end());
    d_Q_diffusion_coef_variable[Q_var] = D_var;
    // indicate that the diffusion coefficient associated to Q_var is variable
    d_Q_is_diffusion_coef_variable[Q_var] = true;
    // set the corresponding constant diffusion coefficient to zero.
    d_Q_diffusion_coef[Q_var] = 0.0;
    return;
} // setDiffusionCoefficientVariable

boost::shared_ptr<SideVariable<double> >
AdvDiffHierarchyIntegrator::getDiffusionCoefficientVariable(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_diffusion_coef_variable.find(Q_var)->second;
} // getDiffusionCoefficientVariable

bool AdvDiffHierarchyIntegrator::isDiffusionCoefficientVariable(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_is_diffusion_coef_variable.find(Q_var)->second;
} // isDiffusionCoefficientVariable

void AdvDiffHierarchyIntegrator::setDampingCoefficient(boost::shared_ptr<CellVariable<double> > Q_var,
                                                       const double lambda)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    d_Q_damping_coef[Q_var] = lambda;
    return;
} // setDampingCoefficient

double AdvDiffHierarchyIntegrator::getDampingCoefficient(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_damping_coef.find(Q_var)->second;
} // getDampingCoefficient

void AdvDiffHierarchyIntegrator::setInitialConditions(boost::shared_ptr<CellVariable<double> > Q_var,
                                                      boost::shared_ptr<IBTK::CartGridFunction> Q_init)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    d_Q_init[Q_var] = Q_init;
    return;
} // setInitialConditions

boost::shared_ptr<IBTK::CartGridFunction>
AdvDiffHierarchyIntegrator::getInitialConditions(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_init.find(Q_var)->second;
} // getInitialConditions

void AdvDiffHierarchyIntegrator::setPhysicalBcCoef(boost::shared_ptr<CellVariable<double> > Q_var,
                                                   RobinBcCoefStrategy* Q_bc_coef)
{
    setPhysicalBcCoefs(Q_var, std::vector<RobinBcCoefStrategy*>(1, Q_bc_coef));
    return;
} // setPhysicalBcCoef

void AdvDiffHierarchyIntegrator::setPhysicalBcCoefs(boost::shared_ptr<CellVariable<double> > Q_var,
                                                    const std::vector<RobinBcCoefStrategy*>& Q_bc_coef)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    const unsigned int Q_depth = Q_var->getDepth();
    TBOX_ASSERT(Q_depth == Q_bc_coef.size());
    d_Q_bc_coef[Q_var] = Q_bc_coef;
    return;
} // setPhysicalBcCoefs

std::vector<RobinBcCoefStrategy*>
AdvDiffHierarchyIntegrator::getPhysicalBcCoefs(boost::shared_ptr<CellVariable<double> > Q_var) const
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    return d_Q_bc_coef.find(Q_var)->second;
} // getPhysicalBcCoefs

void AdvDiffHierarchyIntegrator::setHelmholtzSolver(boost::shared_ptr<CellVariable<double> > Q_var,
                                                    boost::shared_ptr<PoissonSolver> helmholtz_solver)
{
    d_helmholtz_solvers.resize(d_Q_var.size());
    d_helmholtz_solvers_need_init.resize(d_Q_var.size());
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    const size_t l = distance(d_Q_var.begin(), std::find(d_Q_var.begin(), d_Q_var.end(), Q_var));
    TBOX_ASSERT(!d_helmholtz_solvers[l]);
    d_helmholtz_solvers[l] = helmholtz_solver;
    d_helmholtz_solvers_need_init[l] = true;
    return;
} // setHelmholtzSolver

boost::shared_ptr<PoissonSolver>
AdvDiffHierarchyIntegrator::getHelmholtzSolver(boost::shared_ptr<CellVariable<double> > Q_var)
{
    d_helmholtz_solvers.resize(d_Q_var.size());
    d_helmholtz_solvers_need_init.resize(d_Q_var.size());
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    const size_t l = distance(d_Q_var.begin(), std::find(d_Q_var.begin(), d_Q_var.end(), Q_var));
    if (!d_helmholtz_solvers[l])
    {
        const std::string& name = Q_var->getName();
        d_helmholtz_solvers[l] =
            CCPoissonSolverManager::getManager()->allocateSolver(d_helmholtz_solver_type,
                                                                 d_object_name + "::helmholtz_solver::" + name,
                                                                 d_helmholtz_solver_db,
                                                                 "adv_diff_",
                                                                 d_helmholtz_precond_type,
                                                                 d_object_name + "::helmholtz_precond::" + name,
                                                                 d_helmholtz_precond_db,
                                                                 "adv_diff_pc_");
        d_helmholtz_solvers_need_init[l] = true;
    }
    return d_helmholtz_solvers[l];
} // getHelmholtzSolver

void AdvDiffHierarchyIntegrator::setHelmholtzSolversNeedInit()
{
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::iterator it = d_Q_var.begin(); it != d_Q_var.end();
         ++it)
    {
        setHelmholtzSolverNeedsInit(*it);
    }
    return;
}

void AdvDiffHierarchyIntegrator::setHelmholtzSolverNeedsInit(boost::shared_ptr<CellVariable<double> > Q_var)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    const size_t l = distance(d_Q_var.begin(), std::find(d_Q_var.begin(), d_Q_var.end(), Q_var));
    d_helmholtz_solvers_need_init[l] = true;
    return;
}

void AdvDiffHierarchyIntegrator::setHelmholtzRHSOperator(boost::shared_ptr<CellVariable<double> > Q_var,
                                                         boost::shared_ptr<LaplaceOperator> helmholtz_op)
{
    d_helmholtz_rhs_ops.resize(d_Q_var.size());
    d_helmholtz_rhs_ops_need_init.resize(d_Q_var.size());
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    const size_t l = distance(d_Q_var.begin(), std::find(d_Q_var.begin(), d_Q_var.end(), Q_var));
    TBOX_ASSERT(!d_helmholtz_rhs_ops[l]);
    d_helmholtz_rhs_ops[l] = helmholtz_op;
    d_helmholtz_rhs_ops_need_init[l] = true;
    return;
} // setHelmholtzRHSOperator

boost::shared_ptr<LaplaceOperator>
AdvDiffHierarchyIntegrator::getHelmholtzRHSOperator(boost::shared_ptr<CellVariable<double> > Q_var)
{
    d_helmholtz_rhs_ops.resize(d_Q_var.size());
    d_helmholtz_rhs_ops_need_init.resize(d_Q_var.size());
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    const size_t l = distance(d_Q_var.begin(), std::find(d_Q_var.begin(), d_Q_var.end(), Q_var));
    if (!d_helmholtz_rhs_ops[l])
    {
        const std::string& name = Q_var->getName();
        d_helmholtz_rhs_ops[l] = boost::make_shared<CCLaplaceOperator>(d_object_name + "::helmholtz_rhs_op::" + name,
                                                                       /*homogeneous_bc*/ false);
        d_helmholtz_rhs_ops_need_init[l] = true;
    }
    return d_helmholtz_rhs_ops[l];
} // getHelmholtzRHSOperator

void AdvDiffHierarchyIntegrator::setHelmholtzRHSOperatorsNeedInit()
{
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::iterator it = d_Q_var.begin(); it != d_Q_var.end();
         ++it)
    {
        setHelmholtzRHSOperatorNeedsInit(*it);
    }
    return;
}

void AdvDiffHierarchyIntegrator::setHelmholtzRHSOperatorNeedsInit(boost::shared_ptr<CellVariable<double> > Q_var)
{
    TBOX_ASSERT(std::find(d_Q_var.begin(), d_Q_var.end(), Q_var) != d_Q_var.end());
    const size_t l = distance(d_Q_var.begin(), std::find(d_Q_var.begin(), d_Q_var.end(), Q_var));
    d_helmholtz_rhs_ops_need_init[l] = true;
    return;
}

void AdvDiffHierarchyIntegrator::initializeHierarchyIntegrator(boost::shared_ptr<PatchHierarchy> hierarchy,
                                                               boost::shared_ptr<GriddingAlgorithm> gridding_alg)
{
    if (d_integrator_is_initialized) return;

    d_hierarchy = hierarchy;
    d_gridding_alg = gridding_alg;
    auto grid_geom = BOOST_CAST<CartesianGridGeometry>(d_hierarchy->getGridGeometry());

    // Setup hierarchy data operations objects.
    HierarchyDataOpsManager* hier_ops_manager = HierarchyDataOpsManager::getManager();
    auto cc_var = boost::make_shared<CellVariable<double> >(DIM, "cc_var");
    d_hier_cc_data_ops = hier_ops_manager->getOperationsDouble(cc_var, d_hierarchy, true);
    auto sc_var = boost::make_shared<SideVariable<double> >(DIM, "sc_var");
    d_hier_sc_data_ops = hier_ops_manager->getOperationsDouble(sc_var, d_hierarchy, true);

    // Setup coarsening communications algorithms, used in synchronizing refined
    // regions of coarse data with the underlying fine data.
    VariableDatabase* var_db = VariableDatabase::getDatabase();
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::const_iterator cit = d_Q_var.begin();
         cit != d_Q_var.end();
         ++cit)
    {
        boost::shared_ptr<CellVariable<double> > Q_var = *cit;
        const int Q_current_idx = var_db->mapVariableAndContextToIndex(Q_var, getCurrentContext());
        const int Q_new_idx = var_db->mapVariableAndContextToIndex(Q_var, getNewContext());
        boost::shared_ptr<CoarsenOperator> coarsen_operator =
            grid_geom->lookupCoarsenOperator(Q_var, "CONSERVATIVE_COARSEN");
        getCoarsenAlgorithm(SYNCH_CURRENT_DATA_ALG)->registerCoarsen(Q_current_idx, Q_current_idx, coarsen_operator);
        getCoarsenAlgorithm(SYNCH_NEW_DATA_ALG)->registerCoarsen(Q_new_idx, Q_new_idx, coarsen_operator);
    }

    // Operators and solvers are maintained for each variable registered with the
    // integrator.
    if (d_helmholtz_solver_type == CCPoissonSolverManager::UNDEFINED)
    {
        d_helmholtz_solver_type = CCPoissonSolverManager::DEFAULT_KRYLOV_SOLVER;
    }
    if (d_helmholtz_precond_type == CCPoissonSolverManager::UNDEFINED)
    {
        const int max_levels = d_hierarchy->getMaxNumberOfLevels();
        if (max_levels == 1)
        {
            d_helmholtz_precond_type = CCPoissonSolverManager::DEFAULT_LEVEL_SOLVER;
        }
        else
        {
            d_helmholtz_precond_type = CCPoissonSolverManager::DEFAULT_FAC_PRECONDITIONER;
        }
        d_helmholtz_precond_db->putInteger("max_iterations", 1);
    }
    d_helmholtz_solvers.resize(d_Q_var.size());
    d_helmholtz_solvers_need_init.resize(d_Q_var.size());
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::const_iterator cit = d_Q_var.begin();
         cit != d_Q_var.end();
         ++cit)
    {
        boost::shared_ptr<CellVariable<double> > Q_var = *cit;
        const size_t l = distance(d_Q_var.begin(), std::find(d_Q_var.begin(), d_Q_var.end(), Q_var));
        d_helmholtz_solvers[l] = getHelmholtzSolver(Q_var);
    }
    d_helmholtz_rhs_ops.resize(d_Q_var.size());
    d_helmholtz_rhs_ops_need_init.resize(d_Q_var.size());
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::const_iterator cit = d_Q_var.begin();
         cit != d_Q_var.end();
         ++cit)
    {
        boost::shared_ptr<CellVariable<double> > Q_var = *cit;
        const size_t l = distance(d_Q_var.begin(), std::find(d_Q_var.begin(), d_Q_var.end(), Q_var));
        d_helmholtz_rhs_ops[l] = getHelmholtzRHSOperator(Q_var);
    }

    // Indicate that the integrator has been initialized.
    d_integrator_is_initialized = true;
    return;
} // initializeHierarchyIntegrator

/////////////////////////////// PROTECTED ////////////////////////////////////

AdvDiffHierarchyIntegrator::AdvDiffHierarchyIntegrator(const std::string& object_name,
                                                       boost::shared_ptr<Database> input_db,
                                                       bool register_for_restart)
    : HierarchyIntegrator(object_name, input_db, register_for_restart), d_integrator_is_initialized(false),
      d_cfl_max(0.5), d_default_diffusion_time_stepping_type(TRAPEZOIDAL_RULE),
      d_default_convective_difference_form(CONSERVATIVE), d_u_var(), d_u_is_div_free(), d_u_fcn(), d_F_var(), d_F_fcn(),
      d_diffusion_coef_var(), d_diffusion_coef_rhs_var(), d_diffusion_coef_fcn(), d_diffusion_coef_rhs_map(), d_Q_var(),
      d_Q_rhs_var(), d_Q_u_map(), d_Q_F_map(), d_Q_Q_rhs_map(), d_Q_difference_form(), d_Q_diffusion_coef(),
      d_Q_diffusion_coef_variable(), d_Q_is_diffusion_coef_variable(), d_Q_damping_coef(), d_Q_init(), d_Q_bc_coef(),
      d_hier_cc_data_ops(NULL), d_hier_sc_data_ops(NULL), d_sol_vecs(), d_rhs_vecs(),
      d_helmholtz_solver_type(CCPoissonSolverManager::UNDEFINED),
      d_helmholtz_precond_type(CCPoissonSolverManager::UNDEFINED), d_helmholtz_solver_db(), d_helmholtz_precond_db(),
      d_helmholtz_solvers(), d_helmholtz_rhs_ops(), d_helmholtz_solvers_need_init(), d_helmholtz_rhs_ops_need_init(),
      d_coarsest_reset_ln(-1), d_finest_reset_ln(-1)
{
    TBOX_ASSERT(!object_name.empty());
    TBOX_ASSERT(input_db);

    // Initialize object with data read from the input and restart databases.
    bool from_restart = RestartManager::getManager()->isFromRestart();
    if (from_restart) getFromRestart();
    if (input_db) getFromInput(input_db, from_restart);
    return;
} // AdvDiffHierarchyIntegrator

double AdvDiffHierarchyIntegrator::getMaximumTimeStepSizeSpecialized()
{
    double dt = d_dt_max;
    const bool initial_time = MathUtilities<double>::equalEps(d_integrator_time, d_start_time);
    for (int ln = 0; ln <= d_hierarchy->getFinestLevelNumber(); ++ln)
    {
        boost::shared_ptr<PatchLevel> level = d_hierarchy->getPatchLevel(ln);
        for (PatchLevel::iterator p = level->begin(); p != level->end(); ++p)
        {
            boost::shared_ptr<Patch> patch = *p;
            const Box& patch_box = patch->getBox();
            const Index& ilower = patch_box.lower();
            const Index& iupper = patch_box.upper();
            auto patch_geom = BOOST_CAST<CartesianPatchGeometry>(patch->getPatchGeometry());
            const double* const dx = patch_geom->getDx();
            for (std::vector<boost::shared_ptr<FaceVariable<double> > >::const_iterator cit = d_u_var.begin();
                 cit != d_u_var.end();
                 ++cit)
            {
                boost::shared_ptr<FaceVariable<double> > u_var = *cit;
                boost::shared_ptr<FaceData<double> > u_data = patch->getPatchData(u_var, getCurrentContext());
                const IntVector& u_ghost_cells = u_data->getGhostCellWidth();
                double stable_dt = std::numeric_limits<double>::max();
#if (NDIM == 2)
                ADVECT_STABLEDT_FC(dx,
                                   ilower(0),
                                   iupper(0),
                                   ilower(1),
                                   iupper(1),
                                   u_ghost_cells(0),
                                   u_ghost_cells(1),
                                   u_data->getPointer(0),
                                   u_data->getPointer(1),
                                   stable_dt);
#endif
#if (NDIM == 3)
                ADVECT_STABLEDT_FC(dx,
                                   ilower(0),
                                   iupper(0),
                                   ilower(1),
                                   iupper(1),
                                   ilower(2),
                                   iupper(2),
                                   u_ghost_cells(0),
                                   u_ghost_cells(1),
                                   u_ghost_cells(2),
                                   u_data->getPointer(0),
                                   u_data->getPointer(1),
                                   u_data->getPointer(2),
                                   stable_dt);
#endif
                dt = std::min(dt, d_cfl_max * stable_dt);
            }
        }
    }
    if (!initial_time && d_dt_growth_factor >= 1.0)
    {
        dt = std::min(dt, d_dt_growth_factor * d_dt_previous[0]);
    }
    return dt;
} // getMaximumTimeStepSizeSpecialized

void AdvDiffHierarchyIntegrator::resetHierarchyConfigurationSpecialized(
    const boost::shared_ptr<PatchHierarchy> base_hierarchy,
    const int coarsest_level,
    const int finest_level)
{
    const boost::shared_ptr<PatchHierarchy> hierarchy = base_hierarchy;
    TBOX_ASSERT(hierarchy);
    TBOX_ASSERT((coarsest_level >= 0) && (coarsest_level <= finest_level) &&
                (finest_level <= hierarchy->getFinestLevelNumber()));
    for (int ln = 0; ln <= finest_level; ++ln)
    {
        TBOX_ASSERT(hierarchy->getPatchLevel(ln));
    }
    const int finest_hier_level = hierarchy->getFinestLevelNumber();

    // Reset the Hierarchy data operations for the new hierarchy configuration.
    d_hier_cc_data_ops->setPatchHierarchy(hierarchy);
    d_hier_cc_data_ops->resetLevels(0, finest_hier_level);
    d_hier_sc_data_ops->setPatchHierarchy(hierarchy);
    d_hier_sc_data_ops->resetLevels(0, finest_hier_level);

    // Reset the interpolation operators.
    VariableDatabase* var_db = VariableDatabase::getDatabase();
    d_hier_bdry_fill_ops.resize(d_Q_var.size());
    unsigned int l = 0;
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::const_iterator cit = d_Q_var.begin();
         cit != d_Q_var.end();
         ++cit, ++l)
    {
        boost::shared_ptr<CellVariable<double> > Q_var = *cit;
        const int Q_scratch_idx = var_db->mapVariableAndContextToIndex(Q_var, getScratchContext());

        // Setup the interpolation transaction information.
        typedef HierarchyGhostCellInterpolation::InterpolationTransactionComponent InterpolationTransactionComponent;
        InterpolationTransactionComponent transaction_comp(Q_scratch_idx,
                                                           DATA_REFINE_TYPE,
                                                           USE_CF_INTERPOLATION,
                                                           DATA_COARSEN_TYPE,
                                                           BDRY_EXTRAP_TYPE,
                                                           CONSISTENT_TYPE_2_BDRY,
                                                           d_Q_bc_coef[Q_var]);

        // Initialize the interpolation operators.
        d_hier_bdry_fill_ops[l] = boost::make_shared<HierarchyGhostCellInterpolation>();
        d_hier_bdry_fill_ops[l]->initializeOperatorState(transaction_comp, d_hierarchy);
    }

    // Reset the solution and rhs vectors.
    d_sol_vecs.resize(d_Q_var.size());
    d_rhs_vecs.resize(d_Q_var.size());
    l = 0;
    const int wgt_idx = d_hier_math_ops->getCellWeightPatchDescriptorIndex();
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::const_iterator cit = d_Q_var.begin();
         cit != d_Q_var.end();
         ++cit, ++l)
    {
        boost::shared_ptr<CellVariable<double> > Q_var = *cit;
        const std::string& name = Q_var->getName();

        const int Q_scratch_idx = var_db->mapVariableAndContextToIndex(Q_var, getScratchContext());
        d_sol_vecs[l] =
            boost::make_shared<SAMRAIVectorReal<double> >(d_object_name + "::sol_vec::" + name, d_hierarchy, 0, finest_hier_level);
        d_sol_vecs[l]->addComponent(Q_var, Q_scratch_idx, wgt_idx, d_hier_cc_data_ops);

        boost::shared_ptr<CellVariable<double> > Q_rhs_var = d_Q_Q_rhs_map[Q_var];
        const int Q_rhs_scratch_idx = var_db->mapVariableAndContextToIndex(Q_rhs_var, getScratchContext());
        d_rhs_vecs[l] =
            boost::make_shared<SAMRAIVectorReal<double> >(d_object_name + "::rhs_vec::" + name, d_hierarchy, 0, finest_hier_level);
        d_rhs_vecs[l]->addComponent(Q_rhs_var, Q_rhs_scratch_idx, wgt_idx, d_hier_cc_data_ops);
    }

    // Indicate that all linear solvers must be re-initialized.
    std::fill(d_helmholtz_solvers_need_init.begin(), d_helmholtz_solvers_need_init.end(), true);
    std::fill(d_helmholtz_rhs_ops_need_init.begin(), d_helmholtz_rhs_ops_need_init.end(), true);
    d_coarsest_reset_ln = coarsest_level;
    d_finest_reset_ln = finest_level;
    return;
} // resetHierarchyConfigurationSpecialized

void AdvDiffHierarchyIntegrator::putToDatabaseSpecialized(boost::shared_ptr<Database> db)
{
    TBOX_ASSERT(db);
    db->putInteger("ADV_DIFF_HIERARCHY_INTEGRATOR_VERSION", ADV_DIFF_HIERARCHY_INTEGRATOR_VERSION);
    db->putDouble("d_cfl_max", d_cfl_max);
    db->putString("d_default_diffusion_time_stepping_type",
                  enum_to_string<TimeSteppingType>(d_default_diffusion_time_stepping_type));
    db->putString("d_default_convective_difference_form",
                  enum_to_string<ConvectiveDifferencingType>(d_default_convective_difference_form));
    return;
} // putToDatabaseSpecialized

void AdvDiffHierarchyIntegrator::registerVariables()
{
    const IntVector cell_ghosts(DIM, CELLG);
    const IntVector face_ghosts(DIM, FACEG);
    for (std::vector<boost::shared_ptr<FaceVariable<double> > >::const_iterator cit = d_u_var.begin();
         cit != d_u_var.end();
         ++cit)
    {
        boost::shared_ptr<FaceVariable<double> > u_var = *cit;
        int u_current_idx, u_new_idx, u_scratch_idx;
        registerVariable(u_current_idx,
                         u_new_idx,
                         u_scratch_idx,
                         u_var,
                         face_ghosts,
                         "CONSERVATIVE_COARSEN",
                         "CONSERVATIVE_LINEAR_REFINE",
                         d_u_fcn[u_var]);
    }
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::const_iterator cit = d_Q_var.begin();
         cit != d_Q_var.end();
         ++cit)
    {
        boost::shared_ptr<CellVariable<double> > Q_var = *cit;
        int Q_current_idx, Q_new_idx, Q_scratch_idx;
        registerVariable(Q_current_idx,
                         Q_new_idx,
                         Q_scratch_idx,
                         Q_var,
                         cell_ghosts,
                         "CONSERVATIVE_COARSEN",
                         "CONSERVATIVE_LINEAR_REFINE",
                         d_Q_init[Q_var]);
        const int Q_depth = Q_var->getDepth();
        if (d_visit_writer)
            d_visit_writer->registerPlotQuantity(Q_var->getName(), Q_depth == 1 ? "SCALAR" : "VECTOR", Q_current_idx);
    }
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::const_iterator cit = d_F_var.begin();
         cit != d_F_var.end();
         ++cit)
    {
        boost::shared_ptr<CellVariable<double> > F_var = *cit;
        int F_current_idx, F_new_idx, F_scratch_idx;
        registerVariable(F_current_idx,
                         F_new_idx,
                         F_scratch_idx,
                         F_var,
                         cell_ghosts,
                         "CONSERVATIVE_COARSEN",
                         "CONSERVATIVE_LINEAR_REFINE",
                         d_F_fcn[F_var]);
        const int F_depth = F_var->getDepth();
        if (d_visit_writer)
            d_visit_writer->registerPlotQuantity(F_var->getName(), F_depth == 1 ? "SCALAR" : "VECTOR", F_current_idx);
    }
    for (std::vector<boost::shared_ptr<SideVariable<double> > >::const_iterator cit = d_diffusion_coef_var.begin();
         cit != d_diffusion_coef_var.end();
         ++cit)
    {
        boost::shared_ptr<SideVariable<double> > D_var = *cit;
        int D_current_idx, D_new_idx, D_scratch_idx;
        registerVariable(D_current_idx,
                         D_new_idx,
                         D_scratch_idx,
                         D_var,
                         face_ghosts,
                         "CONSERVATIVE_COARSEN",
                         "CONSERVATIVE_LINEAR_REFINE",
                         d_diffusion_coef_fcn[D_var]);
    }
    for (std::vector<boost::shared_ptr<CellVariable<double> > >::const_iterator cit = d_Q_rhs_var.begin();
         cit != d_Q_rhs_var.end();
         ++cit)
    {
        boost::shared_ptr<CellVariable<double> > Q_rhs_var = *cit;
        int Q_rhs_scratch_idx;
        registerVariable(Q_rhs_scratch_idx, Q_rhs_var, cell_ghosts, getScratchContext());
    }
    for (std::vector<boost::shared_ptr<SideVariable<double> > >::const_iterator cit = d_diffusion_coef_rhs_var.begin();
         cit != d_diffusion_coef_rhs_var.end();
         ++cit)
    {
        boost::shared_ptr<SideVariable<double> > D_rhs_var = *cit;
        int D_rhs_scratch_idx;
        registerVariable(D_rhs_scratch_idx, D_rhs_var, cell_ghosts, getScratchContext());
    }
    return;
} // registerVariables

/////////////////////////////// PRIVATE //////////////////////////////////////

void AdvDiffHierarchyIntegrator::getFromInput(boost::shared_ptr<Database> db, bool is_from_restart)
{
    TBOX_ASSERT(db);

    // Read in data members from input database.
    if (!is_from_restart)
    {
        if (db->keyExists("diffusion_time_stepping_type"))
            d_default_diffusion_time_stepping_type =
                string_to_enum<TimeSteppingType>(db->getString("diffusion_time_stepping_type"));
        else if (db->keyExists("diffusion_timestepping_type"))
            d_default_diffusion_time_stepping_type =
                string_to_enum<TimeSteppingType>(db->getString("diffusion_timestepping_type"));
        else if (db->keyExists("default_diffusion_time_stepping_type"))
            d_default_diffusion_time_stepping_type =
                string_to_enum<TimeSteppingType>(db->getString("default_diffusion_time_stepping_type"));
        else if (db->keyExists("default_diffusion_timestepping_type"))
            d_default_diffusion_time_stepping_type =
                string_to_enum<TimeSteppingType>(db->getString("default_diffusion_timestepping_type"));

        if (db->keyExists("convective_difference_type"))
            d_default_convective_difference_form =
                string_to_enum<ConvectiveDifferencingType>(db->getString("convective_difference_type"));
        else if (db->keyExists("convective_difference_form"))
            d_default_convective_difference_form =
                string_to_enum<ConvectiveDifferencingType>(db->getString("convective_difference_form"));
        else if (db->keyExists("default_convective_difference_type"))
            d_default_convective_difference_form =
                string_to_enum<ConvectiveDifferencingType>(db->getString("default_convective_difference_type"));
        else if (db->keyExists("default_convective_difference_form"))
            d_default_convective_difference_form =
                string_to_enum<ConvectiveDifferencingType>(db->getString("default_convective_difference_form"));
    }

    if (db->keyExists("cfl_max"))
        d_cfl_max = db->getDouble("cfl_max");
    else if (db->keyExists("CFL_max"))
        d_cfl_max = db->getDouble("CFL_max");
    else if (db->keyExists("cfl"))
        d_cfl_max = db->getDouble("cfl");
    else if (db->keyExists("CFL"))
        d_cfl_max = db->getDouble("CFL");

    if (db->keyExists("solver_type"))
        d_helmholtz_solver_type = db->getString("solver_type");
    else if (db->keyExists("helmholtz_solver_type"))
        d_helmholtz_solver_type = db->getString("helmholtz_solver_type");
    if (db->keyExists("solver_type") || db->keyExists("helmholtz_solver_type"))
    {
        if (db->keyExists("solver_db"))
            d_helmholtz_solver_db = db->getDatabase("solver_db");
        else if (db->keyExists("helmholtz_solver_db"))
            d_helmholtz_solver_db = db->getDatabase("helmholtz_solver_db");
    }
    if (!d_helmholtz_solver_db) d_helmholtz_solver_db = boost::make_shared<MemoryDatabase>("helmholtz_solver_db");

    if (db->keyExists("precond_type"))
        d_helmholtz_precond_type = db->getString("precond_type");
    else if (db->keyExists("helmholtz_precond_type"))
        d_helmholtz_precond_type = db->getString("helmholtz_precond_type");
    if (db->keyExists("precond_type") || db->keyExists("helmholtz_precond_type"))
    {
        if (db->keyExists("precond_db"))
            d_helmholtz_precond_db = db->getDatabase("precond_db");
        else if (db->keyExists("helmholtz_precond_db"))
            d_helmholtz_precond_db = db->getDatabase("helmholtz_precond_db");
    }
    if (!d_helmholtz_precond_db) d_helmholtz_precond_db = boost::make_shared<MemoryDatabase>("helmholtz_precond_db");
    return;
} // getFromInput

void AdvDiffHierarchyIntegrator::getFromRestart()
{
    boost::shared_ptr<Database> restart_db = RestartManager::getManager()->getRootDatabase();
    boost::shared_ptr<Database> db;
    if (restart_db->isDatabase(d_object_name))
    {
        db = restart_db->getDatabase(d_object_name);
    }
    else
    {
        TBOX_ERROR(d_object_name << ":  Restart database corresponding to " << d_object_name
                                 << " not found in restart file." << std::endl);
    }
    int ver = db->getInteger("ADV_DIFF_HIERARCHY_INTEGRATOR_VERSION");
    if (ver != ADV_DIFF_HIERARCHY_INTEGRATOR_VERSION)
    {
        TBOX_ERROR(d_object_name << ":  Restart file version different than class version." << std::endl);
    }
    d_cfl_max = db->getDouble("d_cfl_max");
    d_default_diffusion_time_stepping_type =
        string_to_enum<TimeSteppingType>(db->getString("d_default_diffusion_time_stepping_type"));
    d_default_convective_difference_form =
        string_to_enum<ConvectiveDifferencingType>(db->getString("d_default_convective_difference_form"));
    return;
} // getFromRestart

//////////////////////////////////////////////////////////////////////////////

} // namespace IBAMR

//////////////////////////////////////////////////////////////////////////////
