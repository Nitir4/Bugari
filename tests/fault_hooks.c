#include "fault_hooks.h"
#include "core/fault.h"
#include <stdatomic.h>
static _Thread_local GhmTestFault fault_hook;
static _Thread_local void *fault_payload;
static _Atomic(GhmTestFault) global_hook;
static _Atomic(void *) global_payload;
void ghm_test_fault_set(GhmTestFault hook, void *payload)
{ fault_hook = hook; fault_payload = payload; }
int ghm_fault(const char *point, GhmError *error)
{
    if (fault_hook != NULL) return fault_hook(point, error, fault_payload);
    GhmTestFault hook = atomic_load(&global_hook);
    return hook != NULL ? hook(point, error, atomic_load(&global_payload)) : 0;
}
void ghm_test_fault_set_global(GhmTestFault hook, void *payload)
{ atomic_store(&global_payload, payload); atomic_store(&global_hook, hook); }
