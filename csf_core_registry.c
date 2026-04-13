/*
 * csf_core_registry.c -Empty registry stub for csf_core.dll
 *
 * Core does not register any C-defined statuses itself.  All statuses
 * come from either:
 *   1. GON files loaded by csf_core_gon_loader.c (generic path)
 *   2. Extension DLLs calling CSFCore_RegisterExtension at their own
 *      DllMain time, after Core has registered the GON statuses
 *
 * This file exists because Initialize() in csf_core.c calls
 * CSF_RegisterAllStatuses() during init.  Keeping the function
 * defined here (rather than moving it into csf_core.c) leaves the
 * door open for Core-owned built-in statuses in a future version
 * without touching the main source file.
 */

#include "csf_core.h"

void CSF_RegisterAllStatuses(void)
{
    /* intentionally empty -all statuses arrive via GON or extensions */
}
