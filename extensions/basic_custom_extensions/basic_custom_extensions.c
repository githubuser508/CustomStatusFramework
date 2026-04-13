/*
 * basic_custom_extensions.c -- CSF official behavior pack
 *
 * A catch-all extension DLL that ships alongside csf_core and provides
 * plug-and-play slot behaviors a modder can attach to their own
 * GON-declared custom statuses without writing any C code.
 *
 *   - Wiring model: the behavior-name registry.  csf_core exports
 *     CSFCore_RegisterBehavior; BCE_RegisterBehaviors publishes every
 *     entry in g_bceBehaviors through that export so GON slot_overrides
 *     can reference behaviors by name.  The modder writes the behavior
 *     name as the third token of a slot_overrides entry and the core's
 *     name registry + per-slot chain dispatcher does the rest -no
 *     hand-written RegisterExtension bind per status required.
 *
 *   - Ships 26 behaviors today, grouped into five families:
 *
 *       (1) Lifecycle / infrastructure (3):
 *           CSFExt_DeregisterFromDonorPool (slot 33 OnInit, AFTER),
 *           CSFExt_No_Op (any VoidSelf slot, REPLACE),
 *           CSFExt_No_Op_Destructor (slot 5, REPLACE).
 *
 *       (2) Generic tick counter mutation (4):
 *           CSFExt_TickDecrementRemoveAtZero,
 *           CSFExt_TickDecrement (alias for the above),
 *           CSFExt_TickIncrement,
 *           CSFExt_TickDecrementAllowNegativeStacks
 *           -all any-VoidSelf-slot REPLACE, all touch instance+0x5C.
 *
 *       (3) Slot 80 OnElementInfluence element-reactive remove (4):
 *           CSFExt_RemoveOnFire, CSFExt_RemoveOnWater,
 *           CSFExt_RemoveOnCold, CSFExt_RemoveOnHeat -all slot 80,
 *           SelfI32P family, REPLACE chain, gated on RequestRemoval.
 *
 *       (4) Slot 81 GetImmunity publishers (8):
 *           CSFExt_PublishImmunity_{Burn, Wet, Cold, Bleed, Immobilize,
 *           Incapacitate, Petrify, Freeze} -slot 81, PtrRetOut family,
 *           REPLACE chain.
 *
 *       (5) Slot 82 stat-bonus donors (7):
 *           CSFExt_StatBonus_{Strength, Dexterity, Constitution,
 *           Intelligence, Speed, Charisma, Luck} -slot 82, VoidSelf
 *           family, REPLACE chain.  Each adds instance+0x5C to the
 *           bearer stat field at the matching CSF_STAT_* offset.
 *
 *     Uses the same init scaffolding as csf_ext_attuned_tuner: core
 *     resolve, retry timer, one-shot GON merge for a bundled config
 *     file if one ever ships, clean shutdown.
 *
 *   - Degrades gracefully if RegisterBehavior resolves NULL:
 *     BCE_RegisterBehaviors logs the table and exits without binding
 *     anything, and GON slot_overrides referencing these behaviors
 *     stay pending.  See the full flow and the current dispatcher
 *     coverage in the implementation notes at the bottom of this file.
 *
 * Depends only on csf_core.dll being loaded first (Mewjector handles
 * load ordering).  No link against csf_core -- every core call goes
 * through CSFCore_Resolve()'s runtime GetProcAddress path.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "csf_core_api.h"


/* ===================================================================
 *  Extension-local state
 *
 *  Cached Core API handle, game base, own DLL handle, and a set of
 *  one-shot flags that gate expensive init work so the retry callback
 *  is cheap to call repeatedly.
 * =================================================================== */

static CSFCoreAPI g_csf;
static int        g_csfReady         = 0;
static UINT_PTR   g_gameBase         = 0;
static HMODULE    g_hSelf            = NULL;

static int        g_initFirstLogDone = 0;   /* one-shot startup banner    */
static int        g_gonMergeRequested = 0;  /* one-shot GON push to core  */
static int        g_behaviorsBound   = 0;   /* set once all binds succeed */


/* ===================================================================
 *  Logging helper
 *
 *  Forwards to csf.Log once core is resolved, falls back to
 *  OutputDebugString during the pre-resolve window.  Every line is
 *  prefixed "[BCE]" so grepping the log stays consistent with the
 *  [AT] / [CSF] / [GON] conventions already in use.
 * =================================================================== */

static void BCELog(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    if (g_csfReady && g_csf.Log)
        g_csf.Log("[BCE] %s", buf);
    else
        OutputDebugStringA(buf);
}


/* ===================================================================
 *  Shippable behaviors
 *
 *  Plain C functions matching the game's slot signatures.  None of
 *  these chain to the donor's original slot -that is the caller's
 *  responsibility (or the core-side dispatcher's, once behavior-name
 *  resolution is wired up; see the design notes at the bottom of
 *  this file).  Each function is written to be as self-contained as
 *  possible so it can be bound onto any GON-declared status that
 *  wants the behavior, not just one specific donor.
 *
 *  Available behaviors.  BCE_RegisterBehaviors publishes these to
 *  csf_core's behavior-name registry at init time (see the
 *  implementation notes at the bottom of this file):
 *
 *    CSFExt_DeregisterFromDonorPool   slot 33 (OnInit)
 *        Removes the instance from the donor class's leaf pool so the
 *        donor's peer-iteration slots (e.g. SoulLink slot 11) stop
 *        seeing the clone.  Must run AFTER the donor's OnInit so the
 *        engine has finished its own pool bookkeeping before we touch
 *        it.
 *
 *    CSFExt_No_Op                     any slot with signature void(void*)
 *        Literal no-op.  Useful for GON statuses that want to silence
 *        a slot outright without having to ship a C stub.
 *
 *    CSFExt_No_Op_Destructor          slot 5 (destructor)
 *        Destructor-compatible no-op.  Matches the engine's destructor
 *        signature void*(void*,UINT64) and just returns self.  DOES
 *        NOT free the instance -callers that use this must be sure
 *        the engine isn't expecting the slot to take ownership of the
 *        allocation, which is true for scalar-deleting destructors
 *        called with flag bit 0 clear.
 *
 *    CSFExt_TickDecrementRemoveAtZero  any VoidSelf tick slot
 *        Countdown tick.  Decrements the 32-bit counter at +0x5C, fires
 *        slot 171 (OnStacksChangedPostTick) on non-zero, and asks the
 *        core to remove the instance when the counter hits zero.  The
 *        Burn/Rot pattern made generic and safe.  Requires
 *        CSFCore_RequestRemoval; skipped if the export is absent.
 *
 *    CSFExt_TickDecrement              ALIAS for TickDecrementRemoveAtZero
 *        Shorter name pointing at the same function.  Exists so a
 *        modder reaching for "the obvious decrement behavior" lands on
 *        the safe variant, not the raw one below.
 *
 *    CSFExt_TickIncrement              any VoidSelf tick slot
 *        Accumulator tick.  Increments the 32-bit counter at +0x5C and
 *        fires slot 171.  Bleed-shape use case: count up until another
 *        slot reads and reacts.
 *
 *    CSFExt_TickDecrementAllowNegativeStacks  any VoidSelf tick slot
 *        Raw signed decrement with no removal condition and no
 *        underflow guard.  The counter is permitted to go negative.
 *        Deliberately long name -most modders should use
 *        CSFExt_TickDecrement instead.  See the implementation
 *        comment for the intended-use contract.
 *
 *    CSFExt_RemoveOn{Fire,Water,Cold,Heat}  slot 80 OnElementInfluence
 *        Element-reactive remove.  When the bound element bit is set
 *        on the incoming mask, asks Core to RequestRemoval(self).
 *        Generalises Burn's "water extinguishes me" / IceArmor's
 *        "heat melts me" pathways.  All four are gated on
 *        g_csf.RequestRemoval.
 *
 *    CSFExt_PublishImmunity_{Burn, Wet, Cold, Bleed, Immobilize,
 *                            Incapacitate, Petrify, Freeze}
 *                                         slot 81 GetImmunity
 *        Writes the matching CSF_IMMUNITY_* bit(s) into the out
 *        buffer and returns it.  Incapacitate, Petrify, and Freeze
 *        publish the composite masks observed in the vanilla
 *        hard-CC donors (IMMOBILIZE+INCAP, +PETRIFY, +COLD
 *        respectively).  Wet additionally sets w1 bit 5 to mirror
 *        the donor exactly.
 *
 *    CSFExt_StatBonus_{Strength, Dexterity, Constitution,
 *                      Intelligence, Speed, Charisma, Luck}
 *                                         slot 82 OnStatModify
 *        Adds instance+0x5C to the bearer stat field at the matching
 *        CSF_STAT_* offset.  Modders set the amount via GON
 *        `params { stacks N }` -positive values buff, negative
 *        values debuff (Weakness-shape).  Does NOT ship a matching
 *        slot-45 OnRemove reverser -see the implementation comment
 *        for the lifecycle contract.
 * =================================================================== */


/* -------------------------------------------------------------------
 *  CSFExt_DeregisterFromDonorPool  (slot 33, OnInit)
 *
 *  Generic port of csf_ext_attuned_tuner's AT_DeregisterFromDonorPool.
 *  GON-cloned statuses register under the donor's type IDs because
 *  slot 3 (GetTypeInfo) is inherited, which means the donor's leaf
 *  class pool sees them as members.  Any donor slot that iterates
 *  that pool looking for peers then hits the clones as false positives
 *  (SoulLink's slot 11 is the canonical example but it's not the only
 *  one).  This helper swap-removes `instance` from its leaf pool entry
 *  and marks the pool cache dirty, so the donor keeps working against
 *  its own instances and the clone silently drops off the radar.
 *
 *  Data layout (stable across the game builds we've reversed):
 *    instance+0x18 -> classRef+0x08 -> classDesc
 *    classDesc+0x18 -> srcRegistry  (dynarray[type_idx], 0x10 each)
 *    classDesc+0x20 -> poolCache    (cache[type_idx],    0x10 each)
 *    srcEntry: { cap(4), count(4), data_ptr(8) }
 *    cacheEntry: { pool_ptr(8), dirty(1) }
 *    typeInfo from vtable[3]: indices=(int*), count=*(u64*)(+0x40)
 *    leaf = indices[count-1]
 *
 *  Takes only `void* instance` -fully generic across donors and
 *  across statuses built on top of them.
 * ------------------------------------------------------------------- */

static void CSFExt_DeregisterFromDonorPool(void* instance)
{
    void* classRef;
    void* classDesc;
    char* srcRegBase;
    char* poolCacheBase;
    void** vtable;
    void* typeInfo;
    int* indices;
    UINT64 typeCount;
    int leafIdx;
    char* srcEntry;
    int entryCount;
    void** dataArray;
    int i;

    typedef void* (*fn_get_type_info_t)(void*);

    if (!instance) return;

    classRef = *(void**)((char*)instance + 0x18);
    if (!classRef) return;
    classDesc = *(void**)((char*)classRef + 8);
    if (!classDesc) return;

    vtable = *(void***)instance;
    {
        fn_get_type_info_t getTypeInfo = (fn_get_type_info_t)vtable[3];
        typeInfo = getTypeInfo(instance);
    }
    if (!typeInfo) return;

    indices   = (int*)typeInfo;
    typeCount = *(UINT64*)((char*)typeInfo + 0x40);
    if (typeCount == 0) return;

    leafIdx = indices[typeCount - 1];

    srcRegBase    = *(char**)((char*)classDesc + 0x18);
    poolCacheBase = *(char**)((char*)classDesc + 0x20);
    if (!srcRegBase || !poolCacheBase) return;

    srcEntry   = srcRegBase + (long long)leafIdx * 0x10;
    entryCount = *(int*)(srcEntry + 4);
    dataArray  = *(void***)(srcEntry + 8);
    if (!dataArray || entryCount <= 0) return;

    for (i = 0; i < entryCount; i++)
    {
        if (dataArray[i] == instance)
        {
            entryCount--;
            if (i != entryCount)
                dataArray[i] = dataArray[entryCount];
            *(int*)(srcEntry + 4) = entryCount;
            *(poolCacheBase + (long long)leafIdx * 0x10 + 8) = 1;

            BCELog("removed 0x%p from donor pool 0x%X (count now %d)",
                   instance, leafIdx, entryCount);
            return;
        }
    }
    BCELog("0x%p not found in donor pool 0x%X", instance, leafIdx);
}


/* -------------------------------------------------------------------
 *  CSFExt_No_Op  (any slot with signature void(void*))
 *
 *  Literal no-op for the common per-instance slot signature.  Lets a
 *  modder silence an inherited slot behavior on their GON-cloned
 *  status without having to compile a C stub of their own.  Typical
 *  use cases: silencing an OnInit that does donor-specific bookkeeping
 *  we don't want, stubbing out a tick callback during iteration on
 *  balance, etc.
 *
 *  IMPORTANT: this does NOT chain to the donor's original slot.  If
 *  the donor's original behavior matters for engine correctness (most
 *  donor slot 5 / destructor work, for instance), use a targeted
 *  behavior instead of this generic no-op.
 * ------------------------------------------------------------------- */

static void CSFExt_No_Op(void* self)
{
    (void)self;
}


/* -------------------------------------------------------------------
 *  CSFExt_No_Op_Destructor  (slot 5, destructor)
 *
 *  Destructor-signature no-op.  The engine's destructor slot is
 *
 *      void* destructor(void* self, UINT64 flags)
 *
 *  where the low bit of `flags` asks the callee to free the backing
 *  allocation (scalar-deleting destructor convention).  This helper
 *  simply returns `self` without touching flags and without freeing.
 *  Use it only when you need slot 5 to be effectively inert and you
 *  know the engine won't then try to reuse or free the instance
 *  through this path.
 *
 *  For the overwhelming majority of cases the right move is NOT this;
 *  it's to chain to the donor's original slot 5.  We ship No_Op_Dtor
 *  because the "I want a genuinely inert status" case exists and
 *  otherwise has no plug-and-play answer.
 * ------------------------------------------------------------------- */

static void* CSFExt_No_Op_Destructor(void* self, UINT64 flags)
{
    (void)flags;
    return self;
}


/* ===================================================================
 *  Generic tick behaviors -- counter mutation on VoidSelf slots
 *
 *  The four behaviors below all share a common shape: they are
 *  signature void(self), they chain REPLACE (a modder's custom status
 *  has no donor tick body worth chaining through to -the donor
 *  inherits the base tick default and we are deliberately
 *  substituting), and they touch the 32-bit counter at instance
 *  offset +0x5C.
 *
 *  Target slots (modder picks via GON on their custom status):
 *    36  OnTurnBeginQueued  (Burn's canonical tick slot, VERIFIED)
 *    37  OnTurnEndQueued    (Rot's canonical tick slot,  VERIFIED)
 *    41  OnRoundEndQueued   (round-scope tick slot,      VERIFIED)
 *    40  (unnamed VoidSelf, suspected OnRoundBegin,      NONE)
 *  Any other VoidSelf slot the modder wants to repurpose will also
 *  route correctly -the 172-slot dispatcher in
 *  csf_core_gon_loader.c covers the full vtable.
 *
 *  Data contract: +0x5C is just 32-bit storage -the engine does
 *  not enforce a "duration" meaning on it.  What it represents is
 *  entirely determined by which behavior the modder wires up.
 *  Burn/Rot use it as a countdown; Bleed uses it as an accumulator;
 *  a custom status built on top of these behaviors can use it
 *  either way.
 *
 *  Slot 171 call: after the counter mutation, each behavior invokes
 *  slot 171 (OnStacksChangedPostTick) with delta=1, matching vanilla
 *  Burn/Rot exactly.  Slot 171's base stub is `ret 0` and it is
 *  inherited unchanged by the vast majority of donors (only Leeches /
 *  ManaLeeches / Ammo override it in shipping data), so by default
 *  the call is literally a no-op -no runtime cost beyond one
 *  indirect through the vtable.  The point is the override path:
 *  a modder who DOES override slot 171 on their custom status -to
 *  publish the new counter to the UI, fire a VFX, heal the bearer
 *  the way Leeches does, etc. -gets their override invoked
 *  automatically without having to write their own tick body.
 *  Binding one of these tick behaviors and overriding slot 171 on
 *  the same status is the canonical GON-only way to get
 *  vanilla-Burn-shape reactivity.
 *
 *  For CSFExt_TickDecrementRemoveAtZero specifically, the slot 171
 *  call and g_csf.RequestRemoval are mutually exclusive -slot 171
 *  fires only on ticks where the counter did not hit zero.  Vanilla
 *  Burn/Rot do the same: on the removal tick the post-tick hook is
 *  skipped because the instance is about to be destroyed.
 * =================================================================== */

#define BCE_STATUS_COUNTER_OFFSET   0x5C
#define BCE_SLOT_ON_STACKS_CHANGED  171

typedef void (*bce_on_stacks_changed_fn_t)(void*, int);

static void BCE_FireOnStacksChangedPostTick(void* self, int delta)
{
    void** vtable = *(void***)self;
    bce_on_stacks_changed_fn_t fn =
        (bce_on_stacks_changed_fn_t)vtable[BCE_SLOT_ON_STACKS_CHANGED];
    fn(self, delta);
}


/* -------------------------------------------------------------------
 *  CSFExt_TickDecrementRemoveAtZero  (any VoidSelf tick slot, REPLACE)
 *
 *  Countdown tick.  Every time the bound slot fires, decrements the
 *  counter and either fires slot 171 (OnStacksChangedPostTick, see
 *  the header block above) or asks the engine to remove the instance
 *  when the counter hits zero.  This is the Burn/Rot pattern made
 *  generic and safe for GON authors: the modder picks the lifecycle
 *  slot (turn start, turn end, round end, etc.), sets the starting
 *  counter via a GON param, and the behavior handles the rest.
 *
 *  Requires CSFCore_RequestRemoval.  BCE_RegisterBehaviors refuses to
 *  publish this behavior (and its CSFExt_TickDecrement alias) if
 *  g_csf.RequestRemoval is NULL.
 * ------------------------------------------------------------------- */

static void CSFExt_TickDecrementRemoveAtZero(void* self)
{
    uint32_t* counter;

    if (!self) return;

    counter = (uint32_t*)((char*)self + BCE_STATUS_COUNTER_OFFSET);
    *counter -= 1;

    if (*counter != 0)
    {
        BCE_FireOnStacksChangedPostTick(self, 1);
    }
    else
    {
        /* Gated at registration time; RequestRemoval was confirmed
         * non-NULL before this behavior was published.  Still
         * null-check defensively. */
        if (g_csf.RequestRemoval)
            g_csf.RequestRemoval(self);
    }
}


/* -------------------------------------------------------------------
 *  CSFExt_TickDecrement  (any VoidSelf tick slot, REPLACE) -- ALIAS
 *
 *  Published as a second entry in g_bceBehaviors pointing at
 *  CSFExt_TickDecrementRemoveAtZero so GON authors who write
 *  `behavior_name "CSFExt_TickDecrement"` get the safe default
 *  automatically.  The reasoning: a modder reading the table and
 *  thinking "I want something that decrements my counter on tick"
 *  will reach for the shortest plausible name first, and that name
 *  had better point at a behavior that won't underflow their counter
 *  into negative values and leak the instance forever.
 *
 *  The raw "just decrement, don't remove, and allow negative values"
 *  behavior exists under a deliberately unwieldy name (see below),
 *  so modders who actually want it have to opt in explicitly by
 *  typing the long name, and nobody stumbles into it by
 *  pattern-matching.
 *
 *  No function of its own -the alias is implemented at the
 *  registration-table level by pointing the second name at
 *  CSFExt_TickDecrementRemoveAtZero's function pointer.
 * ------------------------------------------------------------------- */


/* -------------------------------------------------------------------
 *  CSFExt_TickIncrement  (any VoidSelf tick slot, REPLACE)
 *
 *  Accumulator tick.  The Bleed-shape use case: count something
 *  upward on every lifecycle event until an unrelated slot (damage
 *  apply, external proc, etc.) reads the counter and reacts.  No
 *  cap, no removal condition; if a modder wants a cap they bind a
 *  different behavior (or a second behavior) that enforces it.
 * ------------------------------------------------------------------- */

static void CSFExt_TickIncrement(void* self)
{
    uint32_t* counter;

    if (!self) return;

    counter = (uint32_t*)((char*)self + BCE_STATUS_COUNTER_OFFSET);
    *counter += 1;

    BCE_FireOnStacksChangedPostTick(self, 1);
}


/* -------------------------------------------------------------------
 *  CSFExt_TickDecrementAllowNegativeStacks  (any VoidSelf tick slot,
 *                                            REPLACE)
 *
 *  Raw signed decrement with no removal condition and no underflow
 *  guard.  Lets the counter go negative indefinitely.
 *
 *  This is an unusual use case.  It exists for statuses whose
 *  counter is being used as a signed delta accumulator (e.g.  "net
 *  atk buffs applied this turn" where negative values are
 *  meaningful), not as a duration.  For the overwhelming majority of
 *  "I want to decrement on tick" cases, the right answer is
 *  CSFExt_TickDecrementRemoveAtZero (aliased as CSFExt_TickDecrement
 *  for convenience); the deliberately long name here exists so a
 *  modder has to go out of their way to pick this variant.
 *
 *  A modder who binds this behavior is asserting that SOME other
 *  slot on their status handles the removal path, and that the
 *  counter going below zero is intentional.  If neither of those is
 *  true, the instance will live forever.  Also, most statuses most
 *  likely do not have defined behaviors for negative stacks.
 *  Basically, use if you know EXACTLY what you are doing.
 * ------------------------------------------------------------------- */

static void CSFExt_TickDecrementAllowNegativeStacks(void* self)
{
    int32_t* counter;

    if (!self) return;

    counter = (int32_t*)((char*)self + BCE_STATUS_COUNTER_OFFSET);
    *counter -= 1;

    BCE_FireOnStacksChangedPostTick(self, 1);
}


/* ===================================================================
 *  Slot 80 -OnElementInfluence element-reactive behaviors
 *
 *  Signature: void slot80(void* self, const CSF_ElementMask* mask).
 *  Dispatcher family: SelfI32P (csf_core_gon_loader.c line 483).  Chain
 *  REPLACE in every case below -a modder who picks a "RemoveOn<X>"
 *  behavior is asserting that the custom status has NO donor slot-80
 *  body worth chaining through to, and even if it did, running donor
 *  logic after the remove request would be racing the engine's
 *  teardown.  If chaining is ever wanted, it has to come from a
 *  different, donor-aware behavior.
 *
 *  All four behaviors check a single bit on the mask and, on hit, ask
 *  core to remove the instance via g_csf.RequestRemoval.  That export
 *  requires RequestRemoval, so every entry is flagged requiresRemoval
 *  in g_bceBehaviors and BCE_RegisterBehaviors refuses to publish them
 *  if the export is absent (same gate as the decrement-remove tick).
 *
 *  Canonical use case: a "candle" status that expires when the tile it
 *  sits on gets wet, or a "frost" status that melts when heat arrives.
 *  Burn's own slot-80 reacts to water this way on the game side;
 *  RemoveOnWater is that behavior generalised and GON-bindable.
 *
 *  The mask pointer is permitted to be NULL in theory (the engine
 *  always supplies a valid pointer on the observed call sites, but
 *  the signature allows it).  Every helper null-checks before deref
 *  so a malformed caller doesn't crash the DLL.
 * =================================================================== */

static void CSFExt_RemoveOnFire(void* self, INT32* mask_raw)
{
    const CSF_ElementMask* mask = (const CSF_ElementMask*)mask_raw;

    if (!self || !mask) return;
    if (mask->w0 & CSF_ELEMENT_FIRE) {
        if (g_csf.RequestRemoval)
            g_csf.RequestRemoval(self);
    }
}

static void CSFExt_RemoveOnWater(void* self, INT32* mask_raw)
{
    const CSF_ElementMask* mask = (const CSF_ElementMask*)mask_raw;

    if (!self || !mask) return;
    if (mask->w0 & CSF_ELEMENT_WATER) {
        if (g_csf.RequestRemoval)
            g_csf.RequestRemoval(self);
    }
}

static void CSFExt_RemoveOnCold(void* self, INT32* mask_raw)
{
    const CSF_ElementMask* mask = (const CSF_ElementMask*)mask_raw;

    if (!self || !mask) return;
    if (mask->w0 & CSF_ELEMENT_COLD) {
        if (g_csf.RequestRemoval)
            g_csf.RequestRemoval(self);
    }
}

/* The melt pathway used by IceArmor / Freeze in the wild: both
 * donors loop over {w0 & FIRE, w1 & HEAT} as the trigger set, so
 * "melts on heat" semantically means w1 bit 1, not w0 bit 2. */
static void CSFExt_RemoveOnHeat(void* self, INT32* mask_raw)
{
    const CSF_ElementMask* mask = (const CSF_ElementMask*)mask_raw;

    if (!self || !mask) return;
    if (mask->w1 & CSF_ELEMENT_HEAT) {
        if (g_csf.RequestRemoval)
            g_csf.RequestRemoval(self);
    }
}


/* ===================================================================
 *  Slot 81 -GetImmunity immunity-publisher behaviors
 *
 *  Signature: void* slot81(void* self, void* out_mask).
 *  Dispatcher family: PtrRetOut (csf_core_gon_loader.c line 513).  Chain
 *  REPLACE -the donor's original GetImmunity writes zero (base stub),
 *  and the dispatcher's REPLACE path hands the behavior an already-
 *  zeroed out buffer, so the publishers below only need to OR-in their
 *  own bits and return.
 *
 *  AFTER semantics on PtrRetOut run the donor first, then the
 *  behavior, and return the behavior's written out buffer -so AFTER
 *  is effectively a pre-seeded REPLACE with the donor's bits as a
 *  starting point.  We pick REPLACE because the overwhelmingly common
 *  case is "my custom status grants immunity to X" against a donor
 *  whose GetImmunity is the zero-writing base stub.  If a modder
 *  wants "donor bits plus mine", they can author a second behavior
 *  in AFTER mode.
 *
 *  The engine always allocates the out buffer before calling slot 81;
 *  the dispatcher zeroes it on REPLACE before handing it to the
 *  behavior.  We still defensively check for NULL so a buggy caller
 *  doesn't crash us.
 *
 *  "Composite" behaviors (Petrify, Freeze, Incapacitate) publish the
 *  same multi-bit masks observed in donors that grant the full
 *  hard-CC category: petrified characters also can't move or take
 *  actions, frozen characters can't be re-chilled or re-petrified,
 *  etc.  See the CSF_IMMUNITY_*_MASK defines in csf_core_api.h for
 *  the exact bit composition.
 * =================================================================== */

static void* CSFExt_PublishImmunity_Burn(void* self, void* out_raw)
{
    CSF_ImmunityMask* out = (CSF_ImmunityMask*)out_raw;

    (void)self;
    if (!out) return out_raw;
    out->w0 |= CSF_IMMUNITY_BURN;
    return out_raw;
}

/* Wet publishes two bits: the low-word WET bit (used by the apply
 * path to reject re-wet) and the high-word w1 bit 5 (semantics TBD,
 * documented in csf_core_api.h).  We mirror the donor exactly. */
static void* CSFExt_PublishImmunity_Wet(void* self, void* out_raw)
{
    CSF_ImmunityMask* out = (CSF_ImmunityMask*)out_raw;

    (void)self;
    if (!out) return out_raw;
    out->w0 |= CSF_IMMUNITY_WET;
    out->w1 |= CSF_IMMUNITY_W1_BIT_5;
    return out_raw;
}

static void* CSFExt_PublishImmunity_Cold(void* self, void* out_raw)
{
    CSF_ImmunityMask* out = (CSF_ImmunityMask*)out_raw;

    (void)self;
    if (!out) return out_raw;
    out->w0 |= CSF_IMMUNITY_COLD;
    return out_raw;
}

static void* CSFExt_PublishImmunity_Bleed(void* self, void* out_raw)
{
    CSF_ImmunityMask* out = (CSF_ImmunityMask*)out_raw;

    (void)self;
    if (!out) return out_raw;
    out->w0 |= CSF_IMMUNITY_BLEED;
    return out_raw;
}

static void* CSFExt_PublishImmunity_Immobilize(void* self, void* out_raw)
{
    CSF_ImmunityMask* out = (CSF_ImmunityMask*)out_raw;

    (void)self;
    if (!out) return out_raw;
    out->w0 |= CSF_IMMUNITY_IMMOBILIZE;
    return out_raw;
}

static void* CSFExt_PublishImmunity_Incapacitate(void* self, void* out_raw)
{
    CSF_ImmunityMask* out = (CSF_ImmunityMask*)out_raw;

    (void)self;
    if (!out) return out_raw;
    /* Incapacitate = IMMOBILIZE | INCAPACITATE (0x300000).  Every
     * donor that grants incap grants immobilize too, matching the
     * 0x300000 composite observed in the wild. */
    out->w0 |= CSF_IMMUNITY_INCAP_GROUP;
    return out_raw;
}

static void* CSFExt_PublishImmunity_Petrify(void* self, void* out_raw)
{
    CSF_ImmunityMask* out = (CSF_ImmunityMask*)out_raw;

    (void)self;
    if (!out) return out_raw;
    /* Petrify = IMMOBILIZE | INCAPACITATE | PETRIFY (0x300200). */
    out->w0 |= CSF_IMMUNITY_PETRIFY_MASK;
    return out_raw;
}

static void* CSFExt_PublishImmunity_Freeze(void* self, void* out_raw)
{
    CSF_ImmunityMask* out = (CSF_ImmunityMask*)out_raw;

    (void)self;
    if (!out) return out_raw;
    /* Freeze = IMMOBILIZE | INCAPACITATE | COLD (0x300020). */
    out->w0 |= CSF_IMMUNITY_FREEZE_MASK;
    return out_raw;
}


/* ===================================================================
 *  Slot 82 -stat-bonus behaviors
 *
 *  Signature: void slot82(void* self).
 *  Dispatcher family: VoidSelf.  Chain REPLACE -donor's original
 *  slot-82 is the base no-op stub on every custom status (and on the
 *  vast majority of donors), so there's nothing to chain through.
 *
 *  Donor pattern (TempStrengthUp @ 0x140803570):
 *      *(int*)(bearer + 0x5bc) += *(int*)(instance + 0x5c);
 *
 *  where `bearer` is *(void**)(instance + 0x38) and the instance
 *  counter at +0x5c is the signed int32 the modder sets via their
 *  GON `params { stacks N }` line (core copies stacks into +0x5c at
 *  apply time).  Negative counters donate a negative amount, which
 *  is exactly how Weakness models "Strength -2, Dex -2".
 *
 *  These are ADDITIVE donors, not assigners.  The bearer stat field
 *  holds the cumulative bonus from all active bonus donors; the
 *  engine's stat calculation reads this sum and combines it with
 *  base class stats.  An extension that wants a "set to X"
 *  behavior needs a different implementation -slot 82 as used in
 *  the wild is purely +=.
 *
 *  IMPORTANT about lifecycle: a donor that ONLY sets its field via
 *  slot 82 doesn't also un-set it on removal.  The vanilla *Up
 *  statuses pair their slot-82 with a matching slot-45 (OnRemove)
 *  that subtracts the same amount back off the bearer stat.  We do
 *  NOT ship a matching OnRemove here -a modder using one of these
 *  StatBonus behaviors is expected to either:
 *
 *    (a) make the custom status permanent for the bearer's life
 *        (so removal never runs and the subtraction is moot), OR
 *    (b) author a companion slot-45 behavior that reverses the
 *        addition, OR
 *    (c) accept the leak.
 *
 *  Option (b) is the clean one and will likely ship as a follow-up
 *  behavior pack once we have slot-45 dispatcher coverage validated.
 *  Until then, these are power-user tools with a known caveat.
 *
 *  Slot-82 also has a well-known "self-removal on counter hit 0"
 *  epilogue in donors that use +0x5c as a countdown (the +0x75 latch
 *  + bearer+0xe50 flag + vtable[45] call pattern).  We deliberately
 *  DO NOT implement that epilogue here -if a modder wants
 *  countdown-flavored stat bonuses, the right shape is "bind
 *  StatBonus_X on slot 82 for the donation, bind TickDecrement on
 *  a tick slot for the countdown".  Keeping the two concerns
 *  separate lets modders mix and match tick cadences.
 * =================================================================== */

static void BCE_ApplyStatBonus(void* self, int statOffset)
{
    void* bearer;
    int32_t delta;

    if (!self) return;
    bearer = CSF_BEARER(self);
    if (!bearer) return;
    delta = *(int32_t*)((char*)self + CSF_INSTANCE_COUNTER);
    *(int32_t*)((char*)bearer + statOffset) += delta;
}

static void CSFExt_StatBonus_Strength(void* self)
{
    BCE_ApplyStatBonus(self, CSF_STAT_STRENGTH);
}

static void CSFExt_StatBonus_Dexterity(void* self)
{
    BCE_ApplyStatBonus(self, CSF_STAT_DEXTERITY);
}

static void CSFExt_StatBonus_Constitution(void* self)
{
    BCE_ApplyStatBonus(self, CSF_STAT_CONSTITUTION);
}

static void CSFExt_StatBonus_Intelligence(void* self)
{
    BCE_ApplyStatBonus(self, CSF_STAT_INTELLIGENCE);
}

static void CSFExt_StatBonus_Speed(void* self)
{
    BCE_ApplyStatBonus(self, CSF_STAT_SPEED);
}

static void CSFExt_StatBonus_Charisma(void* self)
{
    BCE_ApplyStatBonus(self, CSF_STAT_CHARISMA);
}

static void CSFExt_StatBonus_Luck(void* self)
{
    BCE_ApplyStatBonus(self, CSF_STAT_LUCK);
}


/* ===================================================================
 *  Behavior registration
 *
 *  Publishes the shippable behaviors above to the core by name.  Each
 *  entry in the table is a (name, slot-index, function-ptr) triple;
 *  the core stores the mapping and the GON loader consults it when a
 *  slot_overrides value doesn't match any known donor, treating the
 *  token as a behavior name instead.  See the "Behavior-name registry"
 *  design block at the bottom of this file for the full flow.
 *
 *  The slotIndex is advisory -it's the slot the behavior is designed
 *  to be wired into, which the core can use to reject obviously
 *  mismatched overrides (e.g. binding a destructor no-op onto slot
 *  33).  Pass -1 to mean "any compatible slot".
 *
 *  Every entry is published through g_csf.RegisterBehavior below.  If
 *  the pointer resolves NULL the helper logs the table only, then flags
 *  itself done so the init-retry loop stops polling.  GON slot_overrides
 *  referencing these behaviors stay pending until a core with the export
 *  is loaded.
 * =================================================================== */

/* Chain mode for a behavior -- see the "Behavior-name registry" design
 * block at the bottom of this file.  Hardcoded per-behavior, not exposed
 * to GON.  A modder sophisticated enough to think about chaining is
 * sophisticated enough to edit this table and rebuild.
 *
 * Uses CSF_CHAIN_* from csf_core_api.h directly so the values can
 * never drift from what core expects. */

typedef struct BCE_BehaviorEntry
{
    const char* name;
    int         slotIndex;    /* suggested slot, -1 = any compatible */
    int         chainMode;    /* CSF_CHAIN_AFTER | CSF_CHAIN_REPLACE */
    void*       fn;
    int         requiresRemoval;   /* 1 if behavior needs RequestRemoval */
} BCE_BehaviorEntry;

static const BCE_BehaviorEntry g_bceBehaviors[] =
{
    /* Postprocesses donor's OnInit pool registration, so the donor
     * MUST run first. */
    { "CSFExt_DeregisterFromDonorPool", 33, CSF_CHAIN_AFTER,
      (void*)CSFExt_DeregisterFromDonorPool, 0 },

    /* A no-op that chains would not be a no-op, so these are
     * intrinsically REPLACE. */
    { "CSFExt_No_Op",                   -1, CSF_CHAIN_REPLACE,
      (void*)CSFExt_No_Op,                    0 },
    { "CSFExt_No_Op_Destructor",         5, CSF_CHAIN_REPLACE,
      (void*)CSFExt_No_Op_Destructor,         0 },

    /* Generic tick behaviors.  All signature void(self), chain
     * REPLACE (modder's custom status has no donor tick body worth
     * chaining), and all operate on the 32-bit counter at +0x5C.
     *
     * The first two share a function pointer -- CSFExt_TickDecrement
     * is the short-name alias of CSFExt_TickDecrementRemoveAtZero so
     * the obvious-looking name lands on the safe variant.  Both are
     * Gated on RequestRemoval; BCE_RegisterBehaviors refuses to
     * publish them if the export is absent. */
    { "CSFExt_TickDecrementRemoveAtZero",       -1, CSF_CHAIN_REPLACE,
      (void*)CSFExt_TickDecrementRemoveAtZero,  1 },
    { "CSFExt_TickDecrement",                   -1, CSF_CHAIN_REPLACE,
      (void*)CSFExt_TickDecrementRemoveAtZero,  1 },
    { "CSFExt_TickIncrement",                   -1, CSF_CHAIN_REPLACE,
      (void*)CSFExt_TickIncrement,              0 },
    { "CSFExt_TickDecrementAllowNegativeStacks", -1, CSF_CHAIN_REPLACE,
      (void*)CSFExt_TickDecrementAllowNegativeStacks, 0 },

    /* Slot 80 -- OnElementInfluence element-reactive remove helpers.
     * SelfI32P signature, REPLACE chain, gated on RequestRemoval. */
    { "CSFExt_RemoveOnFire",            80, CSF_CHAIN_REPLACE,
      (void*)CSFExt_RemoveOnFire,       1 },
    { "CSFExt_RemoveOnWater",           80, CSF_CHAIN_REPLACE,
      (void*)CSFExt_RemoveOnWater,      1 },
    { "CSFExt_RemoveOnCold",            80, CSF_CHAIN_REPLACE,
      (void*)CSFExt_RemoveOnCold,       1 },
    { "CSFExt_RemoveOnHeat",            80, CSF_CHAIN_REPLACE,
      (void*)CSFExt_RemoveOnHeat,       1 },

    /* Slot 81 -- GetImmunity publishers.  PtrRetOut signature, REPLACE
     * chain (dispatcher hands us a pre-zeroed out buffer).  None
     * No removal dependency -- they only touch the out parameter. */
    { "CSFExt_PublishImmunity_Burn",         81, CSF_CHAIN_REPLACE,
      (void*)CSFExt_PublishImmunity_Burn,    0 },
    { "CSFExt_PublishImmunity_Wet",          81, CSF_CHAIN_REPLACE,
      (void*)CSFExt_PublishImmunity_Wet,     0 },
    { "CSFExt_PublishImmunity_Cold",         81, CSF_CHAIN_REPLACE,
      (void*)CSFExt_PublishImmunity_Cold,    0 },
    { "CSFExt_PublishImmunity_Bleed",        81, CSF_CHAIN_REPLACE,
      (void*)CSFExt_PublishImmunity_Bleed,   0 },
    { "CSFExt_PublishImmunity_Immobilize",   81, CSF_CHAIN_REPLACE,
      (void*)CSFExt_PublishImmunity_Immobilize, 0 },
    { "CSFExt_PublishImmunity_Incapacitate", 81, CSF_CHAIN_REPLACE,
      (void*)CSFExt_PublishImmunity_Incapacitate, 0 },
    { "CSFExt_PublishImmunity_Petrify",      81, CSF_CHAIN_REPLACE,
      (void*)CSFExt_PublishImmunity_Petrify, 0 },
    { "CSFExt_PublishImmunity_Freeze",       81, CSF_CHAIN_REPLACE,
      (void*)CSFExt_PublishImmunity_Freeze,  0 },

    /* Slot 82 -- stat-bonus donors.  VoidSelf signature, REPLACE chain.
     * Each behavior adds instance+0x5c to one bearer stat offset;
     * modders pair with a GON `params { stacks N }` to set the amount.
     * No removal dependency -- only needs Log. */
    { "CSFExt_StatBonus_Strength",      82, CSF_CHAIN_REPLACE,
      (void*)CSFExt_StatBonus_Strength, 0 },
    { "CSFExt_StatBonus_Dexterity",     82, CSF_CHAIN_REPLACE,
      (void*)CSFExt_StatBonus_Dexterity, 0 },
    { "CSFExt_StatBonus_Constitution",  82, CSF_CHAIN_REPLACE,
      (void*)CSFExt_StatBonus_Constitution, 0 },
    { "CSFExt_StatBonus_Intelligence",  82, CSF_CHAIN_REPLACE,
      (void*)CSFExt_StatBonus_Intelligence, 0 },
    { "CSFExt_StatBonus_Speed",         82, CSF_CHAIN_REPLACE,
      (void*)CSFExt_StatBonus_Speed,    0 },
    { "CSFExt_StatBonus_Charisma",      82, CSF_CHAIN_REPLACE,
      (void*)CSFExt_StatBonus_Charisma, 0 },
    { "CSFExt_StatBonus_Luck",          82, CSF_CHAIN_REPLACE,
      (void*)CSFExt_StatBonus_Luck,     0 },
};

#define BCE_BEHAVIOR_COUNT (sizeof(g_bceBehaviors) / sizeof(g_bceBehaviors[0]))

static int BCE_RegisterBehaviors(void)
{
    size_t i;
    int haveRemoval;

    if (g_behaviorsBound) return 1;

    haveRemoval = (g_csf.RequestRemoval != NULL);

    g_csf.Log("[BCE] publishing %zu behavior(s) (core v%d%s):",
              (size_t)BCE_BEHAVIOR_COUNT,
              g_csf.GetVersion(),
              haveRemoval ? ", RequestRemoval available"
                     : ", RequestRemoval NOT available");

    for (i = 0; i < BCE_BEHAVIOR_COUNT; i++) {
        const BCE_BehaviorEntry* e = &g_bceBehaviors[i];
        const char* slotDesc  = (e->slotIndex >= 0) ? NULL : "any slot";
        const char* chainDesc = (e->chainMode == CSF_CHAIN_AFTER)   ? "after"
                              : (e->chainMode == CSF_CHAIN_REPLACE) ? "replace"
                              : "?";

        if (e->requiresRemoval && !haveRemoval) {
            g_csf.Log("[BCE]   %s  -- SKIPPED (requires "
                      "CSFCore_RequestRemoval, which is not available "
                      "on this core)", e->name);
            continue;
        }

        if (slotDesc) {
            g_csf.Log("[BCE]   %s  (any slot, chain=%s)",
                      e->name, chainDesc);
        } else {
            g_csf.Log("[BCE]   %s  (slot %d, chain=%s)",
                      e->name, e->slotIndex, chainDesc);
        }

        if (g_csf.RegisterBehavior) {
            g_csf.RegisterBehavior(e->name, e->slotIndex,
                                   e->chainMode, e->fn);
        }
    }

    if (!g_csf.RegisterBehavior) {
        g_csf.Log("[BCE] CSFCore_RegisterBehavior not available -- "
                  "behaviors were only logged, not bound; GON "
                  "slot_overrides referencing them will stay pending");
    }

    g_behaviorsBound = 1;
    return 1;
}


/* ===================================================================
 *  Init entry point
 *
 *  Driven by CSFCore_InitOrWait from csf_core_api.h.  Runs once
 *  synchronously from DllMain; if it returns 0 the helper arms a
 *  thread-pool timer that re-polls every CSF__RETRY_INTERVAL_MS until
 *  the callback returns 1 or CSF__RETRY_MAX_ATTEMPTS passes.
 *
 *  This callback is idempotent -- it caches the resolved API on first
 *  entry, fires a one-shot GON merge (if we're shipping a bundled
 *  config), and then delegates to BCE_RegisterBehaviors() which tracks
 *  its own per-behavior bind state.
 * =================================================================== */

static int BCE_Init_Cb(CSFCoreAPI* api)
{
    if (!api) return 0;

    if (!g_csfReady) {
        g_csf      = *api;
        g_csfReady = 1;
        g_gameBase = g_csf.GetGameBase();
    }

    if (!g_initFirstLogDone) {
        g_csf.Log("[BCE] csf_core v%d resolved, gameBase=0x%016llX",
                  g_csf.GetVersion(), (unsigned long long)g_gameBase);
        g_initFirstLogDone = 1;
    }

    /* Optional: push a bundled custom_statuses.gon into core so behaviors
     * here can come with "reference statuses" that exercise them out of
     * the box.  Skip entirely if this DLL doesn't ship a GON file. */
    if (!g_gonMergeRequested && g_hSelf) {
        if (CSFCore_MergeGonFromMyDir(g_hSelf)) {
            g_csf.Log("[BCE] GON merge from ext directory requested");
        } else {
            g_csf.Log("[BCE] no bundled GON found "
                      "(or core export unavailable) -- continuing");
        }
        g_gonMergeRequested = 1;
    }

    return BCE_RegisterBehaviors();
}


/* ===================================================================
 *  DllMain
 *
 *  Same pattern as csf_ext_attuned_tuner: InitOrWait on attach,
 *  Shutdown on detach.  The Shutdown call tears down the retry timer
 *  so no callback fires against unloaded code pages.
 * =================================================================== */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hSelf = hModule;
        CSFCore_InitOrWait(BCE_Init_Cb);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        CSFCore_Shutdown();
    }
    return TRUE;
}


/* ===================================================================
 *  Behavior-name registry -- IMPLEMENTATION NOTES
 *
 *  How a GON-authored custom_statuses.gon entry binds one of this
 *  file's C behaviors onto a slot without the modder shipping any C
 *  code at all.  Every claim below is backed by code in csf_core.c,
 *  csf_core_gon_loader.c, and csf_core.h on the core side.
 *
 *  End-to-end flow:
 *
 *  1. Core behavior registry.  CSF_RegisterBehavior(name, slotIndex,
 *     chainMode, fn) in csf_core_gon_loader.c stores the (name, fn)
 *     mapping in a lock-guarded table (g_csfLock).  It is exported
 *     via the __declspec wrapper CSFCore_RegisterBehavior in
 *     csf_core.c and surfaced to extensions through the CSFCoreAPI
 *     struct as an optional resolve.  slotIndex is the expected slot
 *     or -1 for "any"; chainMode is CSF_CHAIN_AFTER or
 *     CSF_CHAIN_REPLACE.
 *
 *  2. Pending-override sweep.  Every successful CSF_RegisterBehavior
 *     call tail-triggers CSF_ResolvePendingBehaviorBindings(name),
 *     which walks every loaded CSF_StatusDef and rebinds any
 *     slot_override whose behavior_name matches.  This makes load
 *     order irrelevant: the GON file can land before or after BCE's
 *     DllMain and the outcome is identical.
 *
 *  3. GON loader side.  csf_core_gon_loader.c's LoadOneStatus stashes
 *     the third token of a slot_overrides entry into
 *     CSF_SlotOverride::behavior_name (declared in csf_core.h).  At
 *     status-def build time, or later during a sweep,
 *     CSF_ResolveBehaviorOverride looks up the behavior, picks a
 *     per-(slot, chain) dispatcher via CSF_PickDispatcher, caches the
 *     donor's original slot fn into behavior_donor_fn, and installs
 *     the dispatcher into def->custom_vtable[slot].
 *
 *  4. BCE init (this file).  BCE_RegisterBehaviors loops g_bceBehaviors
 *     and calls g_csf.RegisterBehavior on every entry.  A null
 *     RegisterBehavior pointer means the core lacks the export, in
 *     which case the helper logs the table and exits cleanly without
 *     binding anything -the behavior-name path is unavailable and
 *     the only way to reach these behaviors is a hand-written
 *     RegisterExtension bind from another extension.
 *
 *  Dispatcher coverage -- FULL (172 slots):
 *
 *  csf_core_gon_loader.c ships a CSF_GetSlotDispatchInfo 172-slot
 *  switch that routes every vtable slot through a DEFINE_*_DISPATCHERS
 *  pair for the slot's signature family.  Nine signature families are
 *  supported (VoidSelf, Dtor, SelfI64, SelfI64I64, SelfI64I64I8,
 *  SelfI32P, PtrRetOut, I32PRetOut, I64SelfpI64), and each DEFINE
 *  macro stamps out an AFTER and a REPLACE dispatcher per slot.  A
 *  handful of slots whose signatures we have not nailed down (1, 2,
 *  3, 4, 28, 30, 32, 52, 57, 61, 64, 86, 92, 93, 133, 134, 136, 142,
 *  145, 167, 168, 169) are routed UNIQUE and bind-time will refuse to
 *  install a behavior on them until the right dispatcher is added.
 *  See CSF_GetSlotDispatchInfo in csf_core_gon_loader.c for the
 *  full list and confidence grades.
 *
 *  Every other slot binds cleanly.  Each bind logs the slot's
 *  confidence grade (PROVEN / VERIFIED / HIGH / MEDIUM / LOW / NONE /
 *  STUB), so when a behavior shows up on a LOW-confidence slot and
 *  misbehaves in game, the log tells the modder exactly which slot to
 *  flag.
 *
 *  Dispatcher semantics -- note the slot-5 inversion:
 *
 *  For slot 33 (and the conventional case in general),
 *  CSF_CHAIN_AFTER means "donor first, then behavior"; CSF_CHAIN_REPLACE
 *  means "behavior only".  Slot 5 inverts the order for AFTER: the
 *  behavior runs first and the donor destructor runs second, because
 *  the donor destructor is the thing that actually tears the instance
 *  down -anything we want to observe has to happen before it.  Read
 *  AFTER as "donor is authoritative, behavior wraps around it"
 *  rather than strict temporal ordering.
 *
 *  The PtrRetOut / I32PRetOut / I64SelfpI64 families also use a
 *  donor-first-write, behavior-second-write-and-return pattern for
 *  AFTER so the behavior's return value is authoritative -the
 *  donor's out-buffer write is scratched over on purpose.  These are
 *  the return-value-bearing families and "chain through" does not
 *  have a clean meaning for them, so AFTER is effectively a
 *  pre-seeded REPLACE.
 *
 *  Implication for the behaviors in this file:
 *
 *  All 26 current behaviors are fully pluggable via GON alone:
 *  declare the behavior name as the third token of a slot_overrides
 *  entry and the registry + dispatcher will route it correctly,
 *  subject to the signature family actually matching the target
 *  slot.  The VoidSelf tick behaviors bind against any VoidSelf
 *  slot -slots 36, 37, 40, 41 are the interesting tick slots, but
 *  nothing in BCE enforces "these slots only".  If a modder wires
 *  them onto a non-tick VoidSelf slot deliberately, the behavior
 *  will still fire on that slot's cadence and do the same work.
 *
 *  The slot-80/81/82 behaviors are pinned to their specific slots
 *  via the slotIndex field in g_bceBehaviors -they use signature
 *  families (SelfI32P, PtrRetOut, VoidSelf) that exist on several
 *  other slots, but the hardcoded slotIndex tells core to reject
 *  bindings onto mismatched slots at vtable-build time.  A modder
 *  who wants to use, say, CSFExt_StatBonus_Strength on a slot
 *  other than 82 (there's no obvious reason to) would need to
 *  duplicate the entry in this table with a different slotIndex,
 *  not override it from GON.
 * =================================================================== */
