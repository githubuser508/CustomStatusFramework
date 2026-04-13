/*
 * csf_core_api.h -Custom Status Framework Core API for extension mods
 *
 * Include this in your extension DLL and call CSFCore_Resolve() during
 * init to get access to Core's coordination services.  Everything is
 * resolved at runtime from csf_core.dll -no import lib or build
 * dependency needed.  Matches the shape of mewjector.h intentionally.
 *
 * Usage:
 *
 *   #include "csf_core_api.h"
 *
 *   static CSFCoreAPI csf;
 *
 *   // In your DllMain or init function:
 *   if (!CSFCore_Resolve(&csf)) {
 *       OutputDebugStringA("csf_core.dll not loaded -extension disabled\n");
 *       return;
 *   }
 *
 *   // Bind vtable-slot overrides to an existing GON-declared status
 *   csf.RegisterExtension("Tuner",  5, (void*)AT_destructor_wrapper);
 *   csf.RegisterExtension("Tuner", 11, (void*)Tuner_dispatcher);
 *
 *   // Read the GON-parsed config for that status
 *   CSFStatusConfigView view = { sizeof(view) };
 *   if (csf.GetConfig("Tuner", &view))
 *       csf.Log("Tuner apply_mode = %d", view.apply_mode);
 *
 *
 * What Core gives you for free:
 *
 *   Core already hooks apply_status, resolve_status, get_status_flags,
 *   and primary_registration at framework priority.  Any custom status
 *   declared in a GON file is wired up automatically -donor cloning,
 *   vtable swap, sidecar bookkeeping, apply_mode enforcement (including
 *   the accumulate fix-up), all handled.
 *
 *   Extensions only exist when you need C code to replace specific
 *   vtable slots on a custom status (destructor, OnInit, LateUpdate,
 *   etc.).  A pure GON mod needs no DLL at all -drop the .gon file
 *   alongside csf_core.dll and it's live.
 *
 *
 * Versioning:
 *
 *   CSFCore_Resolve() gates on CSF_CORE_API_MIN_VERSION.  New exports
 *   are added as optional (null-checked by callers); existing ones
 *   never change signature.  CSFStatusConfigView is size-prefixed so
 *   appending fields stays backward-compatible.
 *
 *   CSFCore_Resolve() returns 0 if csf_core.dll is not loaded or too
 *   old.  Extensions should detect this and either refuse to load or
 *   fall back to standalone operation.
 */

#ifndef CSF_CORE_API_H
#define CSF_CORE_API_H

#include <windows.h>
#include <stdint.h>
#include <stdio.h>   /* sscanf for the "csf_core/active" token parse */
#include <string.h>  /* memset */

/* API version.  Extensions call CSFCore_Resolve(), which gates on
 * CSF_CORE_API_MIN_VERSION.  Optional exports (RegisterBehavior,
 * RequestRemoval, SidecarIntByName, SidecarDblByName) are resolved
 * with null-checks so extensions degrade when running against an
 * older core that lacks them. */
#define CSF_CORE_API_VERSION     0
/* Minimum core version an extension compiled against this header can
 * actually run on.  Bump only when a REQUIRED field is added. */
#define CSF_CORE_API_MIN_VERSION 0


/* -------------------------------------------------------------------
 *  Behavior chain-mode constants (mirror CSF_CHAIN_* in csf_core.h)
 *
 *  Extensions that want to publish named behaviors use these when
 *  calling RegisterBehavior.  Mode is hardcoded per-behavior at the
 *  extension side; GON authors never see or choose it.
 * ------------------------------------------------------------------- */
#define CSF_CHAIN_AFTER    0   /* donor original, then behavior          */
#define CSF_CHAIN_REPLACE  1   /* behavior only, donor original skipped  */


/* ===================================================================
 *  Apply mode -mirrors GON parser values in csf_core_gon_loader
 * =================================================================== */

#define CSF_APPLY_ACCUMULATE   0   /* existing stacks += new stacks       */
#define CSF_APPLY_PRESENCE     1   /* stacks unchanged on re-apply        */
#define CSF_APPLY_OVERWRITE    2   /* stacks replaced with new value      */
#define CSF_APPLY_CLONE_DONOR -1   /* inherit donor handler's semantics   */


/* ===================================================================
 *  Sidecar view -read-only snapshot of Core's per-instance state
 *
 *  Core owns a sidecar table keyed by game instance pointer for every
 *  custom status instance it creates.  Extensions can read the sidecar
 *  (e.g. from inside a slot override) via GetSidecarForInstance, which
 *  copies the relevant fields into this view.
 *
 *  The view is a copy, not a live reference -mutations to it do not
 *  flow back into Core.  Struct is size-prefixed for forward compat.
 * =================================================================== */

#define CSF_VIEW_MAX_PARAMS 8

typedef struct CSFSidecarView {
    uint32_t struct_size;
    void*    instance;
    void*    entity;
    int      status_def_idx;
    int      int_params[CSF_VIEW_MAX_PARAMS];
    double   dbl_params[CSF_VIEW_MAX_PARAMS];
} CSFSidecarView;


/* ===================================================================
 *  Read-only config view -returned by CSFCore_GetConfig
 *
 *  Mirrors the GON-parsed config Core stores internally.  Extensions
 *  can read default ints/floats/strings that the generic path already
 *  knows about.  The pointers in this struct are owned by Core and
 *  remain valid for process lifetime.
 * =================================================================== */

#define CSF_CONFIG_MAX_PARAMS 4

typedef struct CSFStatusConfigView {
    uint32_t    struct_size;
    const char* name;
    const char* donor_name;
    const char* display;
    const char* category;
    int         apply_mode;
    int         int_defaults[CSF_CONFIG_MAX_PARAMS];
    double      dbl_defaults[CSF_CONFIG_MAX_PARAMS];
} CSFStatusConfigView;


/* ===================================================================
 *  Vtable slot 80 -OnElementInfluence element bitmask
 *
 *  Signature observed across all donor overrides:
 *      void slot80(CSF_Instance* self, const CSF_ElementMask* mask);
 *
 *  The mask is 8 bytes -two uint32 words.  Status donors inspect bits
 *  in either word to react to tile/area element state: fire ignites or
 *  extinguishes, water dries, cold freezes, etc.
 *
 *  Bit meanings were reverse-engineered from behavioral pattern-matching
 *  across six donor slot-80 implementations (Burn, Freeze, Tangled,
 *  FireArmor, Wet, Tarred, IceArmor).  Not every bit has been seen in
 *  the wild yet -entries marked OBSERVED_BIT_n are recognised-but-
 *  unnamed.  When a new donor surfaces a new bit, extend this table.
 *
 *  Canonical interpretation (low word):
 *      bit 2 (0x00000004) ELEMENT_FIRE         -burns webs (Tangled),
 *                                                consumes tar (Tarred),
 *                                                dries water (Wet),
 *                                                melts ice (IceArmor,
 *                                                Freeze)
 *      bit 3 (0x00000008) ELEMENT_WATER        -extinguishes burn
 *                                                (Burn), extinguishes
 *                                                fire armor (FireArmor)
 *      bit 5 (0x00000020) ELEMENT_COLD         -dries/freezes wet
 *                                                tiles alongside fire
 *                                                (Wet checks 0x24)
 *      bit 7 (0x00000080) ELEMENT_FIRE_FUEL    -secondary "more fire"
 *                                                signal: extends Burn
 *                                                duration and stacks
 *                                                FireArmor (distinct
 *                                                from bit 2 ignite)
 *      bit 26 (0x04000000) OBSERVED_BIT_26     -Tangled branches into
 *                                                FUN_140803ef0(self,1)
 *                                                when fire is absent
 *                                                but this bit is set;
 *                                                probable electric-
 *                                                ignition of webs
 *
 *  Canonical interpretation (high word, param_2[1]):
 *      bit 1 (0x00000002) ELEMENT_HEAT         -melts IceArmor and
 *                                                Freeze as a fallback
 *                                                trigger distinct from
 *                                                the word-0 fire bit
 *                                                (both donors loop
 *                                                over {word0&4, word1&2})
 *
 *  Because the field is 64 bits spread over two words, the enum uses
 *  explicit masks rather than shift positions.  Consumers can test via
 *
 *      if (mask->w0 & CSF_ELEMENT_FIRE) { ... }
 *      if (mask->w1 & CSF_ELEMENT_HEAT) { ... }
 * =================================================================== */

typedef struct CSF_ElementMask {
    uint32_t w0;   /* low word -param_2[0] */
    uint32_t w1;   /* high word -param_2[1] */
} CSF_ElementMask;

/* Low-word bits (CSF_ElementMask.w0) */
#define CSF_ELEMENT_FIRE            0x00000004u  /* bit 2  */
#define CSF_ELEMENT_WATER           0x00000008u  /* bit 3  */
#define CSF_ELEMENT_COLD            0x00000020u  /* bit 5  */
#define CSF_ELEMENT_FIRE_FUEL       0x00000080u  /* bit 7  */
#define CSF_ELEMENT_OBSERVED_BIT_26 0x04000000u  /* bit 26 -Tangled */

/* High-word bits (CSF_ElementMask.w1) */
#define CSF_ELEMENT_HEAT            0x00000002u  /* bit 1  */


/* ===================================================================
 *  Vtable slot 81 -GetImmunity immunity bitmask
 *
 *  Signature observed across all donor overrides:
 *      CSF_ImmunityMask* slot81(CSF_Instance* self, CSF_ImmunityMask* out);
 *
 *  The mask is 8 bytes -two uint32 words.  A status that grants
 *  immunity writes one or more bits into out->w0 / out->w1 to advertise
 *  which status categories the bearer is currently immune to.  The
 *  caller (status-apply path) rejects incoming statuses whose category
 *  bit is set.
 *
 *  The base-vtable default at 0x75cc60 writes zero (no immunities).
 *  The bit meanings are anchored by the donors that uniquely own each
 *  category:
 *
 *      bit 2  (0x00000004) CSF_IMMUNITY_BURN       -Burn slot 81
 *      bit 3  (0x00000008) CSF_IMMUNITY_WET        -Wet slot 81 writes
 *                                                    |= 0x8 (so wet
 *                                                    bearers can't be
 *                                                    re-wetted)
 *      bit 5  (0x00000020) CSF_IMMUNITY_COLD       -IceArmor slot 81
 *                                                    (and Freeze, via
 *                                                    the 0x300020 mask)
 *      bit 9  (0x00000200) CSF_IMMUNITY_PETRIFY    -Petrify slot 81
 *                                                    (part of 0x300200)
 *      bit 14 (0x00004000) CSF_IMMUNITY_BLEED      -Bleed slot 81
 *      bit 20 (0x00100000) CSF_IMMUNITY_IMMOBILIZE -Immobile-group
 *                                                    slot 81
 *      bit 21 (0x00200000) CSF_IMMUNITY_INCAPACITATE -Incap-group
 *                                                      slot 81 (adds
 *                                                      to Immobilize
 *                                                      via 0x300000)
 *
 *  Composite masks observed in the wild:
 *      0x00100000 Immobile  = IMMOBILIZE
 *      0x00300000 Incap     = IMMOBILIZE | INCAPACITATE
 *      0x00300200 Petrify   = IMMOBILIZE | INCAPACITATE | PETRIFY
 *      0x00300020 Freeze    = IMMOBILIZE | INCAPACITATE | COLD
 *
 *  High-word bits (param_2[1]):
 *      bit 5  (0x00000020) CSF_IMMUNITY_W1_BIT_5   -Wet slot 81 also
 *                                                    writes w1 |= 0x20;
 *                                                    semantics TBD
 * =================================================================== */

typedef struct CSF_ImmunityMask {
    uint32_t w0;   /* low word -param_2[0] */
    uint32_t w1;   /* high word -param_2[1] */
} CSF_ImmunityMask;

/* Low-word bits (CSF_ImmunityMask.w0) */
#define CSF_IMMUNITY_BURN           0x00000004u  /* bit 2  */
#define CSF_IMMUNITY_WET            0x00000008u  /* bit 3  */
#define CSF_IMMUNITY_COLD           0x00000020u  /* bit 5  */
#define CSF_IMMUNITY_PETRIFY        0x00000200u  /* bit 9  */
#define CSF_IMMUNITY_BLEED          0x00004000u  /* bit 14 */
#define CSF_IMMUNITY_IMMOBILIZE     0x00100000u  /* bit 20 */
#define CSF_IMMUNITY_INCAPACITATE   0x00200000u  /* bit 21 */

/* Common composites from observed donors */
#define CSF_IMMUNITY_INCAP_GROUP    (CSF_IMMUNITY_IMMOBILIZE | CSF_IMMUNITY_INCAPACITATE)               /* 0x00300000 */
#define CSF_IMMUNITY_PETRIFY_MASK   (CSF_IMMUNITY_INCAP_GROUP | CSF_IMMUNITY_PETRIFY)                   /* 0x00300200 */
#define CSF_IMMUNITY_FREEZE_MASK    (CSF_IMMUNITY_INCAP_GROUP | CSF_IMMUNITY_COLD)                      /* 0x00300020 */

/* High-word bits (CSF_ImmunityMask.w1) */
#define CSF_IMMUNITY_W1_BIT_5       0x00000020u  /* bit 5 -Wet only */


/* ===================================================================
 *  Vtable slot 82 -stat mutation offset table
 *
 *  Signature observed across all donor overrides:
 *      void slot82(CSF_Instance* self);
 *
 *  Slot 82 mutates one or more stats on the bearer's stat block.  The
 *  seven core stats live in a contiguous int32 table at bearer+0x5bc:
 *
 *      +0x5bc  Strength       (StrengthUp     / TempStrengthUp  / Weakness -2)
 *      +0x5c0  Dexterity      (DexterityUp    / TempDexterityUp / Weakness -2)
 *      +0x5c4  Constitution   (ConstitutionUp)
 *      +0x5c8  Intelligence   (IntelligenceUp)
 *      +0x5cc  Speed          (SpeedUp        / TempSpeedUp     / Bound=0 /
 *                              Weakness -2     / Tarred -counter)
 *      +0x5d0  Charisma       (CharismaUp)
 *      +0x5d4  Luck           (LuckUp         / TempLuckUp     /
 *                              NextActionLuckUp)
 *
 *  The bearer pointer is reached via either
 *      bearer = instance[7]            (typed longlong* -slot 82a donors)
 *      bearer = *(longlong*)(inst+0x38)  (typed longlong  -82b donors)
 *  These are the same field: vtable[7] and +0x38 both index the bearer.
 *
 *  Canonical pattern (example: TempStrengthUp @ 0x140803570):
 *      *(int*)(bearer + 0x5bc) += *(int*)(instance + 0x5c);
 *
 *  AllStatsUp rolls through the whole table, writing to all 7 offsets.
 *  Zombie subtracts 2 from both Strength (+0x5bc) and Intelligence
 *  (+0x5c8) to model the intelligence/strength profile swap.
 *
 *  Other slot-82 writers touch fields OUTSIDE the core stat table:
 *      AlphaCat                 bearer+0xc98  (non-stat flag)
 *      TilesMovedToStrength     bearer+0x5bc += instance+0xf0  (Strength
 *                                               scaled by a tile counter
 *                                               in the instance)
 *      NextActionLuckUp         gated on instance+0xf0 char flag, then
 *                                          bearer+0x5d4 += instance+0x5c
 *
 *  Many slot-82 donors also run the shared "counter decremented to 0"
 *  epilogue: set instance+0x75 = 1 (remove latch), set bearer+0xe50 = 1
 *  (bearer-side flag), then call vtable slot 45 (OnRemove).
 * =================================================================== */

/* Byte offsets into the bearer struct for each stat */
#define CSF_STAT_STRENGTH       0x5bc
#define CSF_STAT_DEXTERITY      0x5c0
#define CSF_STAT_CONSTITUTION   0x5c4
#define CSF_STAT_INTELLIGENCE   0x5c8
#define CSF_STAT_SPEED          0x5cc
#define CSF_STAT_CHARISMA       0x5d0
#define CSF_STAT_LUCK           0x5d4

/* Symbolic indices -useful for iterating over the full stat block.
 * The bearer stat array starts at CSF_STAT_STRENGTH and runs 7 entries
 * of int32 (total 0x1c bytes).  AllStatsUp uses a loop over this range
 * in the wild. */
typedef enum {
    CSF_STAT_IDX_STRENGTH     = 0,
    CSF_STAT_IDX_DEXTERITY    = 1,
    CSF_STAT_IDX_CONSTITUTION = 2,
    CSF_STAT_IDX_INTELLIGENCE = 3,
    CSF_STAT_IDX_SPEED        = 4,
    CSF_STAT_IDX_CHARISMA     = 5,
    CSF_STAT_IDX_LUCK         = 6,
    CSF_STAT_COUNT            = 7
} CSF_StatIndex;

#define CSF_STAT_BLOCK_BASE   CSF_STAT_STRENGTH  /* bearer+0x5bc */
#define CSF_STAT_BLOCK_STRIDE 4                   /* int32 per stat */

/* Convert a CSF_StatIndex to its bearer byte offset */
#define CSF_STAT_OFFSET(idx)  (CSF_STAT_BLOCK_BASE + (int)(idx) * CSF_STAT_BLOCK_STRIDE)

/* Bearer pointer access -instance+0x38 typed as void*. Both vtable[7]
 * and +0x38 index the same slot; use the +0x38 form here because it
 * doesn't require the instance's vtable to be valid. */
#define CSF_BEARER(inst)          (*(void**)((char*)(inst) + 0x38))

/* Instance counter field used by slot-82 donors as the "amount to
 * add/subtract" per stat bonus.  Read as int32. */
#define CSF_INSTANCE_COUNTER      0x5c


/* ===================================================================
 *  Function typedefs -C-style to match mewjector.h
 * =================================================================== */

typedef int (__cdecl *CSFCore_fn_GetVersion)(void);

/* Bind a C function pointer to a specific vtable slot on a named
 * custom status.  Must be called before Core builds the status vtable
 * (i.e. during extension DllMain, before csf_core finishes Initialize).
 * Returns 1 on success, 0 if the status name is unknown. */
typedef int (__cdecl *CSFCore_fn_RegisterExtension)(
    const char* statusName,
    int         slotIndex,
    void*       fnPtr
);

/* Copy a sidecar snapshot into out_view.  Returns 1 if the instance
 * has a live sidecar, 0 otherwise (out_view is untouched on failure).
 * Caller sets out_view->struct_size before calling. */
typedef int (__cdecl *CSFCore_fn_GetSidecarForInstance)(
    void*           instance,
    CSFSidecarView* out_view
);

/* Read Core's parsed config for a status by name.  Returns 1 on
 * success, 0 if the status isn't registered.  Caller sets
 * out_view->struct_size before calling. */
typedef int (__cdecl *CSFCore_fn_GetConfig)(
    const char*          statusName,
    CSFStatusConfigView* out_view
);

/* Write a formatted line to Core's log.  Shares the same log file
 * as Core's own SLog output; prefixed with a timestamp automatically. */
typedef void (__cdecl *CSFCore_fn_Log)(
    const char* fmt,
    ...
);

/* Return the game image base address.  Equivalent to
 * GetModuleHandleA(NULL) but cached and cheap. */
typedef UINT_PTR (__cdecl *CSFCore_fn_GetGameBase)(void);

/* Look up a vtable slot function on a donor (vanilla) status by name.
 * Returns the absolute address of the donor's original implementation
 * of that slot, suitable for direct invocation.  Returns NULL if the
 * donor is not in the lookup table or the slot index is out of range.
 *
 * Used by extensions that want to chain through to the donor's
 * original slot behavior after doing their own work (e.g. a custom
 * destructor that calls the vanilla destructor after cleanup).
 *
 * Slot count is 172 for every vanilla status in the current build. */
typedef void* (__cdecl *CSFCore_fn_GetDonorSlotFn)(
    const char* donorName,
    int         slotIndex
);

/* Publish a named behavior to the registry so GON slot_overrides can
 * reference it without a hand-written RegisterExtension bind per
 * status.  The name is a short identifier (fits in GON_MAX_STRING_LEN,
 * currently 64 chars) that matches what a GON author would write as
 * the third argument of a slot_overrides entry.
 *
 *   name        the identifier GON will reference
 *   slotIndex   the slot this behavior is DESIGNED for; used for
 *               sanity-checking bindings at vtable-build time. Pass
 *               -1 to mean "any slot compatible with this signature"
 *   chainMode   CSF_CHAIN_AFTER or CSF_CHAIN_REPLACE (see above).
 *               Hardcoded per-behavior at registration time; not
 *               exposed to GON authors.
 *   fn          the C function pointer implementing the behavior.
 *               Its signature must match whatever the target slot
 *               expects on the game side.
 *
 * Returns 1 on success, 0 if the registry is full, the name is
 * invalid, or the core lacks this export (in which case the function
 * pointer itself is NULL and the extension should fall back to
 * hand-written RegisterExtension binds).
 *
 * Idempotent: calling with an already-registered name replaces the
 * binding and triggers a re-sweep of pending overrides.  Optional. */
typedef int (__cdecl *CSFCore_fn_RegisterBehavior)(
    const char* name,
    int         slotIndex,
    int         chainMode,
    void*       fn
);

/* Request the engine remove a status instance from its owning
 * entity's status list.  Thin wrapper over the game-internal
 * request_removal routine (FUN_14075c660 in the current build,
 * resolved once via g_gameBase at call time).  Safe to call from
 * inside a slot override -the engine handles the actual teardown
 * and destructor chain.  Pass the instance pointer the slot received
 * as `self`.
 *
 * Null-check api->RequestRemoval before invoking; older cores resolve
 * NULL and extensions that need removal should degrade (e.g. no-op
 * the tick behavior, or hard-code the RVA themselves).  Optional. */
typedef void (__cdecl *CSFCore_fn_RequestRemoval)(
    void* instance
);

/* Read a named GON parameter off a live custom-status instance.
 * Looks up the instance's sidecar, then the parameter name in the
 * status definition's CSF_ParamMap, and returns the value stored
 * at the resolved index.  Works across int/double storage: if the
 * named parameter is declared as a double, SidecarIntByName
 * returns a truncated int view of it and vice versa, matching the
 * internal CSF_SidecarInt/CSF_SidecarDbl semantics.
 *
 *   instance    the instance pointer the slot received as `self`
 *   paramName   the exact name authored in the GON `params { ... }`
 *               block, e.g. "damage" or "heal_fraction"
 *
 * Returns 0 / 0.0 if the instance has no sidecar, the status has
 * no param map, or the name isn't in the map.  There is no
 * separate error channel -extensions that need to distinguish
 * "absent" from "zero" should check the status's GON config up
 * front via GetConfig instead.
 *
 * Both helpers are the named counterparts to the positional
 * int_params[] / dbl_params[] in CSFSidecarView.  Extensions that
 * already know the slot index of a parameter can keep using the
 * view; extensions that want to match GON author intent directly
 * should prefer these.
 *
 * Optional. */
typedef int (__cdecl *CSFCore_fn_SidecarIntByName)(
    void*       instance,
    const char* paramName
);

typedef double (__cdecl *CSFCore_fn_SidecarDblByName)(
    void*       instance,
    const char* paramName
);


/* ===================================================================
 *  API struct -filled by CSFCore_Resolve()
 * =================================================================== */

typedef struct {
    CSFCore_fn_GetVersion            GetVersion;
    CSFCore_fn_RegisterExtension     RegisterExtension;
    CSFCore_fn_GetSidecarForInstance GetSidecarForInstance;
    CSFCore_fn_GetConfig             GetConfig;
    CSFCore_fn_Log                   Log;
    CSFCore_fn_GetGameBase           GetGameBase;
    CSFCore_fn_GetDonorSlotFn        GetDonorSlotFn;
    CSFCore_fn_RegisterBehavior      RegisterBehavior;   /* optional */
    CSFCore_fn_RequestRemoval        RequestRemoval;     /* optional */
    CSFCore_fn_SidecarIntByName      SidecarIntByName;   /* optional */
    CSFCore_fn_SidecarDblByName      SidecarDblByName;   /* optional */
} CSFCoreAPI;


/* ===================================================================
 *  CSFCore_Resolve -Resolve all API functions from csf_core.dll
 *
 *  Returns 1 if all functions resolved successfully.
 *  Returns 0 if Core is not loaded or is too old (safe to call always).
 *
 *  This is a static inline so the header is self-contained -no
 *  separate .c file or import lib needed.
 * =================================================================== */

/* -- Internal helper: locate the ACTIVE csf_core.dll instance --
 *
 * Multi-instance support: multiple csf_core.dll copies may be loaded
 * from different mod folders, but only one wins the "active" singleton
 * and owns the real status table.  The winner publishes its HMODULE
 * as a hex token in Mewjector's name table under "csf_core/active".
 *
 * We try three strategies, in order:
 *   1. Look up "csf_core/active" in Mewjector (version.dll) and parse
 *      the HMODULE hex token.  This is the preferred path and directly
 *      returns the winning instance.
 *   2. Enumerate all modules via EnumProcessModules, keep every one
 *      whose base name is csf_core.dll, and probe each with
 *      CSFCore_IsActive().  Used when Mewjector isn't loaded.
 *   3. Fall back to GetModuleHandleA("csf_core.dll") for legacy
 *      single-instance deployments.
 */
static HMODULE CSFCore__FindActiveModule(void)
{
    HMODULE hCore = NULL;

    /* -- Strategy 1: Mewjector name-table lookup -- */
    {
        HMODULE hMJ = GetModuleHandleA("version.dll");
        if (hMJ) {
            typedef const char* (__cdecl *LookupFn)(const char*, const char*);
            LookupFn lookup = (LookupFn)GetProcAddress(hMJ, "MJ_LookupName");
            if (lookup) {
                const char* tok = lookup("csf_core", "active");
                if (tok) {
                    unsigned long long addr = 0;
                    /* Token format: "0x<hex>" (see csf_core.c Initialize) */
                    if (sscanf(tok, "0x%llx", &addr) == 1 && addr) {
                        hCore = (HMODULE)(UINT_PTR)addr;
                    }
                }
            }
        }
    }
    if (hCore) return hCore;

    /* -- Strategy 3: Legacy single-instance fallback --
     *
     * We skip EnumProcessModules enumeration here (would require linking
     * psapi) because the name-table lookup succeeds whenever Mewjector
     * is present, which is the only supported multi-instance scenario.
     * If Mewjector isn't loaded, we're in standalone mode and there
     * should be only one csf_core anyway. */
    hCore = GetModuleHandleA("csf_core.dll");
    return hCore;
}

static int CSFCore_Resolve(CSFCoreAPI* api)
{
    HMODULE hCore;
    if (!api) return 0;
    memset(api, 0, sizeof(CSFCoreAPI));

    hCore = CSFCore__FindActiveModule();
    if (!hCore) return 0;

    /* Version gate. */
    api->GetVersion = (CSFCore_fn_GetVersion)GetProcAddress(hCore, "CSFCore_GetVersion");
    if (!api->GetVersion) return 0;
    if (api->GetVersion() < CSF_CORE_API_MIN_VERSION) return 0;

    /* Required exports: fail the resolve if any are missing. */
    #define CSF__RESOLVE(field) \
        api->field = (CSFCore_fn_##field)GetProcAddress(hCore, "CSFCore_" #field); \
        if (!api->field) return 0;

    CSF__RESOLVE(RegisterExtension);
    CSF__RESOLVE(GetSidecarForInstance);
    CSF__RESOLVE(GetConfig);
    CSF__RESOLVE(Log);
    CSF__RESOLVE(GetGameBase);
    CSF__RESOLVE(GetDonorSlotFn);

    #undef CSF__RESOLVE

    /* Optional exports: a NULL pointer is acceptable and callers must
     * null-check before calling.  Used for API surface that an
     * extension can degrade without -behaviors fall back to hand-
     * written RegisterExtension, etc. */
    #define CSF__RESOLVE_OPT(field) \
        api->field = (CSFCore_fn_##field)GetProcAddress(hCore, "CSFCore_" #field);

    CSF__RESOLVE_OPT(RegisterBehavior);
    CSF__RESOLVE_OPT(RequestRemoval);
    CSF__RESOLVE_OPT(SidecarIntByName);
    CSF__RESOLVE_OPT(SidecarDblByName);

    #undef CSF__RESOLVE_OPT
    return 1;
}


/* ===================================================================
 *  CSFCore_RequestMergeFromDir / CSFCore_MergeGonFromMyDir
 *
 *  Tell the active csf_core to load and merge a custom_statuses.gon
 *  from an arbitrary directory on disk.  Needed when an extension DLL
 *  ships WITHOUT a csf_core.dll next to it -in that case core only
 *  knows about the GON in its OWN directory (wherever the mod that
 *  provides core lives), and will never find the extension's GON on
 *  its own.  The extension explicitly hands over its directory here.
 *
 *  The export is NOT part of the CSFCoreAPI struct because it's an
 *  extension-to-core request and doesn't need the version-gated
 *  resolution path.  The helper resolves the active core module via
 *  CSFCore__FindActiveModule and GetProcAddress's
 *  CSFCore_MergeExternalGon directly.
 *
 *  Both helpers are safe to call with csf_core missing or too old;
 *  they return 0 instead of crashing.
 * =================================================================== */

static int CSFCore_RequestMergeFromDir(const char* modDir)
{
    HMODULE hCore;
    typedef int (__cdecl *CSFCore_fn_MergeExternalGon)(const char* modDir);
    CSFCore_fn_MergeExternalGon merge;

    if (!modDir) return 0;
    hCore = CSFCore__FindActiveModule();
    if (!hCore) return 0;

    merge = (CSFCore_fn_MergeExternalGon)
        GetProcAddress(hCore, "CSFCore_MergeExternalGon");
    if (!merge) return 0;

    return merge(modDir);
}

static int CSFCore_MergeGonFromMyDir(HMODULE hSelf)
{
    char  path[MAX_PATH];
    DWORD len;
    char* lastBack;
    char* lastFwd;
    char* lastSep;

    if (!hSelf) return 0;

    len = GetModuleFileNameA(hSelf, path, (DWORD)sizeof(path));
    if (len == 0 || len >= sizeof(path)) return 0;

    lastBack = strrchr(path, '\\');
    lastFwd  = strrchr(path, '/');
    lastSep  = (lastBack > lastFwd) ? lastBack : lastFwd;
    if (!lastSep) return 0;

    *(lastSep + 1) = '\0';

    return CSFCore_RequestMergeFromDir(path);
}


/* ===================================================================
 *  CSFCore_InitOrWait -Deferred extension bind helper
 *
 *  Motivation: A mod author can ship a csf_ext_*.dll WITHOUT bundling
 *  csf_core.dll in the same directory, relying on another installed
 *  mod to provide core.  In that situation the extension's DllMain may
 *  run BEFORE any csf_core has loaded, so a direct CSFCore_Resolve()
 *  call would fail.  This helper schedules a background thread-pool
 *  timer that re-polls for csf_core periodically and invokes the
 *  caller's init callback once core is ready (and the version check
 *  passes).  It's safe to call from DllMain -the thread-pool timer
 *  API is explicitly documented as loader-lock safe.
 *
 *  Semantics of the caller's initFn:
 *    - Returns 1 when its work is complete and no more retries are
 *      needed (e.g. all desired RegisterExtension calls succeeded).
 *    - Returns 0 to keep retrying -useful when csf_core is ready but
 *      the target status(es) aren't registered yet because the mod
 *      that defines them hasn't loaded its own csf_core yet.  The
 *      initFn MUST be idempotent: it may be called many times, each
 *      time progressing as far as it can.  Partial success is fine;
 *      just track which slots are already bound and skip them.
 *
 *  Semantics of CSFCore_InitOrWait itself:
 *    - Attempts a synchronous Resolve + initFn call first.
 *    - If that returns 1, we're done -returns 1.
 *    - Otherwise schedules a repeating thread-pool timer that retries
 *      every CSF__RETRY_INTERVAL_MS until initFn returns 1 or we hit
 *      CSF__RETRY_MAX_ATTEMPTS.  Returns 0 in that case (deferred).
 *    - Returns 0 on timer API failure (extension quietly disabled).
 *
 *  The static storage is per-DLL (each extension DLL has its own copy
 *  of these statics), which means one extension per DLL can use this
 *  helper.  Extensions that need multiple independent deferred binds
 *  should roll their own.
 * =================================================================== */

#ifndef CSF__RETRY_INTERVAL_MS
#define CSF__RETRY_INTERVAL_MS   250
#endif
#ifndef CSF__RETRY_MAX_ATTEMPTS
#define CSF__RETRY_MAX_ATTEMPTS  240   /* 240 * 250ms = 60 seconds */
#endif

static PTP_TIMER  csf__retryTimer    = NULL;
static int        csf__retryAttempts = 0;
static int        csf__retryDone     = 0;
static CSFCoreAPI csf__retryApi;
static int (*csf__retryInitFn)(CSFCoreAPI* api) = NULL;

static VOID CALLBACK CSFCore__RetryCallback(
    PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer)
{
    (void)instance;
    (void)context;
    (void)timer;

    if (csf__retryDone) return;
    csf__retryAttempts++;

    if (CSFCore_Resolve(&csf__retryApi) &&
        csf__retryInitFn &&
        csf__retryInitFn(&csf__retryApi))
    {
        /* Success -stop polling.  We must not call CloseThreadpoolTimer
         * or WaitForThreadpoolTimerCallbacks from inside the callback
         * itself (would deadlock).  Instead, cancel the timer so it
         * won't fire again, and mark ourselves done.  The timer object
         * itself leaks until DLL unload, which is acceptable. */
        SetThreadpoolTimer(csf__retryTimer, NULL, 0, 0);
        csf__retryDone = 1;
        OutputDebugStringA("[csf_core_api] deferred init succeeded\n");
        return;
    }

    if (csf__retryAttempts >= CSF__RETRY_MAX_ATTEMPTS)
    {
        SetThreadpoolTimer(csf__retryTimer, NULL, 0, 0);
        csf__retryDone = 1;
        OutputDebugStringA("[csf_core_api] deferred init timed out "
                           "(csf_core never became ready, or target "
                           "status(es) never appeared)\n");
    }
}

static int CSFCore_InitOrWait(int (*initFn)(CSFCoreAPI* api))
{
    LARGE_INTEGER due;
    FILETIME      ft;

    if (!initFn) return 0;

    /* 1. Synchronous fast path -csf_core is already loaded and the
     *    extension's init callback succeeds immediately.  This is the
     *    common case for mods that ship csf_core alongside their
     *    extension DLL, or for the second-and-later extensions in a
     *    given mod directory (csf_core already resolved by the first). */
    if (CSFCore_Resolve(&csf__retryApi))
    {
        if (initFn(&csf__retryApi))
        {
            csf__retryDone = 1;
            return 1;
        }
        /* csf_core is loaded but initFn returned 0 -probably because
         * the target status isn't registered yet.  Fall through to the
         * asynchronous retry path. */
    }

    /* 2. Defer: create a repeating thread-pool timer.  The first tick
     *    fires after CSF__RETRY_INTERVAL_MS, and every tick after that
     *    at the same interval.  CreateThreadpoolTimer is explicitly
     *    documented as DllMain-safe because the thread-pool workers
     *    already exist and the timer object doesn't need to create any
     *    new threads. */
    csf__retryInitFn   = initFn;
    csf__retryAttempts = 0;
    csf__retryDone     = 0;

    csf__retryTimer = CreateThreadpoolTimer(CSFCore__RetryCallback, NULL, NULL);
    if (!csf__retryTimer)
    {
        OutputDebugStringA("[csf_core_api] CreateThreadpoolTimer failed -"
                           "deferred init abandoned\n");
        return 0;
    }

    /* Negative FILETIME = relative time, in 100ns units.  Convert our
     * millisecond interval to 100ns units and negate. */
    due.QuadPart = -((LONGLONG)CSF__RETRY_INTERVAL_MS * 10000LL);
    ft.dwLowDateTime  = (DWORD) due.u.LowPart;
    ft.dwHighDateTime = (DWORD) due.u.HighPart;

    SetThreadpoolTimer(csf__retryTimer, &ft, CSF__RETRY_INTERVAL_MS, 0);

    OutputDebugStringA("[csf_core_api] csf_core not ready at DllMain time -"
                       "scheduled retry timer (250ms interval, ~60s cap)\n");
    return 0;
}


/* ===================================================================
 *  CSFCore_Shutdown -Tear down the deferred init retry timer
 *
 *  Call from DLL_PROCESS_DETACH in the extension's DllMain to make
 *  sure any pending retry callbacks are cancelled and drained before
 *  the extension DLL's code pages are unmapped.  Without this, a
 *  retry tick that happens to fire during unload would call
 *  CSFCore_Resolve / the caller's initFn on code that's about to
 *  disappear, resulting in a crash during process shutdown or
 *  FreeLibrary.
 *
 *  NOTE on DllMain semantics: DLL_PROCESS_DETACH runs under loader
 *  lock, and WaitForThreadpoolTimerCallbacks(TRUE) is documented as
 *  safe to call from there for thread-pool timers created earlier in
 *  the same DLL's lifetime.  (CloseThreadpoolTimer is also DllMain-
 *  safe for the same reason the Create call was.)  If we ever add a
 *  code path that calls this from outside DllMain, the order stays
 *  the same.
 *
 *  The function is idempotent -calling it on an already-shut-down
 *  extension is a no-op.  It's also safe if CSFCore_InitOrWait was
 *  never called or never created a timer (csf__retryTimer is NULL).
 * =================================================================== */

static void CSFCore_Shutdown(void)
{
    PTP_TIMER timer;

    /* Mark done FIRST so any in-flight callback that races us will
     * bail out at its first-line `if (csf__retryDone) return;` check
     * instead of trying to call initFn on pages we're about to
     * unload.  This is belt-and-braces on top of the cancel below. */
    csf__retryDone = 1;

    timer = csf__retryTimer;
    if (!timer) return;

    /* Cancel any future ticks.  Passing NULL for the due time tells
     * the thread pool to stop the timer; the third (period) and
     * fourth (window length) arguments are ignored when cancelling. */
    SetThreadpoolTimer(timer, NULL, 0, 0);

    /* Block until any callback already running finishes.  The TRUE
     * flag also cancels pending callbacks that haven't started yet.
     * Without this wait, the DLL code pages could be unmapped while
     * a callback is mid-execution. */
    WaitForThreadpoolTimerCallbacks(timer, TRUE);

    /* Release the timer object itself. */
    CloseThreadpoolTimer(timer);

    csf__retryTimer  = NULL;
    csf__retryInitFn = NULL;
}


#endif /* CSF_CORE_API_H */
