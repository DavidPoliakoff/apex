#platform_malloc="-DJEMALLOC_ROOT=$HOME/install/jemalloc-3.5.1"
#platform_malloc="-DGPERFTOOLS_ROOT=$HOME/install/gperftools/2.5"
platform_malloc="-DUSE_JEMALLOC=TRUE"
platform_bfd="-DUSE_BFD=TRUE"
platform_ah="-DACTIVEHARMONY_ROOT=/usr/local/activeharmony/4.6"
platform_otf="-DOTF2_ROOT=/usr/local/otf2/2.0"
#platform_ompt="-DOMPT_ROOT=/usr/local/LLVM-ompt/Release"
platform_ompt="-DOMPT_ROOT=$HOME/src/LLVM-openmp/build-gcc-Release"
platform_mpi="-DCMAKE_C_COMPILER=mpicc -DCMAKE_CXX_COMPILER=mpicxx -DUSE_MPI=TRUE"
platform_papi="-DUSE_PAPI=TRUE -DPAPI_ROOT=/usr/local/papi/5.5.0"
platform_tau="-DUSE_TAU=TRUE -DTAU_ROOT=/usr/local/tau/git -DTAU_ARCH=x86_64 -DTAU_OPTIONS=-pthread"

parallel_build="-j 8"
