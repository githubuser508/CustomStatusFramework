/*
 * CustomStatusFramework.h -Shared header for the donor-free custom status framework
 *
 * Include this from any status definition file (.c) to access:
 *   - Game RVAs and map offset constants
 *   - Function pointer typedefs for game APIs
 *   - Framework globals (g_gameBase, g_originalAS, etc.)
 *   - Donor vtable RVA arrays (all 303 vanilla statuses via DonorVtables.h)
 *   - Status registration API (CSF_StatusDef, CSF_RegisterStatus)
 *   - Logging (SLog)
 *
 * Compilation model:
 *   cl /LD /O2 /GS- CustomStatusFramework.c MyStatus.c /Fe:MyMod.dll
 *   ren MyMod.dll MyMod.asi
 *
 * Each status file implements its statuses and provides a registration
 * function. One file must implement CSF_RegisterAllStatuses() which
 * calls CSF_RegisterStatus() for each custom status.
 *
 * If you have multiple status files, create a small glue file:
 *   void MyStatusA_Register(void);   // from MyStatusA.c
 *   void MyStatusB_Register(void);   // from MyStatusB.c
 *   void CSF_RegisterAllStatuses(void) {
 *       MyStatusA_Register();
 *       MyStatusB_Register();
 *   }
 */ 

#ifndef CUSTOM_STATUS_FRAMEWORK_H
#define CUSTOM_STATUS_FRAMEWORK_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mewjector.h"

/* Extension-facing ABI: CSF_ElementMask, CSF_ImmunityMask, CSF_STAT_*,
 * CSF_BEARER(), CSF_INSTANCE_COUNTER, chain-mode constants, sidecar
 * view, etc.  Internal translation units pick them up through this
 * include so there is a single source of truth. */
#include "csf_core_api.h"


/* ====================================================================
 *  RVAs -offsets from game base to target addresses
 * ==================================================================== */

/* Init wrapper and registry */
#define RVA_INIT_CALLER      0x25370
#define RVA_REGISTRY_BUILDER 0x3D3DE0
#define RB_STOLEN_BYTES      0   /* 0 = let mj_lde auto-calculate */
#define RVA_POST_INIT_REG    0xD2E990
#define RVA_POST_INIT_CB     0xE2F800

/* Hooking targets */
#define RVA_ATTACH_PASSIVE   0x491B90
#define AP_STOLEN_BYTES      15
#define RVA_APPLY_STATUS     0x480C80
#define AS_STOLEN_BYTES      15
#define RVA_GET_STATUS_FLAGS 0x490CA0
#define GSF_STOLEN_BYTES     15
#define RVA_RESOLVE_STATUS   0x4904D0
#define RS_STOLEN_BYTES      15
#define RVA_PRIMARY_REG      0x95AF40  /* was 0x959B90: that was a 4-arg filter-collect helper, not registration. Day-1 bug -RelocateRVAs never actually implemented the primary_reg heuristic. Real primary_reg has the fingerprint (classDesc, instance), calls vtable slot 3, touches classDesc+0x18/0x20/0x488. */
#define PR_STOLEN_BYTES      15

/* Status registry */
#define RVA_REGISTRY         0x13B1810

/* String functions */
#define RVA_STRING_CTOR      0x52640
#define RVA_STRING_DTOR      0x522D0
#define RVA_STRING_CLEANUP   0x287A0

/* Map insert functions */
#define RVA_INT32_INSERT     0x556330
#define RVA_FLOAT64_INSERT   0x5B51B0
#define RVA_DATA32_INSERT    0x556190
#define RVA_TYPEA_INSERT     0x556170
#define RVA_TYPEB_INSERT     0x556150

/* Registration helpers */
#define RVA_DYNARRAY_APPEND  0x47B60
#define RVA_HASHMAP_INSERT   0x804050
#define RVA_NOOP_CALLBACK    0x35A60

/* Template vtables (donors) */
#define RVA_BLEED_VT         0xF76B00
#define RVA_SOULLINK_VT      0xF31B90

/* Display lookup -glaiel::get_status_icon(StatusIconInfo* out,
 * std::string* name, bool allow_missing). This is the sole reader of
 * the DAT_1413B1910 FNV1a-keyed icon table. Prologue at this RVA is
 * 15 clean bytes (register saves + pushes + LEA/SUB), no RIP-relative
 * instructions -safe for Mewjector's memcpy-based trampoline. Hooked
 * to intercept icon lookups for custom statuses. */
#define RVA_DISPLAY_LOOKUP   0x490EF0
#define GSI_STOLEN_BYTES     15

/* Status removal -called when stacks reach 0 */
#define RVA_STATUS_REMOVAL   0x75CF80

/* Post-turn-end notification */
#define RVA_POST_TURN_NOTIFY 0x75F0F0

/* String assign */
#define RVA_STRING_ASSIGN    0x51C20

/* Heal function */
#define RVA_APPLY_HEAL       0x113DB0

/* Map offsets within the 0x280-byte registry struct */
#define MAP0  0x000
#define MAP1  0x040
#define MAP2  0x080
#define MAP3  0x0C0
#define MAP4  0x100
#define MAP5  0x140
#define MAP6  0x180
#define MAP7  0x1C0
#define MAP8  0x200
#define MAP9  0x240

/* --------------------------------------------------------------------
 *  MAP4 32-byte opaque blob -field layout
 *
 *  Proven via Ghidra analysis of FUN_1403d3de0 (RVA_REGISTRY_BUILDER). The
 *  builder memcpies a status-specific 32-byte blob from .rdata onto
 *  the stack, inserts it into MAP4 keyed by the status name string,
 *  and then MOVSB.REPs it into the slot that DATA32_INSERT returned.
 *
 *  Ground-truth dumps for Bleed / Freeze / Poison (raw bytes at
 *  0x1411040b8 / 0x141104158 / 0x1411040d8):
 *
 *      Bleed   14  -1  4  0   0  0  0  0
 *      Freeze   5  -1  5  0   2  0  0  0
 *      Poison   8  -1  4  0   0  0  0  0
 *
 *  Only three of the eight int32 slots vary per status; the rest are
 *  constants or permanent zero padding.
 * -------------------------------------------------------------------- */
#define MAP4_BLOB_OFF_ICON      0x00   /* int32 -per-character status icon
                                        * index. Bleed=14, Freeze=5,
                                        * Poison=8, etc. This is the
                                        * field CSF needs to splice to
                                        * give custom statuses their own
                                        * portrait glyph. */
#define MAP4_BLOB_OFF_SENTINEL  0x04   /* int32 -always 0xffffffff (-1)
                                        * across every vanilla row.
                                        * Semantics unknown -probably a
                                        * "no secondary slot" marker. */
#define MAP4_BLOB_OFF_CATEGORY  0x08   /* int32 -behavior category or
                                        * tier: Bleed/Poison=4, Freeze=5,
                                        * generic buffs=6, unique statuses
                                        * scatter across 3..6. Semantics
                                        * not yet decoded. */
#define MAP4_BLOB_OFF_PAD0C     0x0C   /* int32 -always 0 across vanilla */
#define MAP4_BLOB_OFF_FLAGS     0x10   /* int32 -0 or 2; set on Freeze
                                        * and similar hard-CC statuses,
                                        * unset on Bleed/Poison. Semantics
                                        * not yet decoded. */
#define MAP4_BLOB_OFF_PAD14     0x14   /* int32 -always 0 across vanilla */
#define MAP4_BLOB_OFF_PAD18     0x18   /* int32 -always 0 across vanilla */
#define MAP4_BLOB_OFF_PAD1C     0x1C   /* int32 -always 0 across vanilla */
#define MAP4_BLOB_SIZE          0x20   /* total blob size, memcpy'd
                                        * wholesale by the builder. */


/* ====================================================================
 *  Slot 80 / 81 / 82 ABI (element mask, immunity mask, stat table)
 *
 *  Moved into csf_core_api.h so extension DLLs (which only include the
 *  lean extension-facing header) can see them directly.  csf_core.h
 *  pulls csf_core_api.h above, so internal callers still get the same
 *  symbols (CSF_ElementMask, CSF_ImmunityMask, CSF_STAT_*, CSF_STAT_OFFSET,
 *  CSF_BEARER, CSF_INSTANCE_COUNTER, CSF_StatIndex, etc.).  See the
 *  documentation block in csf_core_api.h for the full derivation.
 *
 *  Slot 81 is confirmed in binary as the immunity-mask getter.
 *  Bleed's slot 81 returns 0x4000 and
 *  Freeze's returns 0x300020, matching CSF_IMMUNITY_BLEED and
 *  CSF_IMMUNITY_FREEZE_MASK in csf_core_api.h. The fn shape is
 *  15 bytes: `*p = 0; *(uint32*)p = mask; return p;` -i.e. it
 *  zero-initializes the caller's out-slot, stores the 32-bit mask,
 *  and returns the pointer. This is stable across all three and
 *  is the canonical CSF_VSLOT_GET_IMMUNITY_MASK = 81.
 * ==================================================================== */


/* ====================================================================
 *  glaiel::Passive instance layout -base class shared by every status
 *
 *  Derived empirically (2026-04-11) by decompiling the base
 *  constructor FUN_14004c510 and the apply-status factories for Bleed,
 *  Freeze, and Poison.
 *
 *  Offsets marked (W) are written by the base ctor. Offsets marked (F)
 *  are written later by the derived factory / setter. Everything else
 *  in the 0xF0-byte instance is memset-zeroed by the allocator.
 *
 *    +0x00  (F) vptr                  -derived class vtable RVA
 *                                        (base ctor initially writes
 *                                        glaiel::Passive::vftable and
 *                                        the factory overwrites it)
 *    +0x08  (W) int32 instance_id     -monotonic, post-incremented from
 *                                        DAT_1413ae9d0 at construction
 *    +0x30  (W) double 1.0            -literal 0x3ff0000000000000,
 *                                        purpose unknown but constant
 *    +0x5c  (F) uint32 stacks/duration -zeroed by memset, set later by
 *                                        slot-171 (OnStacksChangedPost-
 *                                        Tick) and slot-112 paths
 *    +0x68  (W) void* string_pool      -shared DAT_1413b1d90 pointer
 *    +0x70  (W) uint32 0x01010100      -bitflags of some kind
 *    +0xa0..+0xbf (W) MSVC SSO string -capacity marker 0x0f at +0xb8,
 *                                        zero size at +0xb0, zero data
 *                                        at +0xa0..+0xaf (empty). Never
 *                                        touched by ctor or by the
 *                                        Bleed/Freeze/Poison factories;
 *                                        assumed to be set later by an
 *                                        engine setter that consumes a
 *                                        source struct.
 *
 *  NOT PRESENT (refuted): no field at any offset in the 0xF0-
 *  byte instance stores an "icon integer" matching the ground-truth
 *  values 5=Freeze / 8=Poison / 14=Bleed. The icon lives in the MAP4
 *  32-byte blob keyed by status name in the 10-map registry -see
 *  MAP4_BLOB_OFF_ICON above.
 * ==================================================================== */


/* ====================================================================
 *  Function pointer typedefs
 * ==================================================================== */

typedef void     (*fn_void_ptr)(void*);
typedef void*    (*fn_string_ctor)(void*, const char*);
typedef void     (*fn_string_dtor)(void*);
typedef void     (*fn_string_cleanup)(void*);
typedef void*    (*fn_map_insert)(void*, void*);
typedef void*    (*fn_attach_passive)(void*, void*, void*, int);
typedef int      (*fn_get_status_flags)(void*);
typedef void     (*fn_resolve_status)(void*, void*, void*, int, void*);
typedef void*    (*fn_display_lookup)(void*, void*, char);
typedef void*    (*fn_apply_status)(void*, void*, int, void*, void*,
                                    void*, char, char);
typedef void     (*fn_primary_reg)(void*, void*);
typedef void     (*fn_dynarray_append)(void*, void*);
typedef void     (*fn_hashmap_insert)(void*, void*);
typedef void     (*fn_status_removal)(void*);
typedef void     (*fn_post_turn_notify)(void*, void*);
typedef void*    (*fn_string_assign)(void*, const char*, UINT64);
typedef void     (*fn_apply_heal)(void*, int, void*, int, int, int, int);


/* ====================================================================
 *  Framework globals (defined in CustomStatusFramework.c)
 * ==================================================================== */

extern UINT_PTR         g_gameBase;
extern HMODULE          g_hSelf;       /* our DLL handle -for mod-relative path resolution */
extern fn_apply_status  g_originalAS;

/* Mewjector API -NULL if running in standalone mode */
extern MewjectorAPI*    g_mj;

/* 1 if Mewjector was resolved successfully, 0 if standalone */
extern int              g_hasMewjector;

/* ====================================================================
 *  Global mutex
 *
 *  Serializes every mutation of the shared status state: g_statusDefs,
 *  g_gonDoc, the vtable pool, g_extensions, and the slot overrides.
 *  Needed because extension DLLs can push GONs and RegisterExtension
 *  calls from thread-pool worker threads (the CSFCore_InitOrWait retry
 *  loop runs after DllMain has returned, outside the loader lock).
 *
 *  Windows CRITICAL_SECTION is recursive -a thread that already owns
 *  the lock can re-enter safely, so MergeExternalGon() can call
 *  ApplyAllExtensions() without self-deadlocking.
 *
 *  Initialized eagerly in DllMain before Initialize() runs, which is
 *  documented as safe: InitializeCriticalSection does not block on the
 *  loader lock. Entry points that take this lock:
 *    - CSFCore_RegisterExtension (exported)
 *    - CSF_MergeExternalGon / CSFCore_MergeExternalGon
 *    - CSF_ApplyAllExtensions
 *
 *  Internal callees (LoadOneStatus, CSF_BuildVtableForStatus) are
 *  called while the lock is already held -their individual entry
 *  points do not re-enter the lock.
 * ==================================================================== */
extern CRITICAL_SECTION g_csfLock;
extern int              g_csfLockReady;   /* 1 once InitializeCriticalSection ran */


/* ====================================================================
 *  Donor vtable RVA arrays (defined in DonorVtables.c)
 *
 *  All 303 vanilla status vtable arrays, auto-generated from the
 *  RelocateRVAs output. Every vanilla status is a valid donor.
 *  Regenerate with: python generate_donors.py <RelocateRVAs_output.txt>
 * ==================================================================== */

#include "DonorVtables.h"


/* ====================================================================
 *  Logging (defined in CustomStatusFramework.c)
 *
 *  SLog() writes to the file log always. When Mewjector is available
 *  it also routes through mj.Log for the shared chainloader log.
 * ==================================================================== */

void SLog(const char* fmt, ...);


/* ====================================================================
 *  Dynamic type ID allocation
 *
 *  When Mewjector is available, type IDs are allocated via
 *  mj.AllocTypeIdPair() so multiple CSF-based mods can't collide.
 *  When standalone, falls back to CUSTOM_INDEX_BASE.
 *
 *  Call CSF_AllocTypeIdPair() during status registration to get a
 *  pair of IDs for the type info struct (parent, leaf).
 * ==================================================================== */

/* Fallback base -only used when Mewjector is absent */
#define CSF_STANDALONE_INDEX_BASE  0x072A

/* Allocate a pair of collision-free type IDs.
 * Returns the base -use base for the leaf, base+1 for the parent. */
UINT_PTR CSF_AllocTypeIdPair(const char* statusName);


/* ====================================================================
 *  Slot override -pick a behavioral slot from a different donor
 *
 *  Used by the mix-and-match vtable assembly system.
 *  Each override specifies a vtable slot index and either:
 *    - A source donor name (framework resolves the RVA), OR
 *    - A direct function pointer for fully custom behavior.
 * ==================================================================== */

/* A slot override can come from three sources, in priority order:
 *   1. custom_fn non-NULL  -- direct C function pointer, installed
 *                             verbatim into the slot (used by
 *                             CSF_SLOT_CUSTOM and by extension DLLs
 *                             that hand us raw fn pointers at register
 *                             time).  No chaining.
 *   2. source_donor set    -- copy slot N from a different donor's
 *                             vtable.  The classic Phase-3 mix-and-
 *                             match case.  No chaining.
 *   3. behavior_name set   -- named built-in behavior, resolved at
 *                             build-vtable time against the registry
 *                             populated by CSF_RegisterBehavior.  The
 *                             slot is wired up through a per-
 *                             signature C dispatcher that reads
 *                             behavior metadata at call time and
 *                             optionally chains to the donor's
 *                             original slot fn.  See the "Behavior
 *                             registry" block below. */
typedef struct {
    int         slot_index;     /* vtable slot to override (0–171)       */
    const char* source_donor;   /* e.g. "Marked" -NULL if using custom_fn */
    void*       custom_fn;      /* direct function pointer -NULL if using donor */

    /* Built-in behavior reference.  Points into the slot_override_name
     * storage (the GON parser already copies this string for logging);
     * NULL if this override isn't a named-behavior override.  Resolved
     * on the vtable-build path by CSF_ResolveBehaviorOverride. */
    const char* behavior_name;

    /* Cached result of behavior-name resolution.  Set by
     * CSF_ResolveBehaviorOverride either immediately (if the behavior
     * was already registered) or later (on the sweep triggered by
     * CSF_RegisterBehavior).  Non-NULL means the behavior has been
     * bound to the slot via a dispatcher. */
    void*       behavior_fn;    /* registered behavior C function */
    int         behavior_mode;  /* CSF_CHAIN_* from below */
    void*       behavior_donor_fn; /* donor's original slot fn, captured
                                    * at bind time for the dispatcher to
                                    * chain through (CSF_CHAIN_AFTER) */
} CSF_SlotOverride;

/* Convenience macros for building override lists */
#define CSF_SLOT_FROM(donor, slot)   { (slot), #donor, NULL }
#define CSF_SLOT_CUSTOM(slot, fn)    { (slot), NULL, (void*)(fn) }

/* Maximum slot overrides per status definition */
#define CSF_MAX_SLOT_OVERRIDES 32


/* ====================================================================
 *  Donor vtable lookup -resolve a donor name to its RVA array
 *
 *  Returns the 172-entry UINT_PTR array for a named donor, or NULL
 *  if the donor is not in the lookup table.
 * ==================================================================== */

const UINT_PTR* CSF_ResolveDonorVtable(const char* donorName);


/* ====================================================================
 *  Status registration API
 *
 *  Each custom status fills a CSF_StatusDef struct and passes it to
 *  CSF_RegisterStatus(). The framework uses this table to drive all
 *  hook behavior automatically.
 *
 *  Callbacks:
 *    build_vtable      -Clone donor RVAs to absolute, apply overrides.
 *                         If NULL and slot_overrides is set, the framework
 *                         uses CSF_DefaultBuildVtable which copies the base
 *                         donor then applies slot_overrides automatically.
 *    register_in_maps  -Insert into MAP0–MAP9 hash maps
 *    on_instance_created -Called after factory returns a new instance
 *                          (e.g. register in DLL-side tracking)
 *    hook_apply_status_ext -Called for EVERY non-custom apply_status.
 *                            Use for interception logic (e.g. a collector
 *                            that mirrors events to allies). May be NULL.
 * ==================================================================== */

#define CSF_MAX_STATUSES 16

typedef struct {
    /* Identity -used by WhichCustomStatus for name matching */
    const char* name;           /* e.g. "MyStatus"                       */
    int         name_len;       /* e.g. 8                                */

    /* Donor -which vanilla status factory to hijack */
    const char* donor_name;     /* e.g. "SoulLink"                       */
    UINT_PTR    donor_vt_rva;   /* e.g. RVA_SOULLINK_VT                  */

    /* Custom vtable -172 entries, filled by build_vtable */
    void**      custom_vtable;

    /* Mix-and-match slot overrides.
     * If slot_overrides is non-NULL and build_vtable is NULL, the
     * framework's default vtable builder copies the base donor's RVAs,
     * then applies each override (resolving donor names or installing
     * custom_fn pointers).
     * If both build_vtable and slot_overrides are set, it's the
     * build_vtable callback's responsibility to apply overrides. */
    CSF_SlotOverride* slot_overrides;
    int               slot_override_count;

    /* Allocated type IDs (filled by CSF_RegisterStatus) */
    UINT_PTR    type_id_leaf;   /* unique leaf ID for this status        */
    UINT_PTR    type_id_parent; /* parent ID (leaf + 1)                  */

    /* Lifecycle callbacks */
    void (*build_vtable)(void);
    void (*register_in_maps)(void);
    void (*on_instance_created)(void* instance);

    /* Hook extension: called for every non-custom apply_status.
     * Receives the raw hook args. May be NULL. */
    void (*hook_apply_status_ext)(void* name, void* owner, int stacks,
                                  void* gonObj, void* source);
} CSF_StatusDef;

/* Register a custom status with the framework. Call from CSF_RegisterAllStatuses(). */
void CSF_RegisterStatus(CSF_StatusDef* def);

/* Must be implemented by the status file(s). Called during framework init.
 * This is the single coupling point between framework and status definitions. */
void CSF_RegisterAllStatuses(void);

/* Access a registered status def by 0-based index.
 * Status files can use this after registration to read back allocated IDs. */
const CSF_StatusDef* CSF_GetStatusDef(int index);


/* ====================================================================
 *  Instance sidecar -per-instance data stored DLL-side
 *
 *  Avoids writing into the 0xF0-byte instance memory entirely. Each
 *  active custom status instance gets a sidecar slot allocated in
 *  on_instance_created and freed when the status is removed.
 *
 *  Behavioral slot overrides read parameters from the sidecar instead
 *  of hardcoded values. The sidecar's config values come from GON.
 * ==================================================================== */

#define CSF_MAX_INSTANCES 64

typedef struct {
    void*   instance;       /* key -pointer to 0xF0-byte pool instance   */
    void*   entity;         /* cached entity pointer (from instance+0x38) */
    int     status_def_idx; /* index into g_statusDefs                    */

    /* GON-driven parameters -populated from CSF_StatusConfig at creation.
     * 8 slots to match CSF_MAX_NAMED_PARAMS.  Named access via
     * CSF_SidecarInt/CSF_SidecarDbl is preferred. */
    int     int_params[8];  /* generic integer params (heal, damage, etc) */
    double  dbl_params[8];  /* generic double params (scale, threshold)   */
} CSF_InstanceSidecar;

/* Allocate a sidecar slot for a newly created instance.
 * Returns the sidecar pointer, or NULL if the table is full. */
CSF_InstanceSidecar* CSF_AllocSidecar(void* instance, int statusDefIdx);

/* Look up the sidecar for an existing instance. Returns NULL if not found. */
CSF_InstanceSidecar* CSF_GetSidecar(void* instance);

/* Free the sidecar for an instance being removed. */
void CSF_FreeSidecar(void* instance);


/* ====================================================================
 *  GON config -status parameters loaded from .gon.append data
 *
 *  Parsed lazily on first apply_status call (GON subsystem may not
 *  be ready at DLL init time). Each registered status can have a
 *  config block:
 *
 *    custom_status_config {
 *        MyStatus {
 *            int_param0  1       // first int parameter
 *            int_param1  0       // second int parameter
 *            dbl_param0  1.0     // first double parameter
 *        }
 *    }
 *
 *  The config populates default sidecar values for each status.
 * ==================================================================== */

#define CSF_GON_MAX_PARAMS 4

typedef struct {
    int     int_defaults[CSF_GON_MAX_PARAMS];
    double  dbl_defaults[CSF_GON_MAX_PARAMS];
} CSF_StatusConfig;

/* Force (re-)parse of GON config. Called automatically on first use.
 * Status files can call this after hot-reload if needed. */
void CSF_ParseGonConfig(void);

/* Set config for a status programmatically (alternative to GON).
 * Call after CSF_RegisterStatus(). defIndex is the 0-based index. */
void CSF_SetStatusConfig(int defIndex, const CSF_StatusConfig* cfg);

/* Get config for a status (used by sidecar allocation to copy defaults). */
const CSF_StatusConfig* CSF_GetStatusConfig(int defIndex);


/* ====================================================================
 *  GON API RVAs -discovered by DiscoverGonAPI.java + TraceGpakGlobal.java
 *
 *  These are used internally by the GON loader and are also available
 *  for status files that need direct GON object access at runtime.
 * ==================================================================== */

/* GON field lookup: GonObject* gon_field_lookup(GonObject*, MSVC_String* name)
 * Returns the named child of a GON object, or NULL if not found.
 * The GonObject pointer must be to a type=OBJECT or type=ARRAY node. */
#define RVA_GON_FIELD_LOOKUP    0x93EC00

/* GON struct layout constants (from MewgenicsModSdk, verified against disasm) */
#define GON_CHILDREN_VEC   0x38   /* std::vector<GonObject> (begin/end/cap) */
#define GON_INT_DATA       0x50
#define GON_FLOAT_DATA     0x58
#define GON_BOOL_DATA      0x60
#define GON_STRING_DATA    0x68   /* MSVC SSO string, 32 bytes              */
#define GON_NAME           0x88   /* MSVC SSO string, 32 bytes              */
#define GON_FIELD_TYPE     0xA8   /* enum: NULL=0,STRING=1,NUMBER=2,OBJECT=3,ARRAY=4,BOOL=5 */
#define GON_SIZEOF         0xB0   /* bytes per GonObject                    */

typedef void* (*fn_gon_field_lookup)(void* gonObj, void* nameStr);


/* ====================================================================
 *  GON-Driven Status Definitions
 *
 *  Define custom statuses entirely from a config file (GON format)
 *  without writing C code. The framework reads the file at init time
 *  and auto-registers each status with generic identity overrides,
 *  vtable assembly, and MAP0-MAP9 registration.
 *
 *  For statuses that need custom behavioral logic beyond slot mixing,
 *  a C extension registers by status name and slot number via
 *  CSF_RegisterExtension(). Cross-DLL extensions use the
 *  CSFCore_RegisterExtension export from csf_core_api.h, which resolves
 *  to the same underlying table.
 * ==================================================================== */

/* String length limit -must match GonParser.h if both are included.
 * Defined here with a guard so the header is self-contained. */
#ifndef GON_MAX_STRING_LEN
#define GON_MAX_STRING_LEN 64
#endif


/* -- Named sidecar parameters ------------------------------------
 *
 * GON-defined statuses use named parameters instead of positional
 * int_params[N] / dbl_params[N]. Each status definition has a param
 * name table stored in g_paramMaps[]. CSF_SidecarInt / CSF_SidecarDbl
 * look up a parameter by name, resolve its index inside the owning
 * status's CSF_ParamMap, and read the corresponding slot out of the
 * sidecar's int_params / dbl_params arrays.
 *
 * GON schema:
 *     params {
 *         damage          3
 *         heal_fraction   0.5
 *     }
 *
 * C access:
 *     int    dmg  = CSF_SidecarInt(sidecar, "damage");
 *     double heal = CSF_SidecarDbl(sidecar, "heal_fraction");
 *
 * Both accessors silently cross-cast int↔double if the requested
 * name exists with the other storage type. A missing name returns
 * the zero value (0 or 0.0) rather than signalling an error.
 */

#define CSF_MAX_NAMED_PARAMS 8
#define CSF_PARAM_NAME_LEN   32

typedef struct {
    char    name[CSF_PARAM_NAME_LEN];
    int     type;       /* 0 = int, 1 = double                 */
    int     int_val;    /* default value (int path)            */
    double  dbl_val;    /* default value (double path)         */
} CSF_ParamDesc;

typedef struct {
    CSF_ParamDesc descs[CSF_MAX_NAMED_PARAMS];
    int           count;
} CSF_ParamMap;

/* Look up a named integer parameter from a sidecar instance.
 * Returns 0 if the name is not present. */
int    CSF_SidecarInt(CSF_InstanceSidecar* sc, const char* paramName);

/* Look up a named double parameter from a sidecar instance.
 * Returns 0.0 if the name is not present. */
double CSF_SidecarDbl(CSF_InstanceSidecar* sc, const char* paramName);


/* -- C Extension Registration ------------------------------------
 *
 * For GON-defined statuses that need custom behavioral logic, a C
 * extension registers by status name and slot number. The framework
 * installs the function pointer after the GON loader builds the
 * status's vtable. Extensions for statuses not defined in GON are
 * silently ignored.
 *
 * Usage (from inside csf_core or a statically linked sibling TU):
 *   CSF_RegisterExtension("MyStatus",  5, MyStatus_destructor);
 *   CSF_RegisterExtension("MyStatus", 41, MyStatus_on_turn_end);
 *
 * Cross-DLL callers use CSFCore_RegisterExtension from csf_core_api.h,
 * which ends up at the same table through the exported wrapper.
 */

#define CSF_MAX_EXTENSIONS 64

typedef struct {
    char    status_name[GON_MAX_STRING_LEN];  /* e.g. "MyStatus" */
    int     slot_index;                        /* vtable slot 0-171 */
    void*   fn_ptr;                            /* function pointer */
} CSF_Extension;

/* Register a C extension for a GON-defined status.
 * The function pointer will be installed at the given vtable slot
 * after the GON loader builds the status's vtable. */
void CSF_RegisterExtension(const char* statusName, int slot, void* fnPtr);

/* Apply all registered C extensions to GON-defined status vtables.
 * Called internally after vtable builds complete; extensions can
 * call it manually after a hot-reload if needed. */
void CSF_ApplyAllExtensions(void);


/* -- Behavior registry -------------------------------------------
 *
 * Behaviors are named C function pointers an extension DLL publishes
 * once and that GON `slot_overrides` entries can then reference by
 * name, without any per-status RegisterExtension bind. The typical
 * publisher side lives in basic_custom_extensions.c (ships the
 * official behavior pack); the resolver side lives in
 * csf_core_gon_loader.c and is driven by CSF_FindBehavior /
 * CSF_ResolvePendingBehaviorBindings.
 *
 * Chain mode is fixed per behavior at publish time -GON authors
 * never see or choose it. CSF_CHAIN_AFTER calls the donor's original
 * slot fn first and then the behavior; CSF_CHAIN_REPLACE skips the
 * donor entirely. The constants live in csf_core_api.h so core and
 * extensions agree on the exact integer values.
 *
 * slot_index = -1 means "any compatible slot" -the behavior is
 * willing to be bound to whatever slot the GON author points it at,
 * as long as the slot's dispatcher family (see CSF_GetSlotDispatchInfo
 * in csf_core_gon_loader.c) matches the behavior's C signature. A
 * positive slot_index pins the behavior to exactly one slot and the
 * resolver refuses any GON binding that points elsewhere.
 *
 * Order-independence: GON slot_overrides can reference a behavior
 * whose publisher hasn't loaded yet. Those overrides stay pending
 * with behavior_fn = NULL, and the sweep triggered by a later
 * CSF_RegisterBehavior call re-scans all pending overrides and
 * binds any that now resolve.
 */

#define CSF_MAX_BEHAVIORS 64

typedef struct {
    char   name[GON_MAX_STRING_LEN]; /* publish-time identifier        */
    int    slot_index;               /* pinned slot, or -1 for any     */
    int    chain_mode;               /* CSF_CHAIN_AFTER / _REPLACE     */
    void*  fn;                       /* C function pointer             */
} CSF_Behavior;

/* Publish a behavior into the registry. Typically called from an
 * extension DLL's DllMain / init path via CSFCore_RegisterBehavior
 * (the exported wrapper in csf_core_api.h), but can be called
 * directly from any TU statically linked against csf_core.
 *
 * Duplicate names overwrite the existing entry. Pass slot_index = -1
 * to leave the target slot unconstrained. Returns 1 on success, 0
 * on failure (table full). Triggers CSF_ResolvePendingBehaviorBindings
 * internally so any overrides that referenced this name get bound
 * before the call returns. */
int  CSF_RegisterBehavior(const char* name, int slotIndex,
                          int chainMode, void* fn);

/* Look up a previously registered behavior by name. Returns NULL if
 * no behavior with that name has been registered yet. */
const CSF_Behavior* CSF_FindBehavior(const char* name);

/* Resolve any GON slot_overrides that referenced this behavior by
 * name before it was registered. Called automatically by
 * CSF_RegisterBehavior; extensions can call it manually after a
 * hot-reload to re-bind. Pass NULL to retry every pending override
 * regardless of behavior name. */
void CSF_ResolvePendingBehaviorBindings(const char* newlyRegisteredName);


/* -- GON loader entry point --------------------------------------
 *
 * Called during csf_core's Initialize() path, after any C-defined
 * statuses have registered and any C extensions have pre-registered
 * their function pointers. Reads custom_statuses.gon from disk and
 * auto-registers each status definition (donor clone, vtable
 * assembly, MAP0-MAP9 insertion, sidecar allocation hook,
 * named-parameter table).
 *
 * The config file is searched relative to the loaded mod DLL's own
 * folder first, then the fall-back locations documented in
 * csf_core_gon_loader.c.
 *
 * Returns the number of GON-defined statuses registered (0 if no
 * config file was found, which is a valid "pure C statuses" state).
 */

int CSF_LoadGonStatuses(void);

/* Vtable pool -pre-allocated storage for GON-defined status vtables.
 * Each GON status needs a persistent 172-entry void* array; the pool
 * is sized to match CSF_MAX_STATUSES so registration and loader
 * counts can never get out of sync. */
#define CSF_VTABLE_POOL_SIZE CSF_MAX_STATUSES


/* -- Generic identity overrides ----------------------------------
 *
 * Shared across every GON-defined status. Each one dispatches on
 * the caller's sidecar status_def_idx so a single C function can
 * stand in for the identity slots of every custom status without
 * needing a per-status closure. Installed by CSF_RegisterStatus
 * into the custom vtable at registration time.
 */

void*  CSF_GenericGetName(void* self, void* outStr);         /* slot 0  */
UINT64 CSF_GenericGetTypeId(void* self);                     /* slot 1  */
UINT64 CSF_GenericTypeNameCheck(void* self, void* nameStr);  /* slot 2  */
void*  CSF_GenericGetTypeInfo(void* self);                   /* slot 3  */
void   CSF_GenericGetDisplayName(void* self, void* outStr);
void*  CSF_GenericGetDisplayDesc(void* self, void* outBuf);  /* slot 28 */


#endif /* CUSTOM_STATUS_FRAMEWORK_H */