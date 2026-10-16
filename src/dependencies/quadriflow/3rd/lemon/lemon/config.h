// Hand-written replacement for LEMON's CMake-generated config.h.in
// (QtMeshEditor vendors LEMON header-only for QuadriFlow's network-simplex
// min-cost flow; no LP/MIP solver backends, no threading primitives).
#define LEMON_VERSION "1.3.1"
#define LEMON_HAVE_LONG_LONG 1

#define _LEMON_CPLEX 1
#define _LEMON_CLP 2
#define _LEMON_GLPK 3
#define _LEMON_SOPLEX 4
#define _LEMON_CBC 5
