#include <AMReX_LO_BCTYPES.H>
#include <AMReX_MLMG.H>
#include <AMReX_MultiFab.H> 

#include "MagnonDiffusion.H"

#include "Input/GeometryProperties/GeometryProperties.H"
#include "Utils/FerroXUtils/FerroXUtil.H"

using namespace amrex;
using namespace MagnonDiffusion;

void advance (MultiFab& phi_old,
              MultiFab& phi_new,
              MultiFab& robin_hi_a,
              MultiFab& robin_hi_b,
              MultiFab& robin_hi_f,
              c_MagnonDiffusion& rMagnonDiffusion,
              const Geometry& geom)
{
    /*
      We use an MLABecLaplacian operator:

      (ascalar*acoef - bscalar div bcoef grad) phi = RHS

      for an implicit discretization of the heat equation

      (I - div dt grad) phi^{n+1} = phi^n
     */

    BoxArray ba = phi_old.boxArray();
    DistributionMapping dmap = phi_old.DistributionMap();
    
    // assorment of solver and parallization options and parameters
    // see AMReX_MLLinOp.H for the defaults, accessors, and mutators
    LPInfo info;

    // Implicit solve using MLABecLaplacian class
    MLABecLaplacian mlabec({geom}, {ba}, {dmap}, info);

    auto& rGprop = rMagnonDiffusion.get_GeometryProperties();
#ifdef AMREX_USE_EB
    MLEBABecLap mlebabec({geom}, {ba}, {dmap}, info, {& *rGprop.pEB->p_factory_union});
#endif
    // order of stencil
    int linop_maxorder = 2;
    mlabec.setMaxOrder(linop_maxorder);

#ifdef AMREX_USE_EB
    mlebabec.setMaxOrder(linop_maxorder);
#endif

    // build array of boundary conditions needed by MLABecLaplacian
    // see Src/Boundary/AMReX_LO_BCTYPES.H for supported types
    std::array<LinOpBCType,AMREX_SPACEDIM> linop_bc_lo;
    std::array<LinOpBCType,AMREX_SPACEDIM> linop_bc_hi;

    bool is_any_robin = false;
    
    for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
    {
        // lo-side BCs
        if (bc_lo[idim] == BCType::int_dir) {
            linop_bc_lo[idim] = LinOpBCType::Periodic;
        }
        else if (bc_lo[idim] == BCType::foextrap) {
            linop_bc_lo[idim] = LinOpBCType::Neumann;
        }
        else if (bc_lo[idim] == BCType::ext_dir) {
            linop_bc_lo[idim] = LinOpBCType::Dirichlet;
        }
        else if (bc_lo[idim] == 6) { // 6 = Robin
            linop_bc_lo[idim] = LinOpBCType::Robin;
            is_any_robin = true;
        }
        else {
            amrex::Abort("Invalid bc_lo");
        }

        // hi-side BCs
        if (bc_hi[idim] == BCType::int_dir) {
            linop_bc_hi[idim] = LinOpBCType::Periodic;
        }
        else if (bc_hi[idim] == BCType::foextrap) {
            linop_bc_hi[idim] = LinOpBCType::Neumann;
        }
        else if (bc_hi[idim] == BCType::ext_dir) {
            linop_bc_hi[idim] = LinOpBCType::Dirichlet;
        }
        else if (bc_hi[idim] == 6) { // 6 = Robin
            linop_bc_hi[idim] = LinOpBCType::Robin;
            is_any_robin = true;
        }
        else {
            amrex::Abort("Invalid bc_hi");
        }
    }

    // tell the solver what the domain boundary conditions are
    mlabec.setDomainBC(linop_bc_lo, linop_bc_hi);

#ifdef AMREX_USE_EB
    mlebabec.setDomainBC(linop_bc_lo, linop_bc_hi);
#endif

    // Fill the ghost cells of each grid from the other grids
    // includes periodic domain boundaries
    phi_old.FillBoundary(geom.periodicity());

    // Fill phi ghost cells for Dirichlet or Neumann boundary conditions
    FillBoundaryPhysical(phi_old, geom);

    if (is_any_robin) {

        MultiFab robin_a(ba,dmap,1,1);
        MultiFab robin_b(ba,dmap,1,1);
        MultiFab robin_f(ba,dmap,1,1);

        // Fill phi ghost cells for Robin boundary conditions
        FillBoundaryRobin(robin_a, robin_b, robin_f, robin_hi_a, robin_hi_b, robin_hi_f, geom);

        // set the boundary conditions
        mlabec.setLevelBC(0, &phi_old, &robin_a, &robin_b, &robin_f);
        
#ifdef AMREX_USE_EB
        mlebabec.setLevelBC(0, &phi_old, &robin_a, &robin_b, &robin_f);
#endif
    } else {

        // set the boundary conditions
        mlabec.setLevelBC(0, &phi_old);

#ifdef AMREX_USE_EB
        mlebabec.setLevelBC(0, &phi_old);
#endif
    }

    // scaling factors
    Real ascalar = 1.0;
    Real bscalar = 1.0;
    mlabec.setScalars(ascalar, bscalar);
#ifdef AMREX_USE_EB
    mlebabec.setScalars(ascalar, bscalar);
#endif

    // Set up coefficient matrices
    MultiFab acoef(ba, dmap, 1, 0);

    // fill in the acoef MultiFab and load this into the solver
    acoef.setVal(1.0 + dt/tau_p); // changed for the magnon diffusion equation 
    mlabec.setACoeffs(0, acoef);
#ifdef AMREX_USE_EB
    mlebabec.setACoeffs(0, acoef);
#endif

    // bcoef lives on faces so we make an array of face-centered MultiFabs
    // then we will in face_bcoef MultiFabs and load them into the solver.
    std::array<MultiFab,AMREX_SPACEDIM> face_bcoef;
    for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
    {
        const BoxArray& ba = amrex::convert(acoef.boxArray(),
                                            IntVect::TheDimensionVector(idim));
        face_bcoef[idim].define(ba, acoef.DistributionMap(), 1, 0);
        face_bcoef[idim].setVal(dt * D_const); // changed for the magnon diffusion equation
    }
    mlabec.setBCoeffs(0, amrex::GetArrOfConstPtrs(face_bcoef));

#ifdef AMREX_USE_EB
    mlebabec.setBCoeffs(0, amrex::GetArrOfConstPtrs(face_bcoef));
#endif

    MultiFab cc_bcoef(ba, dmap, 1, 0);
    FerroX_Util::AverageFaceCenteredMultiFabToCellCenters(face_bcoef, cc_bcoef);
#ifdef AMREX_USE_EB
    int amrlev = 0;
    mlebabec.setEBDirichlet(amrlev, *rGprop.pEB->p_surf_soln_union, cc_bcoef);
#endif

    // build an MLMG solver
    MLMG mlmg(mlabec);

#ifdef AMREX_USE_EB
    MLMG ebmlmg(mlebabec);
#endif

    // set solver parameters
    int max_iter = 100;
    mlmg.setMaxIter(max_iter);
    int max_fmg_iter = 0;
    mlmg.setMaxFmgIter(max_fmg_iter);
    int verbose = 2;
    mlmg.setVerbose(verbose);
    int bottom_verbose = 0;
    mlmg.setBottomVerbose(bottom_verbose);

#ifdef AMREX_USE_EB
    ebmlmg.setMaxIter(max_iter);
    ebmlmg.setMaxFmgIter(max_fmg_iter);
    ebmlmg.setVerbose(verbose);
    ebmlmg.setBottomVerbose(bottom_verbose);
#endif

    // relative and absolute tolerances for linear solve
    const Real tol_rel = 1.e-10;
    const Real tol_abs = 0.0;

    // Solve linear system
    mlmg.solve({&phi_new}, {&phi_old}, tol_rel, tol_abs);

#ifdef AMREX_USE_EB
    ebmlmg.solve({&phi_new}, {&phi_old}, tol_rel, tol_abs);
#endif

}

