
#include <AMReX_PlotFileUtil.H>
#include <AMReX_ParmParse.H>
#include <AMReX_Print.H>

#include "MagnonDiffusion.H"
#include "Input/BoundaryConditions/BoundaryConditions.H"
#include "Input/GeometryProperties/GeometryProperties.H"
#include "Utils/SelectWarpXUtils/WarpXUtil.H"
#include "Utils/SelectWarpXUtils/WarpXProfilerWrapper.H"
#include "Utils/eXstaticUtils/eXstaticUtil.H"
#include "Utils/FerroXUtils/FerroXUtil.H"

using namespace amrex;
using namespace MagnonDiffusion;

int main (int argc, char* argv[])
{
    amrex::Initialize(argc,argv);
    {
        c_MagnonDiffusion pMagnonDiffusion;
        pMagnonDiffusion.InitData();
        main_main(pMagnonDiffusion);
    }

    amrex::Finalize();
    return 0;
}

void main_main (c_MagnonDiffusion& rMagnonDiffusion)
{
    // What time is it now?  We'll use this to compute total run time.
    auto strt_time = amrex::second();
    
    auto& rGprop = rMagnonDiffusion.get_GeometryProperties();
    auto& geom = rGprop.geom;
    auto& ba = rGprop.ba;
    auto& dm = rGprop.dm;
    auto& is_periodic = rGprop.is_periodic;
    auto& prob_lo = rGprop.prob_lo;
    auto& prob_hi = rGprop.prob_hi;
    auto& n_cell = rGprop.n_cell;

    // read in inputs file
    InitializeMagnonDiffusionNamespace(prob_lo, prob_hi);

    // Nghost = number of ghost cells for each array
    int Nghost = 1;

    // Ncomp = number of components for each array
    int Ncomp  = 1;

    // time = starting time in the simulation
    Real time = 0.0;

    // we allocate two phi multifabs; one will store the old state, the other the new.
    MultiFab phi_old(ba, dm, Ncomp, Nghost);
    MultiFab phi_new(ba, dm, Ncomp, Nghost);

    //MultiFabs to parse robin BC coefficients
    MultiFab robin_hi_a(ba, dm, Ncomp, Nghost);
    MultiFab robin_hi_b(ba, dm, Ncomp, Nghost);
    MultiFab robin_hi_f(ba, dm, Ncomp, Nghost);
    MultiFab spin_relax_len(ba, dm, Ncomp, Nghost);
    robin_hi_a.setVal(0.);
    robin_hi_b.setVal(0.);
    robin_hi_f.setVal(0.);
    spin_relax_len.setVal(1.); //spin relaxation length. It is position dependent and  is parsed from the input file

    Initialize_Robin_Coefs(rMagnonDiffusion, geom, robin_hi_a, robin_hi_b, robin_hi_f);
 
    Initialize_Spin_Relax_Len(rMagnonDiffusion, geom, spin_relax_len);

#ifdef AMREX_USE_EB
    MultiFab Plt(ba, dm, 5, 0,  MFInfo(), *rGprop.pEB->p_factory_union);
#else    
    MultiFab Plt(ba, dm, 5, 0);
#endif

    // Initialize phi_new here
    phi_new.setVal(0.);

    MultiFab::Copy(Plt, phi_new, 0, 0, 1, 0);
    MultiFab::Copy(Plt, robin_hi_a, 0, 1, 1, 0);
    MultiFab::Copy(Plt, robin_hi_b, 0, 2, 1, 0);
    MultiFab::Copy(Plt, robin_hi_f, 0, 3, 1, 0);
    MultiFab::Copy(Plt, spin_relax_len, 0, 4, 1, 0);

    // Write a plotfile of the initial data if plot_int > 0 (plot_int was defined in the inputs file)
    if (plot_int > 0)
    {
        int n = 0;
        const std::string& pltfile = amrex::Concatenate("plt",n,5);
#ifdef AMREX_USE_EB
        EB_WriteSingleLevelPlotfile(pltfile, Plt, {"phi", "robin_a", "robin_b", "robin_f", "spin_relax_len"}, geom, time, n);
#else    
        WriteSingleLevelPlotfile(pltfile, Plt, {"phi", "robin_a", "robin_b", "robin_f", "spin_relax_len"}, geom, time, n);
#endif
    }


    for (int n = 1; n <= nsteps; ++n)
    {
        MultiFab::Copy(phi_old, phi_new, 0, 0, 1, 0);

        // new_phi = (I-dt)^{-1} * old_phi + dt
        // magnon diffusion case has updated alpha and beta coeffs
        // (a * alpha * I - b del*beta del ) phi = RHS
        advance(phi_old, phi_new, robin_hi_a, robin_hi_b, robin_hi_f, rMagnonDiffusion, geom);
        time = time + dt;

        // Tell the I/O Processor to write out which step we're doing
        amrex::Print() << "Advanced step " << n << "\n";

        // Write a plotfile of the current data (plot_int was defined in the inputs file)
        MultiFab::Copy(Plt, phi_new, 0, 0, 1, 0);

        if (plot_int > 0 && n%plot_int == 0)
        {
            const std::string& pltfile = amrex::Concatenate("plt",n,5);
#ifdef AMREX_USE_EB
            EB_WriteSingleLevelPlotfile(pltfile, Plt, {"phi", "robin_a", "robin_b", "robin_f","spin_relax_len"}, geom, time, n);
#else    
            WriteSingleLevelPlotfile(pltfile, Plt, {"phi", "robin_a", "robin_b", "robin_f", "spin_relax_len"}, geom, time, n);
#endif
        }
    }

    // Call the timer again and compute the maximum difference between the start time and stop time
    //   over all processors
    auto stop_time = amrex::second() - strt_time;
    const int IOProc = ParallelDescriptor::IOProcessorNumber();
    ParallelDescriptor::ReduceRealMax(stop_time,IOProc);

    // Tell the I/O Processor to write out the "run time"
    amrex::Print() << "Run time = " << stop_time << std::endl;
}
