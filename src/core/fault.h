#ifndef GHM_CORE_FAULT_H
#define GHM_CORE_FAULT_H
#include <ghm/ghm.h>
#ifdef GHM_FAULT_TESTING
int ghm_fault(const char *point, GhmError *error);
#else
static inline int ghm_fault(const char *point, GhmError *error)
{ (void)point; (void)error; return 0; }
#endif
#endif
