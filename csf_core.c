/*
 * csf_core.c -Custom Status Framework Core DLL
 *
 * Table-driven framework for injecting custom status effects into Mewgenics.
 * Core owns the hooks at apply_status, resolve_status, get_status_flags, and
 * primary_registration.  GON-declared custom statuses are handled end-to-end
 * by Core's generic path.  C extensions attach to named statuses via the
 * CSFCore_RegisterExtension API defined in csf_core_api.h (vtable slot
 * override), resolved from csf_core.dll by extension DLLs at init time.
 *
 * Built as csf_core.dll.  Exports the CSFCore_* symbols documented in
 * csf_core_api.h.
 */

#include "csf_core.h"
#include "csf_core_api.h"
#include <intrin.h>

/* Stringified version for singleton-guard owner string */
#define CSF_CORE_STRINGIFY_IMPL(x) #x
#define CSF_CORE_STRINGIFY(x) CSF_CORE_STRINGIFY_IMPL(x)
#define CSF_CORE_API_VERSION_STR CSF_CORE_STRINGIFY(CSF_CORE_API_VERSION)

/* User-facing display version (product version for banners) */
#define CSF_DISPLAY_VERSION "betav0.5"


/* ====================================================================
 *  Global state
 * ==================================================================== */

UINT_PTR  g_gameBase      = 0;
HMODULE   g_hSelf         = NULL;   /* our own DLL handle (from DllMain) */

/* Global mutex -see csf_core.h for the contract. */
CRITICAL_SECTION g_csfLock;
int              g_csfLockReady = 0;
static char      g_logPath[MAX_PATH];
static int       g_registryDone  = 0;

/* Active-instance flag.  Multiple csf_core.dll copies may be loaded from
 * different mod folders.  Only the first one to claim the Mewjector name
 * "csf_core/active" fully initializes -the others become inert stubs
 * whose exports exist but whose internal state is empty.  Extensions
 * look up "csf_core/active" via MJ_LookupName to find the HMODULE of
 * the one that actually has statuses loaded. */
static int       g_isActive      = 0;
static char      g_instanceToken[32];  /* "0x<hex HMODULE>" -singleton identity */

/* Mewjector API */
static MewjectorAPI s_mj;
MewjectorAPI*       g_mj          = NULL;
int                 g_hasMewjector = 0;

/* Trampolines */
static fn_attach_passive   g_originalAP  = NULL;
static fn_primary_reg      g_originalPR  = NULL;
static fn_get_status_flags g_originalGSF = NULL;
static fn_resolve_status   g_originalRS  = NULL;
static fn_instance_lookup  g_originalIL  = NULL;
fn_apply_status            g_originalAS  = NULL;

/* Registration-time vtable override (generic infrastructure) */
static void* g_pendingCustomVtable  = NULL;
static void* g_pendingParentVtable  = NULL;  /* exact vtable ptr (C-defined statuses) */
static const UINT_PTR* g_pendingDonorRvas = NULL;  /* slot RVA array (GON-defined statuses) */

/* Active custom-def idx while we are inside HookApplyStatus's redirect
 * window. -1 when no redirect is in flight, so the instance-lookup hook
 * knows to pass through. Set right before g_originalAS, cleared right
 * after. See HookInstanceLookup for the filter logic. */
static int g_pendingDefIdx = -1;

/* DLL-owned registration slots for custom type indices.
 * Max count must accommodate both Mewjector-allocated and
 * standalone-fallback index ranges. */
#define CUSTOM_INDEX_COUNT 32
static __declspec(align(16)) unsigned char g_customRegSlots[CUSTOM_INDEX_COUNT][16] = {{0}};

/* Standalone fallback counter -only used when Mewjector is absent */
static UINT_PTR g_standaloneNextId = CSF_STANDALONE_INDEX_BASE;


/* ====================================================================
 *  Instance sidecar table
 *
 *  DLL-side parallel storage keyed by instance pointer. Avoids writing
 *  into the game's 0xF0-byte pool memory entirely.
 * ==================================================================== */

static CSF_InstanceSidecar g_sidecars[CSF_MAX_INSTANCES];
static int                 g_sidecarCount = 0;

CSF_InstanceSidecar* CSF_AllocSidecar(void* instance, int statusDefIdx)
{
    int i;

    /* Reuse a dead slot first */
    for (i = 0; i < g_sidecarCount; i++)
    {
        if (g_sidecars[i].instance == NULL)
        {
            memset(&g_sidecars[i], 0, sizeof(CSF_InstanceSidecar));
            g_sidecars[i].instance       = instance;
            g_sidecars[i].entity         = *(void**)((char*)instance + 0x38);
            g_sidecars[i].status_def_idx = statusDefIdx;
            return &g_sidecars[i];
        }
    }

    /* Append a new slot */
    if (g_sidecarCount >= CSF_MAX_INSTANCES)
    {
        SLog("[CSF] WARNING: sidecar table full (%d), cannot track instance 0x%p",
             CSF_MAX_INSTANCES, instance);
        return NULL;
    }

    i = g_sidecarCount++;
    memset(&g_sidecars[i], 0, sizeof(CSF_InstanceSidecar));
    g_sidecars[i].instance       = instance;
    g_sidecars[i].entity         = *(void**)((char*)instance + 0x38);
    g_sidecars[i].status_def_idx = statusDefIdx;
    return &g_sidecars[i];
}

CSF_InstanceSidecar* CSF_GetSidecar(void* instance)
{
    int i;
    for (i = 0; i < g_sidecarCount; i++)
    {
        if (g_sidecars[i].instance == instance)
            return &g_sidecars[i];
    }
    return NULL;
}

void CSF_FreeSidecar(void* instance)
{
    int i;
    for (i = 0; i < g_sidecarCount; i++)
    {
        if (g_sidecars[i].instance == instance)
        {
            g_sidecars[i].instance = NULL;
            SLog("[CSF] Freed sidecar slot %d for instance 0x%p", i, instance);
            return;
        }
    }
}


/* ====================================================================
 *  GON config -lazy-parsed status parameter defaults
 *
 *  Deferred to first apply_status call because the GON subsystem
 *  may not be initialized at DLL load time.
 * ==================================================================== */

static CSF_StatusConfig g_statusConfigs[CSF_MAX_STATUSES];
static int              g_gonConfigParsed = 0;

/* Forward declarations -defined further down in this file. */
static CSF_StatusDef g_statusDefs[CSF_MAX_STATUSES];
static int           g_statusDefCount;

void CSF_ParseGonConfig(void)
{
    int i;

    /* DO NOT memset(g_statusConfigs) here -the Phase-4 GON loader
     * (csf_core_gon_loader.c) has already stored per-status configs
     * (category/apply_mode/icon_override) at registration time via
     * CSF_SetStatusConfig. A blanket zero here would clobber those
     * values. This function is now purely a logging pass that confirms
     * what was stored. Future param parsing belongs in the GON loader
     * where the gon doc is already in scope. */

    for (i = 0; i < g_statusDefCount; i++)
    {
        SLog("[CSF] Config for '%s': int={%d,%d,%d,%d} dbl={%.2f,%.2f,%.2f,%.2f}",
             g_statusDefs[i].name,
             g_statusConfigs[i].int_defaults[0],
             g_statusConfigs[i].int_defaults[1],
             g_statusConfigs[i].int_defaults[2],
             g_statusConfigs[i].int_defaults[3],
             g_statusConfigs[i].dbl_defaults[0],
             g_statusConfigs[i].dbl_defaults[1],
             g_statusConfigs[i].dbl_defaults[2],
             g_statusConfigs[i].dbl_defaults[3]);
    }

    g_gonConfigParsed = 1;
    SLog("[CSF] GON config parsed (%d statuses)", g_statusDefCount);
    (void)i;
}

/* Set config for a status programmatically (alternative to GON).
 * Call after CSF_RegisterStatus. */
void CSF_SetStatusConfig(int defIndex, const CSF_StatusConfig* cfg)
{
    if (defIndex < 0 || defIndex >= g_statusDefCount) return;
    g_statusConfigs[defIndex] = *cfg;
}

/* Get config for a status (used by sidecar allocation to copy defaults). */
const CSF_StatusConfig* CSF_GetStatusConfig(int defIndex)
{
    if (defIndex < 0 || defIndex >= g_statusDefCount) return NULL;
    return &g_statusConfigs[defIndex];
}


/* ====================================================================
 *  Donor vtable lookup table
 *
 *  Maps donor names to their 172-entry RVA arrays. All 303 vanilla
 *  statuses are valid donors. The table is sorted alphabetically
 *  (case-sensitive) by generate_donors.py, enabling binary search.
 *
 *  Regenerate with: python generate_donors.py <RelocateRVAs_output.txt>
 * ==================================================================== */

typedef struct {
    const char*     name;
    const UINT_PTR* rvas;
} CSF_DonorEntry;

static const CSF_DonorEntry g_donorTable[CSF_DONOR_COUNT] = {
#include "DonorLookup.inc"
};

/* Case-sensitive binary search over the sorted donor table.
 * Returns the RVA array for the named donor, or NULL if not found. */
const UINT_PTR* CSF_ResolveDonorVtable(const char* donorName)
{
    int lo, hi, mid, cmp;

    if (!donorName) return NULL;

    lo = 0;
    hi = CSF_DONOR_COUNT - 1;
    while (lo <= hi)
    {
        mid = lo + (hi - lo) / 2;
        cmp = strcmp(donorName, g_donorTable[mid].name);
        if (cmp == 0)
            return g_donorTable[mid].rvas;
        else if (cmp < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }

    SLog("[CSF] WARNING: donor '%s' not found in lookup table (%d entries)",
         donorName, CSF_DONOR_COUNT);
    return NULL;
}


/* ====================================================================
 *  Default vtable builder -automatic mix-and-match assembly
 *
 *  When a CSF_StatusDef has slot_overrides set but build_vtable is
 *  NULL, the framework calls this to build the vtable automatically.
 *
 *  Steps:
 *    1. Copy all 172 slots from the base donor (donor_vt_rva)
 *    2. Resolve each slot override:
 *       - If custom_fn is set, install it directly
 *       - Otherwise, look up source_donor's RVA array and install
 *         the slot from there
 *    3. Convert all RVAs to absolute addresses (gameBase + RVA)
 *
 *  Compatibility checking is the caller's responsibility.
 * ==================================================================== */

void CSF_DefaultBuildVtable(CSF_StatusDef* def)
{
    int i;
    const UINT_PTR* baseRvas;

    /* Resolve the base donor's RVA array */
    baseRvas = CSF_ResolveDonorVtable(def->donor_name);
    if (!baseRvas)
    {
        SLog("[CSF] ERROR: base donor '%s' not found in lookup table", def->donor_name);
        return;
    }

    /* Step 1: copy all 172 base donor slots as absolute addresses */
    for (i = 0; i < 172; i++)
        def->custom_vtable[i] = (void*)(g_gameBase + baseRvas[i]);

    /* Step 2: install generic identity overrides.
     * Slots 0, 1, 2 get custom functions so the game identifies this
     * status by its custom name and type ID, not the donor's.
     * Slot 3 STAYS as donor's -primary_reg reads slot 3
     * (getTypeInfo) from the vtable during registration, and we need
     * it to return the donor's type info for the delegation path.
     * Slot 28 gets the generic display descriptor handler. */
    def->custom_vtable[0]  = (void*)CSF_GenericGetName;
    def->custom_vtable[1]  = (void*)CSF_GenericGetTypeId;
    def->custom_vtable[2]  = (void*)CSF_GenericTypeNameCheck;
    def->custom_vtable[28] = (void*)CSF_GenericGetDisplayDesc;

    SLog("[CSF] DefaultBuildVtable: '%s' -base donor '%s', %d overrides + identity [0,1,2,28]",
         def->name, def->donor_name, def->slot_override_count);

    /* Step 3: apply each slot override.  Resolution priority:
     *   1. custom_fn  -- direct C pointer wins unconditionally
     *   2. source_donor -- copy from another donor's vtable
     *   3. behavior_name -- look up in the behavior registry and
     *                       install a dispatcher; may be deferred if
     *                       the behavior isn't registered yet (in which
     *                       case the slot keeps its donor default and
     *                       the binding is parked for the pending sweep)
     *   4. none -- warn */
    for (i = 0; i < def->slot_override_count; i++)
    {
        CSF_SlotOverride* ov = &def->slot_overrides[i];
        int slot = ov->slot_index;

        if (slot < 0 || slot >= 172)
        {
            SLog("[CSF]   WARNING: override %d has invalid slot %d, skipping", i, slot);
            continue;
        }

        if (ov->custom_fn != NULL)
        {
            /* Direct function pointer override */
            def->custom_vtable[slot] = ov->custom_fn;
            SLog("[CSF]   [%3d] <- custom fn 0x%p", slot, ov->custom_fn);
        }
        else if (ov->source_donor != NULL)
        {
            /* Donor-sourced override -resolve the donor's RVA array */
            const UINT_PTR* srcRvas = CSF_ResolveDonorVtable(ov->source_donor);
            if (srcRvas)
            {
                def->custom_vtable[slot] = (void*)(g_gameBase + srcRvas[slot]);
                SLog("[CSF]   [%3d] <- %s (RVA 0x%X)",
                     slot, ov->source_donor, (unsigned)srcRvas[slot]);
            }
            else
            {
                SLog("[CSF]   WARNING: override %d donor '%s' not found, skipping",
                     i, ov->source_donor);
            }
        }
        else if (ov->behavior_name != NULL && ov->behavior_name[0])
        {
            /* Named-behavior override.  Slot currently holds the
             * base donor's default (from step 1); CSF_ResolveBehaviorOverride
             * reads that as the donor_fn for chaining and installs the
             * appropriate dispatcher in its place.  If the behavior
             * isn't registered yet, the slot keeps the donor default
             * and the binding stays pending until
             * CSF_RegisterBehavior triggers a sweep. */
            if (!CSF_ResolveBehaviorOverride(def, ov))
            {
                SLog("[CSF]   [%3d] <- behavior '%s' (PENDING -"
                     "dispatcher not installed, slot keeps donor default)",
                     slot, ov->behavior_name);
            }
        }
        else
        {
            SLog("[CSF]   WARNING: override %d has no custom_fn, "
                 "no source_donor, and no behavior_name", i);
        }
    }
}


/* ====================================================================
 *  Per-status vtable build wrapper -factored out of Initialize so that
 *  CSF_MergeExternalGon can build vtables for statuses discovered and
 *  registered *after* the main init loop has already run.
 *
 *  Mirrors the logic in Initialize()'s per-def loop exactly:
 *    - If the def has a custom build_vtable callback, call it.
 *    - Otherwise (or additionally, if it has slot overrides), call
 *      CSF_DefaultBuildVtable which copies the donor vtable and applies
 *      the overrides.
 * ==================================================================== */
void CSF_BuildVtableForStatus(int defIdx)
{
    CSF_StatusDef* d;
    int hasOverrides;
    int needsDefault;

    if (defIdx < 0 || defIdx >= g_statusDefCount)
    {
        SLog("[CSF] BuildVtableForStatus: index %d out of range (count=%d)",
             defIdx, g_statusDefCount);
        return;
    }

    d = &g_statusDefs[defIdx];
    hasOverrides = (d->slot_overrides != NULL && d->slot_override_count > 0);
    needsDefault = hasOverrides || (d->donor_name && !d->build_vtable);

    if (d->build_vtable)
        d->build_vtable();
    if (needsDefault)
        CSF_DefaultBuildVtable(d);

    SLog("[CSF] BuildVtableForStatus: built vtable for '%s' (idx %d)",
         d->name ? d->name : "?", defIdx);
}


/* ====================================================================
 *  Status registration table  (storage forward-declared above)
 * ==================================================================== */

UINT_PTR CSF_AllocTypeIdPair(const char* statusName)
{
    if (g_hasMewjector)
        return g_mj->AllocTypeIdPair("CSF");

    /* Standalone fallback: sequential allocation */
    {
        UINT_PTR base = g_standaloneNextId;
        g_standaloneNextId += 2;
        SLog("[CSF] Standalone type ID alloc for '%s': 0x%X/0x%X",
             statusName, (unsigned)base, (unsigned)(base + 1));
        return base;
    }
}

const CSF_StatusDef* CSF_GetStatusDef(int index)
{
    if (index < 0 || index >= g_statusDefCount) return NULL;
    return &g_statusDefs[index];
}

void CSF_RegisterStatus(CSF_StatusDef* def)
{
    UINT_PTR ids;

    if (g_statusDefCount >= CSF_MAX_STATUSES)
    {
        SLog("[CSF] WARNING: status table full (%d), cannot register '%s'",
             CSF_MAX_STATUSES, def->name);
        return;
    }

    /* Allocate collision-free type IDs */
    ids = CSF_AllocTypeIdPair(def->name);
    def->type_id_leaf   = ids;
    def->type_id_parent = ids + 1;

    /* Register name for cross-mod collision detection */
    if (g_hasMewjector)
    {
        if (!g_mj->RegisterName("status", def->name, "CSF"))
        {
            SLog("[CSF] WARNING: name collision on '%s' -another mod already registered it!",
                 def->name);
            /* Continue anyway -the modder needs to know but we don't hard-fail */
        }
    }

    g_statusDefs[g_statusDefCount] = *def;
    g_statusDefCount++;
    SLog("[CSF] Registered status '%s' (donor='%s', index=%d, leaf=0x%X, parent=0x%X)",
         def->name, def->donor_name, g_statusDefCount,
         (unsigned)def->type_id_leaf, (unsigned)def->type_id_parent);
}


/* Donor vtable RVA arrays are now in the auto-generated DonorVtables.c.
 * All 303 vanilla statuses are available as donors.
 * Regenerate with: python generate_donors.py <RelocateRVAs_output.txt> */


/* ====================================================================
 *  Logging
 * ==================================================================== */

void SLog(const char* fmt, ...)
{
    FILE* f;
    va_list ap;

    /* Always write to file log */
    if (g_logPath[0] != '\0')
    {
        f = fopen(g_logPath, "a");
        if (f)
        {
            va_start(ap, fmt);
            vfprintf(f, fmt, ap);
            va_end(ap);
            fputc('\n', f);
            fclose(f);
        }
    }

    /* Also route through Mewjector shared log when available.
     * mj.Log is variadic so we need to format into a buffer first. */
    if (g_hasMewjector)
    {
        char buf[1024];
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        g_mj->Log("CSF", "%s", buf);
    }
}


/* ====================================================================
 *  Name matching -table-driven
 *
 *  Returns 0 = not custom, 1..N = index+1 into g_statusDefs
 *  (1-based so 0 means "no match" in boolean context)
 * ==================================================================== */

/* Forward declarations -defined further down */
static void CSF_EnsureMapRegistration(void);

/* ====================================================================
 *  RegistryBuilder hook -ensures MAP registration happens at the
 *  correct time (right after the vanilla status registry is built,
 *  before the display system caches).
 *
 *  The game calls registryBuilder(registryAddr) once during init to
 *  populate all 792 vanilla status MAP entries. Our hook chains to
 *  the original, then inserts custom status MAP entries.
 *
 *  This is the proper timing fix for the init_wrapper never firing
 *  (the function at RVA_INIT_CALLER is called before DLL load).
 * ==================================================================== */

static fn_void_ptr g_originalRegistryBuilder = NULL;

static void HookRegistryBuilder(void* registryAddr)
{
    SLog("[CSF] HookRegistryBuilder ENTRY -vanilla registry building...");
    g_originalRegistryBuilder(registryAddr);
    SLog("[CSF] Vanilla registry built. Running MAP registration for custom statuses...");
    CSF_EnsureMapRegistration();
    SLog("[CSF] HookRegistryBuilder COMPLETE");
}

static int WhichCustomStatus(void* classname)
{
    UINT64* p = (UINT64*)classname;
    UINT64 size = p[2];
    UINT64 cap  = p[3];
    char*  data = (cap > 15) ? *(char**)p : (char*)p;
    int i;

    {
        static int s_wcs_count = 0;
        if (s_wcs_count < 5) {
            s_wcs_count++;
            SLog("[WCS] WhichCustomStatus query #%d: '%.*s' (len=%llu, cap=%llu, g_statusDefCount=%d)",
                 s_wcs_count, (int)(size < 64 ? size : 64), data,
                 (unsigned long long)size, (unsigned long long)cap, g_statusDefCount);
        }
    }

    for (i = 0; i < g_statusDefCount; i++)
    {
        if ((int)size == g_statusDefs[i].name_len &&
            memcmp(data, g_statusDefs[i].name, size) == 0)
        {
            return i + 1;  /* 1-based index */
        }
    }
    return 0;
}

/* Same as WhichCustomStatus but takes a plain C string instead of
 * an MSVC std::string pointer. Returns 1-based index, 0 if not found. */
static int WhichCustomStatus_CStr(const char* name, int nameLen)
{
    int i;
    for (i = 0; i < g_statusDefCount; i++)
    {
        if (nameLen == g_statusDefs[i].name_len &&
            memcmp(name, g_statusDefs[i].name, nameLen) == 0)
        {
            return i + 1;
        }
    }
    return 0;
}


/* ====================================================================
 *  Hook functions -table-driven
 * ==================================================================== */

static int HookGetStatusFlags(void* nameStr)
{
    int which;

    {
        static int s_gsf_logged = 0;
        if (!s_gsf_logged) {
            s_gsf_logged = 1;
            SLog("[GSF hook] ENTRY: HookGetStatusFlags called for FIRST TIME. nameStr=0x%p", nameStr);
        }
    }

    which = WhichCustomStatus(nameStr);
    if (which)
    {
        CSF_StatusDef* def = &g_statusDefs[which - 1];
        int flags;
        char tempStr[40];
        fn_string_ctor ctor = (fn_string_ctor)(g_gameBase + RVA_STRING_CTOR);

        /* Return the DONOR's flags so the game treats this status
         * with the same capabilities (damage reports, late update,
         * etc.) as the donor.  Returning 0 would tell the game
         * this status has no behavioral slots at all. */
        memset(tempStr, 0, 40);
        ctor(tempStr, def->donor_name);
        flags = g_originalGSF(tempStr);
        /* g_originalGSF may or may not consume the string;
         * clean up the original nameStr we intercepted too. */
        {
            fn_string_cleanup cleanup =
                (fn_string_cleanup)(g_gameBase + RVA_STRING_CLEANUP);
            cleanup(nameStr);
        }

        SLog("[GSF hook] Intercepted '%s' -returning donor '%s' flags 0x%X",
             def->name, def->donor_name, flags);
        return flags;
    }
    return g_originalGSF(nameStr);
}

static void HookResolveStatus(void* nameStr, void* p2, void* p3,
                               int p4, void* p5)
{
    int which = WhichCustomStatus(nameStr);
    if (which)
    {
        CSF_StatusDef* def = &g_statusDefs[which - 1];
        char tempStr[40];
        fn_string_ctor ctor = (fn_string_ctor)(g_gameBase + RVA_STRING_CTOR);
        fn_string_cleanup cleanup =
            (fn_string_cleanup)(g_gameBase + RVA_STRING_CLEANUP);

        SLog("[RS hook] Intercepted '%s' -redirecting to '%s'",
             def->name, def->donor_name);
        cleanup(nameStr);
        memset(tempStr, 0, 40);
        ctor(tempStr, def->donor_name);
        g_originalRS(tempStr, p2, p3, p4, p5);
        return;
    }
    g_originalRS(nameStr, p2, p3, p4, p5);
}

static void HookPrimaryRegistration(void* classDesc, void* instance)
{
    if (g_pendingCustomVtable == NULL)
    {
        g_originalPR(classDesc, instance);
        return;
    }

    {
        void* currentVt = *(void**)instance;
        int isDonorVt = 0;

        /* Match the donor vtable -two strategies:
         *   1. Exact pointer match (g_pendingParentVtable, for C-defined statuses
         *      with a known vtable RVA)
         *   2. Slot-content match (g_pendingDonorRvas, for GON-defined statuses
         *      where we have the slot RVAs but not the vtable pointer RVA).
         *      Check slots 0-3 of the instance's vtable against the donor's
         *      expected values -these are the RTTI/ctor/dtor block and are
         *      unique per derived class. */
        if (g_pendingParentVtable)
            isDonorVt = (currentVt == g_pendingParentVtable);
        else if (g_pendingDonorRvas)
            isDonorVt = (((void**)currentVt)[0] == (void*)(g_gameBase + g_pendingDonorRvas[0])
                      && ((void**)currentVt)[1] == (void*)(g_gameBase + g_pendingDonorRvas[1])
                      && ((void**)currentVt)[2] == (void*)(g_gameBase + g_pendingDonorRvas[2])
                      && ((void**)currentVt)[3] == (void*)(g_gameBase + g_pendingDonorRvas[3]));

        if (!isDonorVt)
        {
            SLog("[REG hook] Base-class registration (vt=0x%p) -passing through",
                 currentVt);
            g_originalPR(classDesc, instance);
            return;
        }
    }

    /* For GON-defined statuses, the instance's type_info still has the
     * donor's type indices (slot 3 / getTypeInfo is inherited from the
     * donor, not overridden). We can't reimplement registration with
     * custom type IDs because the type_info doesn't know about them.
     * Instead, let the original primary_reg handle registration under
     * donor IDs, then swap the vtable. The vtable swap (done after
     * apply_status returns) is what makes the status behave custom. */
    if (g_pendingDonorRvas)
    {
        SLog("[REG hook] GON-defined status -delegating to original primary_reg, "
             "vtable swap will happen after factory returns");
        *(void**)instance = g_pendingCustomVtable;
        g_originalPR(classDesc, instance);
        return;
    }

    /* Parent-level registration: reimplement with DLL-owned slot
     * (C-defined statuses with custom getTypeInfo that returns custom IDs) */
    {
        typedef void* (*fn_get_type_info_t)(void*);
        fn_dynarray_append appendFn =
            (fn_dynarray_append)(g_gameBase + RVA_DYNARRAY_APPEND);
        fn_hashmap_insert hashmapFn =
            (fn_hashmap_insert)(g_gameBase + RVA_HASHMAP_INSERT);

        void*   typeInfo;
        int*    indices;
        UINT64  count;
        UINT64  i;
        void*   instanceCopy;
        char*   regArrayBase;
        char*   flagArrayBase;

        *(void**)instance = g_pendingCustomVtable;

        SLog("[REG hook] Parent-level registration -reimplementing with DLL-owned slot");

        {
            void** vtable = *(void***)instance;
            fn_get_type_info_t getTypeInfo = (fn_get_type_info_t)vtable[3];
            typeInfo = getTypeInfo(instance);
        }

        indices = (int*)typeInfo;
        count   = *(UINT64*)((char*)typeInfo + 0x40);

        regArrayBase  = *(char**)((char*)classDesc + 0x18);
        flagArrayBase = *(char**)((char*)classDesc + 0x20);

        SLog("  type_info=0x%p  count=%llu", typeInfo, (unsigned long long)count);

        for (i = 0; i < count; i++)
        {
            int type_idx = indices[i];
            int isCustom = 0;
            int j;
            instanceCopy = instance;

            /* Check if this type_idx belongs to any registered custom status.
             * With Mewjector, IDs are dynamically allocated so we can't use
             * a simple range check -we must match against each status's
             * allocated leaf and parent IDs. */
            for (j = 0; j < g_statusDefCount; j++)
            {
                UINT_PTR leaf   = g_statusDefs[j].type_id_leaf;
                UINT_PTR parent = g_statusDefs[j].type_id_parent;

                if ((UINT_PTR)type_idx == leaf || (UINT_PTR)type_idx == parent)
                {
                    int slotIdx = (int)((UINT_PTR)type_idx - leaf);
                    /* Use a DLL-side slot keyed by (status_index * 2 + offset) */
                    int regSlot = j * 2 + slotIdx;
                    if (regSlot < CUSTOM_INDEX_COUNT)
                    {
                        SLog("  index[%llu] = 0x%X -> DLL slot[%d] (status '%s')",
                             (unsigned long long)i, type_idx, regSlot,
                             g_statusDefs[j].name);
                        appendFn(g_customRegSlots[regSlot], &instanceCopy);
                    }
                    else
                    {
                        SLog("  index[%llu] = 0x%X -> DLL slot overflow! (status '%s')",
                             (unsigned long long)i, type_idx, g_statusDefs[j].name);
                    }
                    isCustom = 1;
                    break;
                }
            }

            if (!isCustom)
            {
                char* slot = regArrayBase + (long long)type_idx * 0x10;
                appendFn(slot, &instanceCopy);
                *(flagArrayBase + (long long)type_idx * 0x10 + 8) = 1;
            }
        }

        instanceCopy = instance;
        hashmapFn((char*)classDesc + 0x488, &instanceCopy);

        SLog("  Registration complete -%llu indices processed", (unsigned long long)count);
    }
}

static void* HookApplyStatus(void* name, void* owner, int stacks, void* gonObj,
                               void* source, void* ctx6, char flag7, char flag8)
{
    int which;

    {
        static int s_as_logged = 0;
        if (!s_as_logged) {
            s_as_logged = 1;
            SLog("[AS hook] ENTRY: HookApplyStatus called for FIRST TIME. name=0x%p owner=0x%p", name, owner);
        }
    }

    /* Ensure MAP registration has happened */
    CSF_EnsureMapRegistration();

    /* Lazy-init GON config on first status application.
     * By this point the GON subsystem is guaranteed to be live. */
    if (!g_gonConfigParsed)
        CSF_ParseGonConfig();

    which = WhichCustomStatus(name);

    if (which)
    {
        #define CSF_SNAP_MAX 16
        CSF_StatusDef* def = &g_statusDefs[which - 1];
        int  defIdx = which - 1;
        void* result;
        char tempStr[40];
        fn_string_ctor ctor = (fn_string_ctor)(g_gameBase + RVA_STRING_CTOR);
        fn_string_dtor dtor = (fn_string_dtor)(g_gameBase + RVA_STRING_DTOR);
        const CSF_StatusConfig* cfg = CSF_GetStatusConfig(defIdx);
        int   apply_mode = (cfg ? cfg->int_defaults[1] : -1);
        void* snap_inst [CSF_SNAP_MAX];
        int   snap_stack[CSF_SNAP_MAX];
        int   snap_count = 0;
        int   i;

        /* -- Snapshot any pre-existing sidecar instances for this
         * def so we can recover the pre-apply stack count.
         * The donor's per-status inner handler (e.g. Doomed's
         * MIN-semantics FUN_1405db420) overrides the stack count
         * with its own rules, ignoring MAP1. For accumulate mode
         * we post-process and restore stacks = pre + new below. */
        if (apply_mode == 0)
        {
            for (i = 0; i < g_sidecarCount && snap_count < CSF_SNAP_MAX; i++)
            {
                if (g_sidecars[i].instance != NULL &&
                    g_sidecars[i].status_def_idx == defIdx)
                {
                    snap_inst [snap_count] = g_sidecars[i].instance;
                    snap_stack[snap_count] = *(int*)((char*)g_sidecars[i].instance + 0x5C);
                    snap_count++;
                }
            }
        }

        SLog("[AS hook] Intercepted '%s' -- redirecting to %s (flag7=%d stacks=%d apply_mode=%d snap=%d)",
             def->name, def->donor_name, (int)flag7, stacks, apply_mode, snap_count);

        memset(tempStr, 0, 40);
        ctor(tempStr, def->donor_name);

        /* Set up pending vtable for HookPrimaryRegistration.
         * If apply_status creates a new instance, the PR hook swaps
         * the vtable. If it accumulates into an existing one, the
         * vtable is already ours from the first apply. */
        g_pendingCustomVtable  = def->custom_vtable;
        g_pendingDonorRvas     = NULL;
        if (def->donor_vt_rva)
            g_pendingParentVtable = (void*)(g_gameBase + def->donor_vt_rva);
        else {
            g_pendingParentVtable = NULL;
            g_pendingDonorRvas    = CSF_ResolveDonorVtable(def->donor_name);
        }

        /* Pass the caller's flags through unchanged. The game's inner
         * per-status handlers (e.g. FUN_1405db420 for Doomed) have
         * hard-coded per-status semantics; MAP1 is NOT consulted by
         * them. We therefore post-process the stack count below for
         * accumulate-mode statuses. */

        /* Arm the instance-lookup hook so it filters out cross-custom
         * collisions on the shared donor. Cleared in the finally-style
         * block below regardless of how g_originalAS returns. */
        g_pendingDefIdx = defIdx;

        result = g_originalAS(tempStr, owner, stacks, gonObj, source,
                               ctx6, flag7, flag8);

        g_pendingDefIdx        = -1;
        g_pendingCustomVtable  = NULL;
        g_pendingParentVtable  = NULL;
        g_pendingDonorRvas     = NULL;

        dtor(tempStr);

        if (result)
        {
            /* -- Prong 2a: DISABLED -CRASHES THE GAME --
             *
             * Writing an SSO string to +0x88 causes an access violation
             * (RVA 0x765A10, reading RDI=0xE = strlen of the name we
             * wrote).  The +0x88 SSO's capacity field at +0xA0 overlaps
             * with a different game field, and the game reads our cap=15
             * as data for that field, causing a crash.
             *
             * Conclusion: +0x88 on the instance returned by apply_status
             * is NOT where the portrait HUD reads the display name.
             * The cap=0 state is intentional (uninitialized / unused).
             * The display name "Bleed" comes from somewhere else,
             * likely a type-level lookup table, not an instance field.
             *
             * Next diagnostic: read/write breakpoint on a VANILLA Bleed
             * instance's +0x88 to confirm whether the portrait HUD
             * reads from instance +0x88 at all, or uses a different path.
             */
#if 0  /* DO NOT ENABLE -causes ACCESS VIOLATION */
            {
                char* nameSSO = (char*)result + 0x88;
                unsigned long long nameCap =
                    *(unsigned long long*)(nameSSO + 24);
                if (nameCap > 0)
                    dtor(nameSSO);
                else
                    memset(nameSSO, 0, 32);
                ctor(nameSSO, def->name);
            }
#endif

            /* Ensure vtable is ours. Redundant if PR hook already
             * swapped it on creation, but harmless and needed if the
             * game returned an existing instance (accumulate path). */
            *(void**)result = def->custom_vtable;

            /* Only fire on_instance_created for genuinely new instances.
             * If the sidecar already exists, this was an accumulation. */
            if (CSF_GetSidecar(result) == NULL)
            {
                SLog("  New instance 0x%p -- setting %s vtable", result, def->name);
                if (def->on_instance_created)
                    def->on_instance_created(result);
            }
            else
            {
                /* Pre-existing instance (donor handler returned the
                 * same pointer). If apply_mode is accumulate, the
                 * donor handler may have clobbered the stack count
                 * (e.g. Doomed uses MIN semantics). Restore proper
                 * accumulate semantics: stacks = pre + new. */
                if (apply_mode == 0)
                {
                    int matched = 0;
                    for (i = 0; i < snap_count; i++)
                    {
                        if (snap_inst[i] == result)
                        {
                            int post = *(int*)((char*)result + 0x5C);
                            int want = snap_stack[i] + stacks;
                            *(int*)((char*)result + 0x5C) = want;
                            SLog("  Accumulate fix-up 0x%p: pre=%d +new=%d -> %d (donor set %d)",
                                 result, snap_stack[i], stacks, want, post);
                            matched = 1;
                            break;
                        }
                    }
                    if (!matched)
                    {
                        SLog("  Accumulated 0x%p (no snapshot match -- stacks unchanged)",
                             result);
                    }
                }
                else
                {
                    SLog("  Existing 0x%p (apply_mode=%d, donor handler left as-is, stacks +%d)",
                         result, apply_mode, stacks);
                }
            }
        }
        else
        {
            SLog("  apply_status returned NULL for %s redirect.", def->donor_name);
        }

        return result;
        #undef CSF_SNAP_MAX
    }

    /* -- Non-custom status: call hook extensions --------------------
     * Status definitions that need to intercept ALL status applications
     * (e.g. a collector that mirrors events to allies) register a
     * hook_apply_status_ext callback. We call all of them here. */
    {
        int i;
        for (i = 0; i < g_statusDefCount; i++)
        {
            if (g_statusDefs[i].hook_apply_status_ext)
            {
                g_statusDefs[i].hook_apply_status_ext(
                    name, owner, stacks, gonObj, source);
            }
        }
    }

    return g_originalAS(name, owner, stacks, gonObj, source, ctx6, flag7, flag8);
}

static void CSF_EnsureMapRegistration(void)
{
    int i;
    if (g_registryDone) return;
    g_registryDone = 1;

    SLog("[CSF] WARNING: Init wrapper never fired -running MAP registration as fallback");
    for (i = 0; i < g_statusDefCount; i++)
    {
        if (g_statusDefs[i].register_in_maps)
        {
            SLog("[CSF] Fallback MAP registration for '%s'", g_statusDefs[i].name);
            g_statusDefs[i].register_in_maps();
        }
    }
    SLog("[CSF] Fallback MAP registration complete (%d statuses)", g_statusDefCount);
}

static void* HookAttachPassive(void* classname, void* owner,
                                void* params, int treat_as_passive)
{
    int which;

    {
        static int s_ap_logged = 0;
        if (!s_ap_logged) {
            s_ap_logged = 1;
            SLog("[AP hook] ENTRY: HookAttachPassive called for FIRST TIME. classname=0x%p owner=0x%p",
                 classname, owner);
        }
    }

    /* Ensure MAP registration has happened before any status creation */
    CSF_EnsureMapRegistration();

    which = WhichCustomStatus(classname);

    if (which)
    {
        CSF_StatusDef* def = &g_statusDefs[which - 1];
        void* result;
        char tempStr[40];
        fn_string_ctor ctor = (fn_string_ctor)(g_gameBase + RVA_STRING_CTOR);
        fn_string_dtor dtor = (fn_string_dtor)(g_gameBase + RVA_STRING_DTOR);

        SLog("[AP hook] Intercepted '%s' -redirecting to %s  owner=0x%p",
             def->name, def->donor_name, owner);

        memset(tempStr, 0, 40);
        ctor(tempStr, def->donor_name);

        g_pendingCustomVtable  = def->custom_vtable;
        g_pendingDonorRvas     = NULL;
        if (def->donor_vt_rva)
            g_pendingParentVtable = (void*)(g_gameBase + def->donor_vt_rva);
        else {
            g_pendingParentVtable = NULL;
            g_pendingDonorRvas    = CSF_ResolveDonorVtable(def->donor_name);
        }
        result = g_originalAP(tempStr, owner, params, treat_as_passive);
        g_pendingCustomVtable  = NULL;
        g_pendingParentVtable  = NULL;
        g_pendingDonorRvas     = NULL;

        dtor(tempStr);

        if (result)
        {
            *(void**)result = def->custom_vtable;
            ((unsigned char*)result)[0xE9] = 0;
            SLog("  %s created at 0x%p", def->name, result);

            if (def->on_instance_created)
                def->on_instance_created(result);
        }

        return result;
    }

    return g_originalAP(classname, owner, params, treat_as_passive);
}



/* ====================================================================
 *  Hook -instance_lookup (RVA 0x961B60)
 *
 *  PURPOSE
 *  -------
 *  Disambiguate two custom statuses cloned from the same donor on the
 *  same bearer. The game's per-status-name inner handlers call this
 *  helper to ask "does the bearer already have a status whose type_info
 *  contains this hash?". Because our custom clones inherit the donor's
 *  type_info, all of them match the donor's hash, so the first instance
 *  on a bearer shadows every subsequent apply of a different custom
 *  status on the same donor (silent vtable clobber in HookApplyStatus's
 *  existing-instance branch).
 *
 *  Fix: while HookApplyStatus is mid-redirect (g_pendingDefIdx >= 0),
 *  post-filter the output buffer. Any instance whose sidecar carries a
 *  different status_def_idx is removed from the buffer. From the per-
 *  status-name handler's perspective, that bearer has no matching
 *  instance, so it takes the "create new" branch instead of folding
 *  into the sibling custom status. Vanilla statuses (no sidecar) pass
 *  through unchanged.
 *
 *  Buffer layout (from decomp of FUN_140961b60):
 *      +0x00  uint32   capacity
 *      +0x04  uint32   count
 *      +0x08  void**   data   (array of instance pointers)
 *
 *  Outside the redirect window g_pendingDefIdx is -1 and the hook is a
 *  pass-through to the original. All 303 vanilla callers of the helper
 *  (unrelated statuses doing their own dedupe, broadcast iterations,
 *  etc.) still see the unfiltered output buffer.
 * ==================================================================== */

typedef struct CSF_LookupOutBuf {
    unsigned int capacity;
    unsigned int count;
    void**       data;
} CSF_LookupOutBuf;

static void* HookInstanceLookup(void* container, void* out_raw, int type_hash)
{
    void* result = g_originalIL(container, out_raw, type_hash);

    if (g_pendingDefIdx >= 0 && out_raw)
    {
        CSF_LookupOutBuf* buf = (CSF_LookupOutBuf*)out_raw;
        if (buf->count > 0 && buf->data)
        {
            unsigned int write = 0;
            unsigned int read;
            unsigned int orig = buf->count;

            for (read = 0; read < buf->count; read++)
            {
                void* inst = buf->data[read];
                const CSF_InstanceSidecar* sc = CSF_GetSidecar(inst);

                /* Hide custom instances whose def idx does not match the
                 * pending apply. Vanilla instances (no sidecar) and
                 * same-def customs pass through. */
                if (sc && sc->status_def_idx != g_pendingDefIdx)
                    continue;

                if (write != read)
                    buf->data[write] = buf->data[read];
                write++;
            }

            if (write != orig)
            {
                buf->count = write;
                SLog("[IL hook] filtered %u -> %u entries "
                     "(pending defIdx=%d, type_hash=0x%X)",
                     orig, write, g_pendingDefIdx, type_hash);
            }
        }
    }

    return result;
}


/* ====================================================================
 *  Hook -glaiel::get_status_icon (RVA 0x492500)
 *
 *  PURPOSE
 *  -------
 *  Intercept status icon queries and patch the returned icon index
 *  for custom statuses that have modder-supplied icon overrides.
 *
 *  ABI (MSVC x64, verified from prologue bytes at 0x492500)
 *  --------------------------------------------------------
 *      RCX = out          StatusIconInfo* (hidden 32B struct return)
 *      RDX = name         std::string* (MSVC SSO layout)
 *      R8B = allow_missing char (bool)
 *      returns RAX = RCX
 *
 *  Prologue (15 bytes, no RIP-relative -safe for memcpy trampoline):
 *      48 89 5c 24 18       MOV  [RSP+0x18], RBX
 *      48 89 54 24 10       MOV  [RSP+0x10], RDX
 *      55                   PUSH RBP
 *      56                   PUSH RSI
 *      57                   PUSH RDI
 *      41 54                PUSH R12
 *
 *  MSVC std::string SSO layout (32 bytes total)
 *  --------------------------------------------
 *      [ 0..15]  inline buffer (16 chars) OR 8-byte heap ptr + 8 pad
 *      [16..23]  size (uint64)
 *      [24..31]  capacity (uint64) -15 = inline, >15 = heap
 * ==================================================================== */

typedef void* (*fn_get_status_icon)(void* out, void* nameStr, int allow_missing);
static fn_get_status_icon g_originalGSI = NULL;

/* Read an MSVC SSO std::string into a null-terminated C buffer.
 * Returns the number of bytes written (not including terminator). */
static int CSF_ReadSSO(const void* sso, char* outBuf, size_t outSize)
{
    const unsigned char* p;
    unsigned long long size;
    unsigned long long cap;
    const char* src;
    size_t n;

    if (outBuf == NULL || outSize == 0)
        return 0;
    outBuf[0] = '\0';
    if (sso == NULL)
        return 0;

    p = (const unsigned char*)sso;
    size = *(const unsigned long long*)(p + 16);
    cap  = *(const unsigned long long*)(p + 24);

    /* cap>15 means heap-allocated: the first 8 bytes are a char* */
    if (cap > 15)
        src = *(const char* const*)(p + 0);
    else
        src = (const char*)(p + 0);

    if (src == NULL)
        return 0;

    n = (size_t)size;
    if (n >= outSize) n = outSize - 1;
    /* Defensive: if size claims >= 2GB something is terribly wrong */
    if (size > 0x10000ULL) n = 0;
    if (n > 0)
        memcpy(outBuf, src, n);
    outBuf[n] = '\0';
    return (int)n;
}

/* Hash a C string with FNV1a-64 (same algorithm glaiel uses to key
 * the DAT_1413B1910 icon table -not that we need to collide with it,
 * just a fast uniform hash for our log-throttle bucket.) */
static unsigned long long CSF_Fnv1a64(const char* s)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    if (s == NULL) return h;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void* HookGetStatusIcon(void* out, void* nameStr, int allow_missing)
{
    char nameBuf[64];
    int  nameLen;
    int  which;

    /* Read the SSO name string that the game (or portrait HUD) is
     * querying. If this name matches a registered CSF custom status
     * that has an icon override, we re-query under the DONOR name to
     * get the correct base StatusIconInfo, then patch the icon field
     * with the modder's override. This is the permanent fix for the
     * icon bug: it doesn't matter whether the caller is slot 28,
     * the portrait HUD, or any other code path -every query for a
     * custom name gets the right icon. */
    nameLen = CSF_ReadSSO(nameStr, nameBuf, sizeof(nameBuf));

    which = WhichCustomStatus_CStr(nameBuf, nameLen);
    if (which)
    {
        const CSF_StatusDef* def = CSF_GetStatusDef(which - 1);
        const CSF_StatusConfig* cfg = CSF_GetStatusConfig(which - 1);
        int iconOverride = (cfg ? cfg->int_defaults[2] : -1);

        if (def && def->donor_name)
        {
            /* Build a temporary std::string holding the DONOR name
             * and query the original get_status_icon with it. This
             * returns the donor's full StatusIconInfo (icon index,
             * category, flags, stacks sentinel, etc.) which is correct
             * for everything except the icon index itself. */
            char tempStr[40];
            fn_string_ctor ctor = (fn_string_ctor)(g_gameBase + RVA_STRING_CTOR);
            fn_string_dtor dtor = (fn_string_dtor)(g_gameBase + RVA_STRING_DTOR);
            void* result;

            memset(tempStr, 0, 40);
            ctor(tempStr, def->donor_name);

            result = g_originalGSI(out, tempStr, allow_missing);

            dtor(tempStr);

            /* Patch the icon field if the modder specified an override.
             * iconOverride == -1 means "inherit donor icon" (no patch).
             * This is the single, stateless fix: no budget counters,
             * no thread-of-control tracking, no scope leaks. */
            if (iconOverride >= 0 && out != NULL)
            {
                int* out_icon = (int*)((char*)out + 0x00);

                {
                    static int s_gsiPatchLogged = 0;
                    if (s_gsiPatchLogged < 3) {
                        s_gsiPatchLogged++;
                        SLog("[GSI] Custom icon patch: '%s' -> donor '%s', icon %d -> %d",
                             def->name, def->donor_name, *out_icon, iconOverride);
                    }
                }

                *out_icon = iconOverride;
            }

            return result;
        }

        /* def exists but donor_name is NULL -shouldn't happen, but
         * fall through to the default query path below. */
    }

    return g_originalGSI(out, nameStr, allow_missing);
}


/* ====================================================================
 *  Hook installation -Mewjector path
 *
 *  Uses mj.InstallHook for priority-based hook chaining. Each hook
 *  gets a trampoline that chains to the next hook (or the original
 *  game function if last in the chain).
 * ==================================================================== */

static int InstallHooks_Mewjector(void)
{
    int hookCount = 0;
    void* trampoline = NULL;

    /* Hook 1: apply_status -priority 5 (before ExposeCatData at 20)
     * CSF must intercept before cond_dispatch so the donor name swap
     * happens before conditional evaluation sees the name. */
    trampoline = NULL;
    if (g_mj->InstallHook(RVA_APPLY_STATUS, AS_STOLEN_BYTES,
                           (void*)HookApplyStatus, &trampoline, 5, "CSF")) {
        g_originalAS = (fn_apply_status)trampoline;
        hookCount++;
        SLog("  [MJ] apply_status hook installed (priority 5)");
    } else {
        SLog("  [MJ] FAILED: apply_status hook");
    }

    /* Hook 2: attach_passive -priority 10 (no known contention) */
    trampoline = NULL;
    if (g_mj->InstallHook(RVA_ATTACH_PASSIVE, AP_STOLEN_BYTES,
                           (void*)HookAttachPassive, &trampoline, 10, "CSF")) {
        g_originalAP = (fn_attach_passive)trampoline;
        hookCount++;
        SLog("  [MJ] attach_passive hook installed (priority 10)");
    } else {
        SLog("  [MJ] FAILED: attach_passive hook");
    }

    /* Hook 3: get_status_flags -priority 10 */
    trampoline = NULL;
    if (g_mj->InstallHook(RVA_GET_STATUS_FLAGS, GSF_STOLEN_BYTES,
                           (void*)HookGetStatusFlags, &trampoline, 10, "CSF")) {
        g_originalGSF = (fn_get_status_flags)trampoline;
        hookCount++;
        SLog("  [MJ] get_status_flags hook installed (priority 10)");
    } else {
        SLog("  [MJ] FAILED: get_status_flags hook");
    }

    /* Hook 4: resolve_status -priority 10 */
    trampoline = NULL;
    if (g_mj->InstallHook(RVA_RESOLVE_STATUS, RS_STOLEN_BYTES,
                           (void*)HookResolveStatus, &trampoline, 10, "CSF")) {
        g_originalRS = (fn_resolve_status)trampoline;
        hookCount++;
        SLog("  [MJ] resolve_status hook installed (priority 10)");
    } else {
        SLog("  [MJ] FAILED: resolve_status hook");
    }

    /* Hook 5: primary_registration -priority 10 */
    trampoline = NULL;
    if (g_mj->InstallHook(RVA_PRIMARY_REG, PR_STOLEN_BYTES,
                           (void*)HookPrimaryRegistration, &trampoline, 10, "CSF")) {
        g_originalPR = (fn_primary_reg)trampoline;
        hookCount++;
        SLog("  [MJ] primary_registration hook installed (priority 10)");
    } else {
        SLog("  [MJ] FAILED: primary_registration hook");
    }

    /* Hook 6: registry_builder -priority 1 (must run first, before any
     * other mod touches the registry). MAP registration must happen right after vanilla registry builds,
     * before the display system caches anything. */
    {
        unsigned char* rbBytes = (unsigned char*)(g_gameBase + RVA_REGISTRY_BUILDER);
        SLog("  [MJ] registry_builder prologue: %02X %02X %02X %02X %02X %02X",
             rbBytes[0], rbBytes[1], rbBytes[2], rbBytes[3], rbBytes[4], rbBytes[5]);
    }
    trampoline = NULL;
    if (g_mj->InstallHook(RVA_REGISTRY_BUILDER, RB_STOLEN_BYTES,
                           (void*)HookRegistryBuilder, &trampoline, 1, "CSF")) {
        g_originalRegistryBuilder = (fn_void_ptr)trampoline;
        hookCount++;
        SLog("  [MJ] registry_builder hook installed (priority 1)");
    } else {
        SLog("  [MJ] FAILED: registry_builder hook -will rely on fallback MAP registration");
    }

    /* Hook 7: glaiel::get_status_icon -custom icon override support.
     * priority 20 (runs after any mod that might want to rewrite the
     * name string in-flight -not that we expect any, but higher
     * priority = later in the chain = we see the final name that
     * actually reaches the icon table).
     *
     * See big comment block above HookGetStatusIcon for ABI and
     * rationale. This hook intercepts icon lookups for custom statuses
     * and patches the returned icon index with any modder-supplied
     * override. */
    trampoline = NULL;
    if (g_mj->InstallHook(RVA_DISPLAY_LOOKUP, GSI_STOLEN_BYTES,
                           (void*)HookGetStatusIcon, &trampoline, 20, "CSF")) {
        g_originalGSI = (fn_get_status_icon)trampoline;
        hookCount++;
        SLog("  [MJ] get_status_icon hook installed (priority 20) [custom icon override]");
    } else {
        SLog("  [MJ] FAILED: get_status_icon hook -custom icon override will not work");
    }

    /* Hook 8: instance_lookup -cross-custom collision filter. Priority
     * 10; only active when g_pendingDefIdx is set (i.e. inside our
     * apply_status redirect window), pass-through otherwise. */
    trampoline = NULL;
    if (g_mj->InstallHook(RVA_INSTANCE_LOOKUP, IL_STOLEN_BYTES,
                           (void*)HookInstanceLookup, &trampoline, 10, "CSF")) {
        g_originalIL = (fn_instance_lookup)trampoline;
        hookCount++;
        SLog("  [MJ] instance_lookup hook installed (priority 10) [cross-custom collision filter]");
    } else {
        SLog("  [MJ] FAILED: instance_lookup hook -cross-custom collision on shared donor will silently break");
    }

    SLog("  [MJ] %d/8 hooks installed via Mewjector", hookCount);
    return hookCount;
}


/* ====================================================================
 *  Hook installation -Raw trampoline fallback
 *
 *  Used only when Mewjector is absent (CSF loaded standalone via
 *  ASI loader). Writes JMP-ABS patches directly into game code.
 *  WARNING: Not compatible with other mods hooking the same RVAs.
 * ==================================================================== */

/* Helper: allocate a trampoline that executes the stolen prologue
 * bytes then jumps back to target + stolenBytes */
static void* RawTrampoline(UINT_PTR target, int stolenBytes)
{
    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;

    memcpy(tramp, (void*)target, stolenBytes);
    tramp[stolenBytes + 0] = 0xFF;  /* JMP [rip+0] */
    tramp[stolenBytes + 1] = 0x25;
    tramp[stolenBytes + 2] = 0x00;
    tramp[stolenBytes + 3] = 0x00;
    tramp[stolenBytes + 4] = 0x00;
    tramp[stolenBytes + 5] = 0x00;
    *(UINT_PTR*)(tramp + stolenBytes + 6) = target + stolenBytes;

    return tramp;
}

/* Helper: patch the prologue to JMP-ABS to hookFn */
static int RawPatchPrologue(UINT_PTR target, void* hookFn, int stolenBytes)
{
    unsigned char* p = (unsigned char*)target;
    DWORD oldProt;
    int i;

    if (!VirtualProtect((LPVOID)target, stolenBytes + 1,
                         PAGE_EXECUTE_READWRITE, &oldProt))
        return 0;

    p[0] = 0xFF; p[1] = 0x25;  /* JMP [rip+0] */
    p[2] = 0x00; p[3] = 0x00; p[4] = 0x00; p[5] = 0x00;
    *(UINT_PTR*)(p + 6) = (UINT_PTR)hookFn;

    /* NOP any remaining stolen bytes */
    for (i = 14; i < stolenBytes; i++)
        p[i] = 0x90;

    VirtualProtect((LPVOID)target, stolenBytes + 1, oldProt, &oldProt);
    return 1;
}

static int InstallHooks_Raw(void)
{
    int hookCount = 0;
    void* tramp;

    SLog("  [RAW] Mewjector not available -using raw trampoline hooking");
    SLog("  [RAW] WARNING: This is incompatible with other mods hooking the same functions!");

    /* apply_status */
    tramp = RawTrampoline(g_gameBase + RVA_APPLY_STATUS, AS_STOLEN_BYTES);
    if (tramp && RawPatchPrologue(g_gameBase + RVA_APPLY_STATUS,
                                   (void*)HookApplyStatus, AS_STOLEN_BYTES)) {
        g_originalAS = (fn_apply_status)tramp;
        hookCount++;
        SLog("  [RAW] apply_status hook installed");
    } else {
        SLog("  [RAW] FAILED: apply_status hook");
    }

    /* attach_passive */
    tramp = RawTrampoline(g_gameBase + RVA_ATTACH_PASSIVE, AP_STOLEN_BYTES);
    if (tramp && RawPatchPrologue(g_gameBase + RVA_ATTACH_PASSIVE,
                                   (void*)HookAttachPassive, AP_STOLEN_BYTES)) {
        g_originalAP = (fn_attach_passive)tramp;
        hookCount++;
        SLog("  [RAW] attach_passive hook installed");
    } else {
        SLog("  [RAW] FAILED: attach_passive hook");
    }

    /* get_status_flags */
    tramp = RawTrampoline(g_gameBase + RVA_GET_STATUS_FLAGS, GSF_STOLEN_BYTES);
    if (tramp && RawPatchPrologue(g_gameBase + RVA_GET_STATUS_FLAGS,
                                   (void*)HookGetStatusFlags, GSF_STOLEN_BYTES)) {
        g_originalGSF = (fn_get_status_flags)tramp;
        hookCount++;
        SLog("  [RAW] get_status_flags hook installed");
    } else {
        SLog("  [RAW] FAILED: get_status_flags hook");
    }

    /* resolve_status */
    tramp = RawTrampoline(g_gameBase + RVA_RESOLVE_STATUS, RS_STOLEN_BYTES);
    if (tramp && RawPatchPrologue(g_gameBase + RVA_RESOLVE_STATUS,
                                   (void*)HookResolveStatus, RS_STOLEN_BYTES)) {
        g_originalRS = (fn_resolve_status)tramp;
        hookCount++;
        SLog("  [RAW] resolve_status hook installed");
    } else {
        SLog("  [RAW] FAILED: resolve_status hook");
    }

    /* primary_registration */
    tramp = RawTrampoline(g_gameBase + RVA_PRIMARY_REG, PR_STOLEN_BYTES);
    if (tramp && RawPatchPrologue(g_gameBase + RVA_PRIMARY_REG,
                                   (void*)HookPrimaryRegistration, PR_STOLEN_BYTES)) {
        g_originalPR = (fn_primary_reg)tramp;
        hookCount++;
        SLog("  [RAW] primary_registration hook installed");
    } else {
        SLog("  [RAW] FAILED: primary_registration hook");
    }

    /* get_status_icon -custom icon override support (see HookGetStatusIcon
     * comment block for rationale and ABI). */
    tramp = RawTrampoline(g_gameBase + RVA_DISPLAY_LOOKUP, GSI_STOLEN_BYTES);
    if (tramp && RawPatchPrologue(g_gameBase + RVA_DISPLAY_LOOKUP,
                                   (void*)HookGetStatusIcon, GSI_STOLEN_BYTES)) {
        g_originalGSI = (fn_get_status_icon)tramp;
        hookCount++;
        SLog("  [RAW] get_status_icon hook installed [custom icon override]");
    } else {
        SLog("  [RAW] FAILED: get_status_icon hook");
    }

    /* instance_lookup -cross-custom collision filter. Skipped in raw
     * mode because IL_STOLEN_BYTES is 0 (auto-calc) and the raw
     * trampoline has no length disassembler. Standalone users running
     * two custom statuses on the same donor will still hit the silent
     * collision; load CSF through Mewjector to get the fix. */
    SLog("  [RAW] instance_lookup hook SKIPPED -- requires Mewjector's "
         "length-disassembled trampoline. Cross-custom collisions on a "
         "shared donor will silently break under standalone loading.");

    SLog("  [RAW] %d/6 hooks installed via raw trampolines", hookCount);
    return hookCount;
}


/* ====================================================================
 *  Init wrapper hook -replacement for FUN_1400254f0
 * ==================================================================== */

static void ReplacementInitCaller(void)
{
    UINT_PTR gb = g_gameBase;
    fn_void_ptr registryBuilder = (fn_void_ptr)(gb + RVA_REGISTRY_BUILDER);
    fn_void_ptr postInitReg     = (fn_void_ptr)(gb + RVA_POST_INIT_REG);
    void*       registryAddr    = (void*)(gb + RVA_REGISTRY);
    void*       postInitCB      = (void*)(gb + RVA_POST_INIT_CB);
    int i;

    SLog("========================================");
    SLog("  ReplacementInitCaller FIRED");
    SLog("========================================");

    SLog("Step 1: Building vanilla registry...");
    registryBuilder(registryAddr);
    SLog("  Done. 792 vanilla effects registered.");

    if (!g_registryDone)
    {
        g_registryDone = 1;

        /* Register all custom statuses in MAP0–MAP9 */
        for (i = 0; i < g_statusDefCount; i++)
        {
            SLog("");
            SLog("Step 2%c: Registering '%s' in status registry...",
                 'a' + i, g_statusDefs[i].name);
            if (g_statusDefs[i].register_in_maps)
                g_statusDefs[i].register_in_maps();
            SLog("  %s registration COMPLETE.", g_statusDefs[i].name);
        }
        SLog("");
    }

    SLog("Step 3: Registering post-init callback...");
    postInitReg(postInitCB);
    SLog("  Done.");

    SLog("========================================");
    SLog("  ReplacementInitCaller COMPLETE");
    SLog("========================================");
    SLog("");
}

static void InstallInitHook(void)
{
    UINT_PTR target = g_gameBase + RVA_INIT_CALLER;
    unsigned char* p = (unsigned char*)target;
    DWORD oldProt;
    UINT_PTR hookFn;

    SLog("Installing init wrapper hook...");
    SLog("  Target RVA: 0x%X  VA: 0x%p", (unsigned)RVA_INIT_CALLER, (void*)target);
    SLog("  Current bytes: %02X %02X %02X %02X %02X %02X",
         p[0], p[1], p[2], p[3], p[4], p[5]);

    if (p[0] != 0x48 || p[1] != 0x83 || p[2] != 0xEC || p[3] != 0x28)
    {
        SLog("  ABORT: Init wrapper prologue mismatch!");
        return;
    }

    if (!VirtualProtect((LPVOID)target, 16, PAGE_EXECUTE_READWRITE, &oldProt)) {
        SLog("  ABORT: VirtualProtect failed."); return;
    }

    hookFn = (UINT_PTR)&ReplacementInitCaller;
    p[0]  = 0xFF; p[1]  = 0x25;
    p[2]  = 0x00; p[3]  = 0x00; p[4]  = 0x00; p[5]  = 0x00;
    *(UINT_PTR*)(p + 6) = hookFn;
    p[14] = 0x90; p[15] = 0x90;

    VirtualProtect((LPVOID)target, 16, oldProt, &oldProt);
    SLog("  Init hook INSTALLED.");
}


/* ====================================================================
 *  Initialization
 * ==================================================================== */

static void Initialize(void)
{
    char baseDir[MAX_PATH];
    char logDir[MAX_PATH];
    DWORD len;
    int i;

    /* -- Build the log path (but do NOT wipe the file yet) --
     *
     * Multiple csf_core.dll instances may be loaded from different mod
     * folders; only the one that wins the "active" singleton should
     * clobber the shared log file.  Losing instances must leave the
     * file untouched so the winner's log survives. */

    len = GetModuleFileNameA(NULL, baseDir, MAX_PATH);
    if (len > 0) {
        char* lastSlash = strrchr(baseDir, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
    } else {
        strcpy(baseDir, ".\\");
    }

    snprintf(logDir, MAX_PATH, "%smod_logs", baseDir);
    CreateDirectoryA(logDir, NULL);

    snprintf(g_logPath, MAX_PATH, "%smod_logs\\custombehavior_log.txt", baseDir);

    /* -- Resolve Mewjector API -- */

    if (MJ_Resolve(&s_mj))
    {
        g_mj = &s_mj;
        g_hasMewjector = 1;
        g_gameBase = g_mj->GetGameBase();

        /* -- Active-instance claim --
         *
         * Each DLL instance has a unique HMODULE address.  We format it
         * as a hex token and try to claim "csf_core/active" with that
         * token as owner.  First instance wins; subsequent same-name
         * loads from different file paths get distinct HMODULEs and
         * therefore distinct tokens, so they collide and lose.
         *
         * Losing instances return early without touching any shared
         * state: no log wipe, no hook install, no GON load.  Their
         * exports remain callable but will see empty state; extensions
         * are expected to bind to the WINNING instance via the
         * "csf_core/active" lookup (see csf_core_api.h). */
        snprintf(g_instanceToken, sizeof(g_instanceToken),
                 "0x%llx", (unsigned long long)(UINT_PTR)g_hSelf);

        if (!g_mj->RegisterName("csf_core", "active", g_instanceToken))
        {
            const char* holder = g_mj->LookupName("csf_core", "active");
            /* Use OutputDebugStringA so debuggers see the decision even
             * though we deliberately don't touch the log file. */
            OutputDebugStringA("[csf_core] Another csf_core instance is "
                               "active -this one is inert\n");
            g_mj->Log("csf_core",
                      "Inert: 'active' role held by %s (this instance=%s, hSelf=0x%p)",
                      holder ? holder : "?", g_instanceToken, (void*)g_hSelf);
            g_isActive = 0;

            /* -- Loser contribution: push our mod's custom_statuses.gon
             *    into the ACTIVE csf_core instance so our statuses get
             *    registered too.  We have direct access to the winner's
             *    HMODULE via the name-table token, so we resolve its
             *    CSFCore_MergeExternalGon export and call it with our
             *    own mod directory as the argument.  The winner does
             *    all the parsing, merging, and incremental registration.
             *
             *    If the winner lacks MergeExternalGon, GetProcAddress
             *    returns NULL and we silently skip. */
            if (holder)
            {
                unsigned long long winnerAddr = 0;
                if (sscanf(holder, "0x%llx", &winnerAddr) == 1 && winnerAddr)
                {
                    HMODULE hWinner = (HMODULE)(UINT_PTR)winnerAddr;
                    typedef int (__cdecl *MergeFn)(const char*);
                    MergeFn mergeFn = (MergeFn)GetProcAddress(
                        hWinner, "CSFCore_MergeExternalGon");

                    if (mergeFn)
                    {
                        /* Compute our own mod directory the same way
                         * CSF_LoadGonStatuses does -strip the DLL file
                         * name from g_hSelf's module path. */
                        char myModDir[MAX_PATH];
                        DWORD mlen;
                        char* slash;

                        myModDir[0] = '\0';
                        if (g_hSelf)
                        {
                            mlen = GetModuleFileNameA(g_hSelf, myModDir, MAX_PATH);
                            slash = (mlen > 0) ? strrchr(myModDir, '\\') : NULL;
                            if (slash) *(slash + 1) = '\0';
                            else        myModDir[0] = '\0';
                        }

                        if (myModDir[0])
                        {
                            int added = mergeFn(myModDir);
                            {
                                char line[512];
                                snprintf(line, sizeof(line),
                                         "[csf_core] Loser pushed '%s' to "
                                         "winner %s -- %d status(es) added\n",
                                         myModDir, holder, added);
                                OutputDebugStringA(line);
                            }
                            if (g_mj && g_mj->Log)
                                g_mj->Log("csf_core",
                                          "Loser pushed '%s' -> winner %s added %d",
                                          myModDir, holder, added);
                        }
                        else
                        {
                            OutputDebugStringA(
                                "[csf_core] Loser cannot determine own "
                                "mod dir -- skipping GON push\n");
                        }
                    }
                    else
                    {
                        /* Winner is too old (API < 3) to accept GON pushes. */
                        OutputDebugStringA(
                            "[csf_core] Active csf_core has no "
                            "MergeExternalGon export -- older version, "
                            "skipping GON contribution\n");
                        if (g_mj && g_mj->Log)
                            g_mj->Log("csf_core",
                                      "Winner %s lacks MergeExternalGon "
                                      "(older API) -- loser GON not pushed",
                                      holder);
                    }
                }
            }

            return;
        }
        g_isActive = 1;

        /* We won -safe to wipe the shared log file now. */
        { FILE* f = fopen(g_logPath, "w"); if (f) fclose(f); }

        SLog("================================================================");
        SLog("  csf_core %s (API v%d) -Mewjector Mode  [ACTIVE instance]",
             CSF_DISPLAY_VERSION, CSF_CORE_API_VERSION);
        SLog("================================================================");
        SLog("");
        SLog("Mewjector API v%d resolved", g_mj->GetVersion());
        SLog("Singleton token:    %s  (hSelf=0x%p)",
             g_instanceToken, (void*)g_hSelf);
        g_mj->Log("csf_core",
                   "Active: initializing (API v%d, token=%s)",
                   g_mj->GetVersion(), g_instanceToken);
    }
    else
    {
        /* Standalone mode: no Mewjector, no coordination layer.  We
         * assume we're the only csf_core instance; if another is loaded
         * too, they'll race on the log file.  Acceptable because
         * standalone mode is already flagged as incompatible with other
         * hook-based mods. */
        { FILE* f = fopen(g_logPath, "w"); if (f) fclose(f); }

        g_mj = NULL;
        g_hasMewjector = 0;
        g_isActive = 1;
        g_gameBase = (UINT_PTR)GetModuleHandleW(NULL);
        SLog("================================================================");
        SLog("  csf_core %s (API v%d) -Standalone Mode",
             CSF_DISPLAY_VERSION, CSF_CORE_API_VERSION);
        SLog("================================================================");
        SLog("");
        SLog("Mewjector not available -using raw trampoline hooking");
        SLog("WARNING: Standalone mode is incompatible with other hook-based mods!");
    }

    SLog("GameBase:           0x%016llX", (unsigned long long)g_gameBase);
    SLog("");

    /* -- Binary version check --
     * Read the PE header of the running executable to verify it matches
     * the binary this framework was built against. If the TimeDateStamp
     * or SizeOfImage differ, the game has been updated and all RVAs are
     * likely invalid. We warn loudly but still attempt to proceed.
     */
    {
        /* Expected PE values for the known Mewgenics.exe build */
        #define EXPECTED_TIMESTAMP    0x6A10F6CA
        #define EXPECTED_SIZE_OF_IMAGE 0x156B000

        const unsigned char* base = (const unsigned char*)g_gameBase;
        int binaryOk = 1;

        /* DOS header: e_lfanew at offset 0x3C gives PE header offset */
        DWORD peOffset = *(const DWORD*)(base + 0x3C);

        /* PE signature check */
        if (*(const DWORD*)(base + peOffset) != 0x00004550) {  /* "PE\0\0" */
            SLog("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
            SLog("!! WARNING: Could not locate PE header.            !!");
            SLog("!! Binary validation SKIPPED -proceed with caution !!");
            SLog("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
            SLog("");
            binaryOk = 0;
        }

        if (binaryOk) {
            /* COFF header starts at peOffset+4 */
            DWORD timestamp   = *(const DWORD*)(base + peOffset + 4 + 4);
            /* Optional header starts at peOffset+4+20 */
            /* SizeOfImage is at optional header + 56 */
            DWORD sizeOfImage = *(const DWORD*)(base + peOffset + 4 + 20 + 56);

            SLog("Binary check:");
            SLog("  TimeDateStamp:  0x%08X (expected 0x%08X) %s",
                 timestamp, EXPECTED_TIMESTAMP,
                 (timestamp == EXPECTED_TIMESTAMP) ? "OK" : "MISMATCH");
            SLog("  SizeOfImage:    0x%08X (expected 0x%08X) %s",
                 sizeOfImage, EXPECTED_SIZE_OF_IMAGE,
                 (sizeOfImage == EXPECTED_SIZE_OF_IMAGE) ? "OK" : "MISMATCH");

            /* Big fat fucking warning so even regular Johnathan McUser knows what happened */
            if (timestamp != EXPECTED_TIMESTAMP || sizeOfImage != EXPECTED_SIZE_OF_IMAGE) {
                SLog("");
                SLog("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                SLog("!!                                                           !!");
                SLog("!!  WARNING: BINARY MISMATCH DETECTED                        !!");
                SLog("!!                                                           !!");
                SLog("!!  The game executable does not match the version this      !!");
                SLog("!!  framework was built for. The game has most               !!");
                SLog("!!  likely been updated.                                     !!");
                SLog("!!                                                           !!");
                SLog("!!  ALL mod hooks target hardcoded addresses that are now    !!");
                SLog("!!  probably wrong. Expect crashes, freezes, or broken       !!");
                SLog("!!  behavior. This is NOT a bug in the dependent mods.       !!");
                SLog("!!                                                           !!");
                SLog("!!  The mod developer will rebuild against the new binary.   !!");
                SLog("!!  This may take some time, so please be patient            !!");
                SLog("!!  and wait for a new release.                              !!");
                SLog("!!                                                           !!");
                SLog("!!  Continuing anyway, but things WILL break.                !!");
                SLog("!!                                                           !!");
                SLog("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                SLog("");
            } else {
                SLog("  Binary version: VERIFIED");
            }
            SLog("");
        }
    }

    /* -- Register all custom statuses --
     * This must happen before hook installation so the registration
     * table is populated and type IDs are allocated. */
    CSF_RegisterAllStatuses();
    SLog("[CSF] %d C-defined status(es) registered.", g_statusDefCount);
    SLog("");

    /* -- Load GON-defined statuses --
     * Reads custom_statuses.gon from disk and auto-registers each
     * status. C extensions (registered during CSF_RegisterAllStatuses
     * or earlier) are applied after vtable builds. */
    {
        int gonCount = CSF_LoadGonStatuses();
        if (gonCount > 0)
            SLog("[CSF] %d GON-defined status(es) added (total: %d)",
                 gonCount, g_statusDefCount);
    }

    /* Build all custom vtables.
     *
     * Four modes:
     *   1. donor_name only (pure GON, no overrides, no build_vtable) ->
     *      run the default builder. Produces a pure donor clone with
     *      generic identity overrides.
     *   2. donor_name + slot_overrides -> run the default builder; it
     *      copies the donor and then applies overrides.
     *   3. build_vtable only (legacy C path) -> call it alone; the
     *      status constructs its own full vtable.
     *   4. build_vtable + slot_overrides -> run build_vtable first
     *      (e.g. for type info patching), then the default builder
     *      on top (it wipes to donor and reapplies overrides). */
    for (i = 0; i < g_statusDefCount; i++)
    {
        CSF_StatusDef* d = &g_statusDefs[i];
        int hasOverrides = (d->slot_overrides != NULL
                            && d->slot_override_count > 0);
        int needsDefault = hasOverrides
                        || (d->donor_name && !d->build_vtable);

        if (d->build_vtable)
            d->build_vtable();

        if (needsDefault)
            CSF_DefaultBuildVtable(d);

        SLog("");
    }

    /* -- Apply C extensions to GON-defined vtables --
     * Must happen after all vtables are built so the base donor
     * slots are in place before C code replaces specific slots. */
    CSF_ApplyAllExtensions();

    /* -- Install hooks --
     * The init_wrapper is always a raw replacement (it replaces the
     * entire function body, not a hook-and-chain). The other 5 hooks
     * use Mewjector when available, raw trampolines otherwise. */
    InstallInitHook();
    SLog("");

    if (g_hasMewjector)
        InstallHooks_Mewjector();
    else
        InstallHooks_Raw();

    SLog("");

    /* Verify hook chain integrity (Mewjector only) */
    if (g_hasMewjector)
    {
        /* VerifyHooks returns the *number of corrupted sites* (0 = all OK). */
        int corrupted = g_mj->VerifyHooks();
        int ok = (corrupted == 0);
        if (ok) {
            SLog("[CSF] Mewjector hook verification: PASSED");
        } else {
            SLog("[CSF] Mewjector hook verification: FAILED (%d site(s) corrupted)", corrupted);
        }
        g_mj->Log("CSF", "Init complete: %d statuses, hook verify=%s",
                   g_statusDefCount, ok ? "OK" : "FAIL");
    }

    SLog("================================================================");
    SLog("  Initialization COMPLETE -awaiting game init call");
    SLog("================================================================");
    SLog("");
}


/* ====================================================================
 *  VEH crash diagnostics
 * ==================================================================== */

static LONG CALLBACK CrashDiagnosticHandler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* ctx = ep->ContextRecord;
    ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    UINT_PTR rip = (UINT_PTR)ep->ExceptionRecord->ExceptionAddress;
    UINT_PTR rvaRip = (g_gameBase != 0) ? (rip - g_gameBase) : 0;

    if (code != 0xC0000005)
        return EXCEPTION_CONTINUE_SEARCH;

    SLog("");
    SLog("ACCESS VIOLATION CAUGHT BY VEH");
    SLog("Faulting RIP:    0x%016llX  (RVA 0x%llX)",
         (unsigned long long)rip, (unsigned long long)rvaRip);
    SLog("Access type:     %s",
         (info[0] == 0) ? "READ" : (info[0] == 1) ? "WRITE" : "DEP/EXECUTE");
    SLog("Target address:  0x%016llX", (unsigned long long)info[1]);
    SLog("");
    SLog("RAX=0x%016llX RBX=0x%016llX RCX=0x%016llX RDX=0x%016llX",
         (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
         (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
    SLog("RSI=0x%016llX RDI=0x%016llX RSP=0x%016llX RBP=0x%016llX",
         (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
         (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp);
    SLog("R8 =0x%016llX R9 =0x%016llX R10=0x%016llX R11=0x%016llX",
         (unsigned long long)ctx->R8,  (unsigned long long)ctx->R9,
         (unsigned long long)ctx->R10, (unsigned long long)ctx->R11);
    SLog("R12=0x%016llX R13=0x%016llX R14=0x%016llX R15=0x%016llX",
         (unsigned long long)ctx->R12, (unsigned long long)ctx->R13,
         (unsigned long long)ctx->R14, (unsigned long long)ctx->R15);
    SLog("");

    return EXCEPTION_CONTINUE_SEARCH;
}


/* ====================================================================
 *  CSFCore_* exported API (see csf_core_api.h)
 *
 *  These are thin wrappers around the existing internal functions so
 *  the public ABI stays stable while the internal symbols can evolve.
 *  All exports are __cdecl to match the typedefs in csf_core_api.h.
 * ==================================================================== */

__declspec(dllexport) int __cdecl CSFCore_GetVersion(void)
{
    return CSF_CORE_API_VERSION;
}

__declspec(dllexport) int __cdecl CSFCore_RegisterExtension(
    const char* statusName, int slotIndex, void* fnPtr)
{
    int i;
    CSF_StatusDef* def = NULL;
    int result = 0;

    if (!statusName || !fnPtr) return 0;
    if (slotIndex < 0 || slotIndex >= 172) {
        SLog("[csf_core] RegisterExtension: slot %d out of range for '%s'",
             slotIndex, statusName);
        return 0;
    }

    /* Inert-instance guard: if this DLL lost the active-singleton race,
     * our g_statusDefs is empty.  Rather than pretending to serve the
     * call and confusing the caller with an "unknown status" message,
     * tell the extension explicitly that it bound to the wrong instance.
     * Done before taking the lock because the inert instance never
     * initialized its lock either. */
    if (!g_isActive) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[csf_core] RegisterExtension('%s') called on INERT instance "
                 "(token=%s) -- extension must bind to the active csf_core "
                 "(look up MJ_LookupName(\"csf_core\",\"active\"))\n",
                 statusName, g_instanceToken);
        OutputDebugStringA(buf);
        if (g_hasMewjector && g_mj) {
            g_mj->Log("csf_core",
                      "Inert RegisterExtension('%s') -- token=%s -- "
                      "caller should use the active instance",
                      statusName, g_instanceToken);
        }
        return 0;
    }

    /* Serialize against CSF_MergeExternalGon and CSF_ApplyAllExtensions.
     * Without this lock, a background thread-pool timer could be in the
     * middle of a merge (adding new statuses, building vtables) while
     * a different extension's bind walks g_statusDefs and writes to a
     * slot that the merge thread is about to memset back to defaults. */
    if (g_csfLockReady) EnterCriticalSection(&g_csfLock);

    /* Extension DLLs load after Core's Initialize() has already built
     * and applied vtables.  Patching the live custom_vtable directly is
     * therefore the only option that takes effect.  If this is called
     * DURING Core's own Initialize (e.g. a hypothetical built-in
     * extension), the same write still works because the vtable has
     * already been built by the time CSF_ApplyAllExtensions runs. */
    for (i = 0; i < g_statusDefCount; i++) {
        if (g_statusDefs[i].name &&
            strcmp(g_statusDefs[i].name, statusName) == 0) {
            def = &g_statusDefs[i];
            break;
        }
    }
    if (!def) {
        SLog("[csf_core] RegisterExtension: unknown status '%s'", statusName);
        goto done;
    }
    if (!def->custom_vtable) {
        SLog("[csf_core] RegisterExtension: '%s' has no custom_vtable yet -- "
             "called before Core finished Initialize?", statusName);
        goto done;
    }

    /* Note: collision detection against an existing extension binding
     * is not done here because the slot normally holds a donor default
     * function pointer at this point, which is not distinguishable from
     * another extension's pointer without a separate bookkeeping table.
     * A proper collision check would walk g_extensions[] from the GON
     * loader, or track extension bindings in a dedicated registry.
     * Deferred to a later fix. */
    def->custom_vtable[slotIndex] = fnPtr;
    SLog("[csf_core] Live-patched %s slot %d -> 0x%p", statusName, slotIndex, fnPtr);
    result = 1;

done:
    if (g_csfLockReady) LeaveCriticalSection(&g_csfLock);
    return result;
}

__declspec(dllexport) int __cdecl CSFCore_GetSidecarForInstance(
    void* instance, CSFSidecarView* out_view)
{
    CSF_InstanceSidecar* sc;
    int i;
    if (!instance || !out_view) return 0;
    if (out_view->struct_size < sizeof(uint32_t) + sizeof(void*) * 2 + sizeof(int))
        return 0;

    sc = CSF_GetSidecar(instance);
    if (!sc) return 0;

    out_view->instance       = sc->instance;
    out_view->entity         = sc->entity;
    out_view->status_def_idx = sc->status_def_idx;

    /* Copy whatever param slots fit into out_view.  Core's sidecar
     * has 8 slots; our view also exposes 8 (CSF_VIEW_MAX_PARAMS).
     * The struct_size check above ensures we only write fields that
     * fit in the caller's buffer. */
    if (out_view->struct_size >= sizeof(CSFSidecarView))
    {
        for (i = 0; i < CSF_VIEW_MAX_PARAMS && i < 8; i++)
        {
            out_view->int_params[i] = sc->int_params[i];
            out_view->dbl_params[i] = sc->dbl_params[i];
        }
    }
    return 1;
}

__declspec(dllexport) int __cdecl CSFCore_GetConfig(
    const char* statusName, CSFStatusConfigView* out_view)
{
    int i;
    const CSF_StatusConfig* cfg;
    const CSF_StatusDef*    def = NULL;
    int                     defIdx = -1;

    if (!statusName || !out_view) return 0;
    if (out_view->struct_size < sizeof(uint32_t) + sizeof(const char*) * 4 + sizeof(int))
        return 0;

    /* Linear scan by name -the def count is small (<= CSF_MAX_STATUSES)
     * and this is a slow-path lookup from extension init code. */
    for (i = 0; i < g_statusDefCount; i++)
    {
        if (g_statusDefs[i].name &&
            strcmp(g_statusDefs[i].name, statusName) == 0)
        {
            def    = &g_statusDefs[i];
            defIdx = i;
            break;
        }
    }
    if (!def) return 0;

    cfg = CSF_GetStatusConfig(defIdx);

    out_view->name       = def->name;
    out_view->donor_name = def->donor_name;
    out_view->display    = NULL;  /* reserved; wire up when we expose display string */
    out_view->category   = NULL;  /* reserved; wire up when we expose category       */
    out_view->apply_mode = cfg ? CSF_APPLY_ACCUMULATE : CSF_APPLY_CLONE_DONOR;
    /* NOTE: apply_mode storage lives inside CSF_StatusConfig's int_defaults
     * slot by convention (see prior diagnosis).  We read it back via the
     * same path the hook uses. */
    if (cfg && CSF_CONFIG_MAX_PARAMS <= CSF_GON_MAX_PARAMS)
    {
        /* Defensive: only copy up to the smaller of the two max counts. */
        for (i = 0; i < CSF_CONFIG_MAX_PARAMS; i++)
        {
            out_view->int_defaults[i] = cfg->int_defaults[i];
            out_view->dbl_defaults[i] = cfg->dbl_defaults[i];
        }
        /* apply_mode is stored in int_defaults[1] per CSF_GonLoader convention */
        out_view->apply_mode = cfg->int_defaults[1];
    }
    else
    {
        for (i = 0; i < CSF_CONFIG_MAX_PARAMS; i++)
        {
            out_view->int_defaults[i] = 0;
            out_view->dbl_defaults[i] = 0.0;
        }
    }
    return 1;
}

__declspec(dllexport) void __cdecl CSFCore_Log(const char* fmt, ...)
{
    /* Forward to SLog via vprintf-style.  SLog is already varargs, but
     * it doesn't expose a va_list entry point, so we format into a
     * scratch buffer and pass "%s". */
    char buf[1024];
    va_list ap;
    if (!fmt) return;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    SLog("%s", buf);
}

__declspec(dllexport) UINT_PTR __cdecl CSFCore_GetGameBase(void)
{
    return g_gameBase;
}

/* --------------------------------------------------------------------
 *  CSFCore_RegisterBehavior -publish a named built-in behavior
 *
 *  Thin wrapper around CSF_RegisterBehavior.  See csf_core.h for the
 *  full design: extensions publish C function pointers by name, and
 *  GON slot_overrides reference those names.  Order-independent
 *  resolution via CSF_ResolvePendingBehaviorBindings.
 *
 *  Inert-instance guard matches CSFCore_RegisterExtension: a losing
 *  csf_core.dll instance returns 0 immediately rather than poisoning
 *  its own empty behavior table and silently eating the call.
 *
 * -------------------------------------------------------------------- */
__declspec(dllexport) int __cdecl CSFCore_RegisterBehavior(
    const char* name, int slotIndex, int chainMode, void* fn)
{
    if (!g_isActive) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[csf_core] RegisterBehavior('%s') called on INERT instance "
                 "(token=%s) -- extension must bind to the active csf_core\n",
                 name ? name : "(null)", g_instanceToken);
        OutputDebugStringA(buf);
        return 0;
    }
    return CSF_RegisterBehavior(name, slotIndex, chainMode, fn);
}

/* --------------------------------------------------------------------
 *  CSFCore_IsActive -Returns 1 if this DLL instance won the singleton
 *  race and fully initialized, 0 if it is an inert loser.  Extensions
 *  use this to probe each loaded csf_core.dll when the Mewjector
 *  name-table lookup is unavailable.  Always safe to call.
 * -------------------------------------------------------------------- */
__declspec(dllexport) int __cdecl CSFCore_IsActive(void)
{
    return g_isActive;
}

__declspec(dllexport) void* __cdecl CSFCore_GetDonorSlotFn(
    const char* donorName, int slotIndex)
{
    const UINT_PTR* rvas;
    if (!donorName) return NULL;
    if (slotIndex < 0 || slotIndex >= 172) return NULL;

    rvas = CSF_ResolveDonorVtable(donorName);
    if (!rvas) return NULL;
    if (rvas[slotIndex] == 0) return NULL;

    return (void*)(g_gameBase + rvas[slotIndex]);
}

/* --------------------------------------------------------------------
 *  CSFCore_RequestRemoval -ask the engine to remove a status instance
 *
 *  Thin wrapper over the game's internal request_removal routine at
 *  RVA 0x7650B0 (FUN_1407650b0 in the current decomp).  Resolved once
 *  per call via g_gameBase.  Extensions that want to implement
 *  removal-driven behaviors (e.g. "decrement counter, remove at
 *  zero") call this instead of hard-coding the RVA themselves, which
 *  keeps the binary-offset knowledge inside csf_core.
 *
 *  The engine routine expects the status instance pointer the slot
 *  received as `self`.  It performs its own teardown/destructor
 *  chain; do not free the instance after calling.
 *
 *  Inert-instance guard: a loser csf_core.dll that never claimed the
 *  active singleton still has a valid g_gameBase, so we don't gate
 *  on g_isActive here -the call is genuinely stateless and safe
 *  from either instance.
 *
 * -------------------------------------------------------------------- */
__declspec(dllexport) void __cdecl CSFCore_RequestRemoval(void* instance)
{
    typedef void (*request_removal_fn_t)(void* self);
    request_removal_fn_t fn;
    if (!instance) return;
    if (!g_gameBase) return;
    fn = (request_removal_fn_t)(g_gameBase + 0x7650B0);
    fn(instance);
}

/* --------------------------------------------------------------------
 *  CSFCore_SidecarIntByName / CSFCore_SidecarDblByName
 *
 *  Named-parameter accessors for custom status instances.  These
 *  wrap the internal CSF_SidecarInt / CSF_SidecarDbl helpers so
 *  extension DLLs can read a GON-authored `params { damage 3 }`
 *  value directly by name, instead of going through the positional
 *  int_params[] / dbl_params[] arrays on CSFSidecarView and
 *  reconstructing the mapping from the status's param map.
 *
 *  Name lookup matches CSF_SidecarInt semantics: if the parameter
 *  is declared as the opposite storage type (double vs int), the
 *  helper does a silent truncation / promotion so the caller gets
 *  the typed view it asked for.  Returns 0 / 0.0 if the instance
 *  has no sidecar, the status has no param map, or the name is
 *  absent from the map.
 *
 * -------------------------------------------------------------------- */
__declspec(dllexport) int __cdecl CSFCore_SidecarIntByName(
    void* instance, const char* paramName)
{
    CSF_InstanceSidecar* sc;
    if (!instance || !paramName) return 0;
    sc = CSF_GetSidecar(instance);
    if (!sc) return 0;
    return CSF_SidecarInt(sc, paramName);
}

__declspec(dllexport) double __cdecl CSFCore_SidecarDblByName(
    void* instance, const char* paramName)
{
    CSF_InstanceSidecar* sc;
    if (!instance || !paramName) return 0.0;
    sc = CSF_GetSidecar(instance);
    if (!sc) return 0.0;
    return CSF_SidecarDbl(sc, paramName);
}

/* --------------------------------------------------------------------
 *  CSFCore_MergeExternalGon -Cross-instance GON merge.
 *
 *  Called by a losing csf_core.dll instance on the ACTIVE csf_core
 *  instance to hand off its own custom_statuses.gon file so the active
 *  instance can load any statuses it defines.  The caller passes its
 *  own mod directory (where its data\custom_statuses.gon lives).
 *
 *  Returns the number of newly-registered statuses, or 0 if this DLL
 *  is not the active instance (the caller should not be calling a
 *  loser), if no GON file was found, or if the status table was full.
 *
 * -------------------------------------------------------------------- */
__declspec(dllexport) int __cdecl CSFCore_MergeExternalGon(const char* modDir)
{
    if (!g_isActive) {
        /* An inert instance has nothing useful to merge into -if a
         * loser mistakenly calls another loser (shouldn't happen, as
         * the name-table lookup returns the winner), bail out loudly
         * without touching any state. */
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[csf_core] MergeExternalGon called on INERT instance "
                 "(token=%s) -- caller should use the active instance\n",
                 g_instanceToken);
        OutputDebugStringA(buf);
        return 0;
    }
    return CSF_MergeExternalGon(modDir);
}


/* ====================================================================
 *  DllMain
 * ==================================================================== */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)lpReserved;

    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        g_hSelf = hModule;              /* stash for GON loader path resolution */
        DisableThreadLibraryCalls(hModule);

        /* Initialize the global mutex BEFORE anything that may take it.
         * InitializeCriticalSection is documented loader-lock safe; it
         * does not block and cannot fail on modern Windows for non-
         * shared critical sections. */
        InitializeCriticalSection(&g_csfLock);
        g_csfLockReady = 1;

        /* Register vectored exception handler for crash diagnostics. */
        AddVectoredExceptionHandler(1, CrashDiagnosticHandler);
        Initialize();
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH)
    {
        /* Mewjector doesn't expose an "unregister name" call today, so
         * the "csf_core/active" token may outlive us.  Any future
         * CSFCore__FindActiveModule caller should validate the HMODULE
         * it reads from that token before use. */

        if (g_csfLockReady) {
            g_csfLockReady = 0;
            DeleteCriticalSection(&g_csfLock);
        }
    }
    return TRUE;
}
