#ifndef GHM_TEST_FAULT_HOOKS_H
#define GHM_TEST_FAULT_HOOKS_H
#include <ghm/ghm.h>
typedef int (*GhmTestFault)(const char *point, GhmError *error, void *payload);
void ghm_test_fault_set(GhmTestFault hook, void *payload);
/* Cross-thread injection for the GTK harness; clear only after tasks finish. */
void ghm_test_fault_set_global(GhmTestFault hook, void *payload);
#endif
