/*
 * csf_ext_attuned_tuner.c -- Attuned/Tuner damage-forwarding extension
 *
 * A self-contained csf_core extension DLL.  Depends only on csf_core.dll
 * being loaded first (Mewjector handles load ordering via [LoadOrder]
 * priorities).  No link against csf_core -- all Core calls are resolved
 * at runtime via CSFCore_Resolve().
 *
 * Paired statuses (GON-declared in custom_statuses.gon, donor SoulLink):
 *   Tuner   (publisher)  -- on damage taken, forward to all Attuned carriers
 *   Attuned (subscriber) -- passively receives forwarded damage
 *
 * Asymmetric: Attuned damage does NOT feed back to Tuners.
 * Stackless:  presence-only, no stack multiplier.
 * Many-to-many: any Tuner forwards to every live Attuned.
 *
 * Slot bindings installed via csf.RegisterExtension at DllMain time:
 *   Slot  5 (Destructor)             -> AT_destructor_wrapper (shared)
 *   Slot 11 (LateUpdate)             -> Tuner_dispatcher / Attuned_dispatcher
 *   Slot 33 (OnInit)                 -> AT_OnInit_Tuner / AT_OnInit_Attuned
 *   Slot 67 (OnReceivedDamageReport) -> Tuner_collector  / Attuned_collector
 *
 * Slot 169 (DamageInstance template) inherited from SoulLink unchanged.
 *
 * This DLL owns its own per-instance sidecar (g_atRegistry) separate from
 * Core's sidecar table.  Core's sidecar is keyed on the same game instance
 * pointers but stores different data (apply_mode bookkeeping, GON-declared
 * int/double params).  The two coexist without collision.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "csf_core_api.h"


/* ===================================================================
 *  Game RVAs
 * =================================================================== */

/* apply_damage(Entity*, DamageInstance*) -- full damage pipeline.
 * Confirmed by objdump: vanilla SoulLink slot 11 calls this at
 * 0x1402708EC (e8 7f da e9 ff). Proper x64 prologue at entry. */
#define RVA_APPLY_DAMAGE        0x10E370

/* DamageInstance buffer size (game uses ~0x150, we reserve 0x200) */
#define DAMAGE_INSTANCE_SIZE    0x200

/* Echo-bit and source-flag offsets inside DamageInstance.
 * Derived from current binary assembly (objdump-confirmed):
 *   buf lives at [rsp+0x50] in vanilla slot 11
 *   Echo: or BYTE PTR [rsp+0xAA], 0x08  ->  buf+0x5A
 *   Flag: mov BYTE PTR [rsp+0xAD], cl   ->  buf+0x5D */
#define DI_ECHO_BIT_OFFSET      0x5a
#define DI_ECHO_BIT_MASK        0x08
#define DI_SRCFLAG_OFFSET       0x5d

/* Passive instance field offsets (matched to vanilla SoulLink) */
#define PASSIVE_DYING_FLAG      0x75   /* !=0: instance dying            */
#define PASSIVE_HAS_PENDING     0x74   /* set by collector               */
#define PASSIVE_DISPATCHED_FLAG 0xe8   /* cleared by collector           */
#define PASSIVE_ENTITY_OFFSET   0x38   /* instance -> entity             */
#define PASSIVE_CHAR_OFFSET     0x98   /* instance -> character          */


/* ===================================================================
 *  Function pointer typedefs
 * =================================================================== */

typedef void (*fn_apply_damage)(void* entity, void* damage_instance);
typedef void (*fn_slot169_template)(void* self, void* out_instance,
                                    unsigned int amount, void* source,
                                    char no_apply);
typedef void* (*fn_destructor)(void* self, UINT64 flags);
typedef void  (*fn_on_init)(void* self);


/* ===================================================================
 *  Extension-local state
 *
 *  This DLL owns its own per-instance registry (g_atRegistry) plus
 *  cached Core API + game base + donor chain fn pointers.  None of
 *  this is shared with csf_core.dll -- Core keeps its own sidecar
 *  table keyed by the same instance pointers but storing different
 *  fields (apply_mode, GON params).  The two tables coexist.
 * =================================================================== */

#define ATKIND_TUNER    1
#define ATKIND_ATTUNED  2

#define AT_MAX_INSTANCES    64
#define AT_MAX_EVENTS       32

typedef struct {
    unsigned int amount;
    void*        source;        /* vanilla source: *(*(*(evdata+8))+0x98) */
    unsigned char src_flag_bit; /* relayed into DI_SRCFLAG_OFFSET bit 5   */
} AT_Event;

typedef struct {
    void* instance;             /* Passive* (pool instance)               */
    void* entity;               /* cached entity (instance+0x38)          */
    void* character;            /* cached character (instance+0x98)       */
    int   kind;                 /* ATKIND_TUNER or ATKIND_ATTUNED         */
    int   event_count;          /* queued events (Tuner only)             */
    AT_Event events[AT_MAX_EVENTS];
} AT_InstanceData;

static AT_InstanceData g_atRegistry[AT_MAX_INSTANCES];
static int g_atCount           = 0;
static int g_tunerDispatching  = 0;   /* re-entry guard */

/* Resolved Core API (populated on first successful Init_Cb invocation) */
static CSFCoreAPI g_csf;
static int        g_csfReady  = 0;
static UINT_PTR   g_gameBase  = 0;

/* Captured SoulLink originals -- chained so donor bookkeeping still runs */
static fn_destructor g_origSoulLinkDtor   = 0;
static fn_on_init    g_origSoulLinkOnInit = 0;

/* Per-status bind tracking.  Each flag goes 0 -> 1 exactly once, when
 * all four slots (5/11/33/67) have been successfully installed for that
 * status.  The Init_Cb skips work for already-bound statuses, so it's
 * cheap to call repeatedly from the CSFCore_InitOrWait retry loop. */
static int     g_tunerBound        = 0;
static int     g_attunedBound      = 0;
static int     g_donorsResolved    = 0;   /* SoulLink slot 5/33 cached once  */
static int     g_initFirstLogDone  = 0;   /* one-shot startup banner          */
static int     g_gonMergeRequested = 0;   /* one-shot GON push to core        */
static HMODULE g_hSelf             = NULL; /* our own DLL handle from DllMain */


/* ===================================================================
 *  Logging helper
 *
 *  Forwards to csf.Log if Core is resolved, OutputDebugString as a
 *  fallback.  Keeps the legacy "[AT] ..." prefix so log grepping still
 *  works across the refactor.
 * =================================================================== */

static void ATLog(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    if (g_csfReady && g_csf.Log)
        g_csf.Log("%s", buf);
    else
        OutputDebugStringA(buf);
}


/* ===================================================================
 *  Registry helpers
 * =================================================================== */

static int AT_FindIndex(void* instance)
{
    int i;
    for (i = 0; i < g_atCount; i++)
        if (g_atRegistry[i].instance == instance) return i;
    return -1;
}

static void AT_Register(void* instance, int kind)
{
    void* entity;
    void* character;

    if (!instance) return;
    if (AT_FindIndex(instance) >= 0) return;

    entity = *(void**)((char*)instance + PASSIVE_ENTITY_OFFSET);

    if (g_atCount >= AT_MAX_INSTANCES)
    {
        ATLog("[AT] registry full (%d), can't register 0x%p",
              AT_MAX_INSTANCES, instance);
        return;
    }

    character = *(void**)((char*)instance + PASSIVE_CHAR_OFFSET);

    g_atRegistry[g_atCount].instance    = instance;
    g_atRegistry[g_atCount].entity      = entity;
    g_atRegistry[g_atCount].character   = character;
    g_atRegistry[g_atCount].kind        = kind;
    g_atRegistry[g_atCount].event_count = 0;
    g_atCount++;

    ATLog("[AT] registered %s 0x%p entity=0x%p char=0x%p (total %d)",
          (kind == ATKIND_TUNER) ? "Tuner" : "Attuned",
          instance, entity, character, g_atCount);
}

/* Swap-remove. Safe to call if not registered. */
static void AT_Deregister(void* instance)
{
    int idx = AT_FindIndex(instance);
    if (idx < 0) return;

    ATLog("[AT] deregistering %s 0x%p (%d of %d)",
          (g_atRegistry[idx].kind == ATKIND_TUNER) ? "Tuner" : "Attuned",
          instance, idx, g_atCount);

    g_atCount--;
    if (idx != g_atCount)
        g_atRegistry[idx] = g_atRegistry[g_atCount];

    g_atRegistry[g_atCount].instance    = 0;
    g_atRegistry[g_atCount].entity      = 0;
    g_atRegistry[g_atCount].kind        = 0;
    g_atRegistry[g_atCount].event_count = 0;
}


/* ===================================================================
 *  Class pool deregistration -- isolate Tuner/Attuned from SoulLink
 *
 *  GON-defined statuses register under the donor's type IDs (slot 3
 *  is inherited). Vanilla SoulLink's slot 11 iterates the leaf class
 *  pool to find peers, so it picks up our clones. We remove ourselves
 *  from that pool after OnInit, preserving base-class membership for
 *  the engine's general queries.
 *
 *  Data layout:
 *    instance+0x18 -> classRef+0x08 -> classDesc
 *    classDesc+0x18 -> srcRegistry  (dynarray[type_idx], 0x10 each)
 *    classDesc+0x20 -> poolCache    (cache[type_idx],    0x10 each)
 *    srcEntry: { cap(4), count(4), data_ptr(8) }
 *    cacheEntry: { pool_ptr(8), dirty(1) }
 *    typeInfo from vtable[3]: indices=(int*), count=*(u64*)(+0x40)
 *    leaf = indices[count-1]
 * =================================================================== */

static void AT_DeregisterFromDonorPool(void* instance)
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

            ATLog("[AT] removed 0x%p from donor pool 0x%X (count now %d)",
                  instance, leafIdx, entryCount);
            return;
        }
    }
    ATLog("[AT] 0x%p not found in donor pool 0x%X", instance, leafIdx);
}


/* ===================================================================
 *  OnInit hooks (slot 33) -- register + isolate from donor pool
 * =================================================================== */

static void AT_OnInit_Tuner(void* self)
{
    if (g_origSoulLinkOnInit) g_origSoulLinkOnInit(self);
    AT_Register(self, ATKIND_TUNER);
    AT_DeregisterFromDonorPool(self);
}

static void AT_OnInit_Attuned(void* self)
{
    if (g_origSoulLinkOnInit) g_origSoulLinkOnInit(self);
    AT_Register(self, ATKIND_ATTUNED);
    AT_DeregisterFromDonorPool(self);
}


/* ===================================================================
 *  Destructor wrapper (slot 5) -- shared by both statuses
 * =================================================================== */

static void* AT_destructor_wrapper(void* self, UINT64 flags)
{
    AT_Deregister(self);
    if (g_origSoulLinkDtor)
        return g_origSoulLinkDtor(self, flags);
    return self;
}


/* ===================================================================
 *  Tuner collector (slot 67 -- OnReceivedDamageReport)
 *
 *  Queues incoming damage events. Dispatcher (slot 11) drains them.
 *
 *  Source extraction matches vanilla SoulLink slot 67:
 *    source    = *(*(*(event_data+8)) + 0x98)  (double deref + 0x98)
 *    flag_byte = *(*(event_data+8) + 0x5d)     (single deref + 0x5d)
 * =================================================================== */

static void Tuner_collector(void* self, void* event_data)
{
    unsigned int amount;
    void* source;
    unsigned char src_flag_byte;
    int idx;
    AT_InstanceData* data;
    AT_Event* ev;

    if (g_tunerDispatching) return;
    if (!event_data) return;

    amount = *(unsigned int*)((char*)event_data + 0x14);
    if ((int)amount <= 0) return;

    {
        void* event_ptr = *(void**)((char*)event_data + 8);
        source = NULL;
        src_flag_byte = 0;
        if (event_ptr)
        {
            void* inner = *(void**)event_ptr;
            src_flag_byte = *((unsigned char*)event_ptr + 0x5d);
            if (inner)
                source = *(void**)((char*)inner + 0x98);
        }
    }

    idx = AT_FindIndex(self);
    if (idx < 0) return;
    data = &g_atRegistry[idx];
    if (data->kind != ATKIND_TUNER) return;

    if (data->event_count >= AT_MAX_EVENTS)
    {
        ATLog("[Tuner] event queue full on 0x%p, dropping %u damage",
              self, amount);
        return;
    }

    ev = &data->events[data->event_count++];
    ev->amount       = amount;
    ev->source       = source;
    ev->src_flag_bit = (src_flag_byte >> 5) & 1;

    ATLog("[Tuner] collected %u dmg, src=0x%p flag=%d (queue %d)",
          amount, source, ev->src_flag_bit, data->event_count);

    *((unsigned char*)self + PASSIVE_HAS_PENDING) = 1;
    *((unsigned char*)self + PASSIVE_DISPATCHED_FLAG) = 0;
}


/* ===================================================================
 *  Tuner dispatcher (slot 11 -- LateUpdate)
 *
 *  Drains queued events, forwarding each to every live Attuned.
 *  Mirrors vanilla SoulLink's slot 11 pattern:
 *
 *  Slot 169 reads inst+0xC8; if non-NULL and *(+0xC8-8)==+0xD0,
 *  writes *(+0xC8+0x60) into buf[0]. buf[0] becomes the "source
 *  character data" stored to entity+0x4D8 by damage_applier.
 *
 *  We set +0xC8 = source, +0xD0 = *(source-8) to match vanilla.
 * =================================================================== */

static void Tuner_dispatcher(void* self)
{
    int selfIdx, ev, i;
    AT_InstanceData* selfData;
    fn_apply_damage applyDmg;

    selfIdx = AT_FindIndex(self);
    if (selfIdx < 0 || g_atRegistry[selfIdx].kind != ATKIND_TUNER)
        goto done;

    selfData = &g_atRegistry[selfIdx];
    if (selfData->event_count == 0) goto done;

    if (g_tunerDispatching)
    {
        selfData->event_count = 0;
        goto done;
    }
    g_tunerDispatching = 1;
    applyDmg = (fn_apply_damage)(g_gameBase + RVA_APPLY_DAMAGE);

    for (ev = 0; ev < selfData->event_count; ev++)
    {
        AT_Event* event = &selfData->events[ev];
        int attuned_found = 0;

        for (i = 0; i < g_atCount; i++)
        {
            AT_InstanceData* tgt = &g_atRegistry[i];
            void* tgtInstance;
            void* tgtEntity;
            unsigned char dying;
            unsigned char buf[DAMAGE_INSTANCE_SIZE];
            fn_slot169_template populate;
            void** tgtVtable;
            void* srcObj;
            void* savedC8;
            void* savedD0;

            if (tgt->kind != ATKIND_ATTUNED) continue;
            tgtInstance = tgt->instance;
            if (!tgtInstance) continue;

            dying = *((unsigned char*)tgtInstance + PASSIVE_DYING_FLAG);
            if (dying != 0) continue;

            tgtEntity = tgt->entity;
            if (!tgtEntity) {
                tgtEntity = *(void**)((char*)tgtInstance + PASSIVE_ENTITY_OFFSET);
                tgt->entity = tgtEntity;
            }
            if (!tgtEntity) continue;

            /* alive check: entity+0x4C2 == 0 */
            {
                char aliveFlag = *((char*)tgtEntity + 0x4c2);
                if (aliveFlag != 0) continue;
            }

            attuned_found++;
            srcObj = event->source;

            /* Set +0xC8/+0xD0 so slot 169 writes correct source into buf[0].
             * Save and restore around the call. */
            savedC8 = *(void**)((char*)tgtInstance + 0xC8);
            savedD0 = *(void**)((char*)tgtInstance + 0xD0);

            if (srcObj)
            {
                *(void**)((char*)tgtInstance + 0xC8) = srcObj;
                *(void**)((char*)tgtInstance + 0xD0) = *(void**)((char*)srcObj - 8);
            }
            else
            {
                *(void**)((char*)tgtInstance + 0xC8) = NULL;
                *(void**)((char*)tgtInstance + 0xD0) = NULL;
            }

            /* Populate DamageInstance via slot 169 template */
            tgtVtable = *(void***)tgtInstance;
            populate = (fn_slot169_template)tgtVtable[169];

            memset(buf, 0, sizeof(buf));
            populate(tgtInstance, buf, event->amount, srcObj, 1);

            /* Echo bit: suppress re-entry through our collector */
            buf[DI_ECHO_BIT_OFFSET] |= DI_ECHO_BIT_MASK;
            buf[DI_SRCFLAG_OFFSET] =
                (unsigned char)((event->src_flag_bit & 1) << 5)
              | (buf[DI_SRCFLAG_OFFSET] & 0xdf);

            ATLog("[Tuner] fwd %u dmg -> Attuned 0x%p (ent 0x%p) src=0x%p buf[0]=0x%p",
                  event->amount, tgtInstance, tgtEntity, srcObj, *(void**)buf);

            applyDmg(tgtEntity, buf);

            *(void**)((char*)tgtInstance + 0xC8) = savedC8;
            *(void**)((char*)tgtInstance + 0xD0) = savedD0;
        }

        if (attuned_found == 0)
            ATLog("[Tuner] event %d: no live Attuned targets", ev);
    }

    selfData->event_count = 0;
    g_tunerDispatching = 0;

done:
    *((unsigned char*)self + PASSIVE_HAS_PENDING) = 0;
    *((unsigned char*)self + PASSIVE_DISPATCHED_FLAG) = 1;
}


/* ===================================================================
 *  Attuned slots -- no-op stubs
 *
 *  Attuned receives damage but does not forward. Stubs clear the
 *  pending flag so the game doesn't repeatedly wake the instance.
 * =================================================================== */

static void Attuned_collector(void* self, void* event_data)
{
    (void)event_data;
    *((unsigned char*)self + PASSIVE_HAS_PENDING) = 0;
    *((unsigned char*)self + PASSIVE_DISPATCHED_FLAG) = 1;
}

static void Attuned_dispatcher(void* self)
{
    *((unsigned char*)self + PASSIVE_HAS_PENDING) = 0;
    *((unsigned char*)self + PASSIVE_DISPATCHED_FLAG) = 1;
}


/* ===================================================================
 *  Init entry point
 *
 *  Driven by CSFCore_InitOrWait from csf_core_api.h.  The helper runs
 *  this callback once synchronously from DllMain; if it returns 0 (not
 *  fully bound yet) the helper arms a thread-pool timer that re-polls
 *  every CSF__RETRY_INTERVAL_MS until either every status is bound or
 *  CSF__RETRY_MAX_ATTEMPTS passes.  This lets modders ship the
 *  extension DLL without bundling csf_core.dll next to it -- as long
 *  as any other installed mod provides csf_core, we'll latch onto it
 *  once it loads and wins the active-instance race.
 *
 *  The callback is idempotent: every call re-probes per-status bind
 *  state via flags and CSFCore_GetConfig, and only calls
 *  RegisterExtension for slots that haven't already been installed.
 * =================================================================== */

/* Try to bind all 4 slot overrides for one status.  Returns 1 if the
 * status is fully bound (either right now or was already bound earlier),
 * 0 if the status isn't known to Core yet (expected during retry wait)
 * or if any RegisterExtension call failed. */
static int AT_TryBindStatus(const char* name, int* boundFlag,
                            void* slot5, void* slot11,
                            void* slot33, void* slot67)
{
    CSFStatusConfigView view;
    int ok;

    if (*boundFlag) return 1;

    /* Probe Core's registry before touching RegisterExtension.  This
     * keeps Core's own error log quiet during the "status not loaded
     * yet" window -the caller mod's GON may not have been merged in
     * yet, so the status literally doesn't exist from Core's POV. */
    view.struct_size = sizeof(view);
    if (!g_csf.GetConfig(name, &view)) {
        return 0;   /* not registered yet -keep polling */
    }

    ok  = g_csf.RegisterExtension(name,  5, slot5);
    ok &= g_csf.RegisterExtension(name, 11, slot11);
    ok &= g_csf.RegisterExtension(name, 33, slot33);
    ok &= g_csf.RegisterExtension(name, 67, slot67);

    if (!ok) {
        g_csf.Log("[AT] ERROR: RegisterExtension failed for '%s' -- "
                  "some slots may be half-bound", name);
        return 0;
    }

    *boundFlag = 1;
    g_csf.Log("[AT] bound '%s' slots 5/11/33/67", name);
    return 1;
}

/* Idempotent init callback driven by CSFCore_InitOrWait.  Returns 1
 * only when BOTH Tuner and Attuned are fully bound; otherwise 0 so the
 * retry loop keeps polling. */
static int AttunedTuner_Init_Cb(CSFCoreAPI* api)
{
    if (!api) return 0;

    /* Cache a local copy of the resolved API on first entry.  We stash
     * it in g_csf so every other file-local helper (ATLog, the
     * wrappers, etc.) can keep using g_csf without caring about the
     * deferred init path. */
    if (!g_csfReady) {
        g_csf      = *api;
        g_csfReady = 1;
        g_gameBase = g_csf.GetGameBase();
    }

    if (!g_initFirstLogDone) {
        g_csf.Log("[AT] csf_core v%d resolved, gameBase=0x%016llX",
                  g_csf.GetVersion(), (unsigned long long)g_gameBase);
        g_initFirstLogDone = 1;
    }

    /* Push our custom_statuses.gon into core so it knows about the
     * statuses this extension defines.  Without this, core only
     * scanned its own directory and never found Tuner/Attuned.
     * One-shot: the merge is idempotent, but expensive to repeat. */
    if (!g_gonMergeRequested && g_hSelf) {
        if (CSFCore_MergeGonFromMyDir(g_hSelf)) {
            g_csf.Log("[AT] GON merge from ext directory requested");
        } else {
            g_csf.Log("[AT] GON merge returned 0 "
                      "(no GON found, or core export unavailable)");
        }
        g_gonMergeRequested = 1;
    }

    /* Resolve SoulLink donor slots once.  These live in csf_core's
     * donor-slot table populated from vanilla status discovery, so
     * they're available the moment csf_core is active. */
    if (!g_donorsResolved) {
        g_origSoulLinkDtor   = (fn_destructor)
            g_csf.GetDonorSlotFn("SoulLink", 5);
        g_origSoulLinkOnInit = (fn_on_init)
            g_csf.GetDonorSlotFn("SoulLink", 33);

        if (!g_origSoulLinkDtor || !g_origSoulLinkOnInit) {
            /* SoulLink is a vanilla status so this really shouldn't
             * happen -- treat it as a retry condition just in case
             * the donor table is being built lazily. */
            return 0;
        }

        g_csf.Log("[AT] SoulLink originals: dtor=0x%p OnInit=0x%p",
                  g_origSoulLinkDtor, g_origSoulLinkOnInit);
        g_donorsResolved = 1;
    }

    /* Try each status independently.  Either may become available
     * before the other if their GONs are merged in different passes. */
    AT_TryBindStatus("Tuner",   &g_tunerBound,
                     (void*)AT_destructor_wrapper,
                     (void*)Tuner_dispatcher,
                     (void*)AT_OnInit_Tuner,
                     (void*)Tuner_collector);

    AT_TryBindStatus("Attuned", &g_attunedBound,
                     (void*)AT_destructor_wrapper,
                     (void*)Attuned_dispatcher,
                     (void*)AT_OnInit_Attuned,
                     (void*)Attuned_collector);

    return (g_tunerBound && g_attunedBound) ? 1 : 0;
}


/* ===================================================================
 *  DllMain
 *
 *  Hands the init work off to CSFCore_InitOrWait.  If csf_core is
 *  already loaded (common case when a mod bundles csf_core in the same
 *  directory and Mewjector loads it first), the init runs synchronously
 *  and we're done before DllMain returns.  If csf_core isn't loaded
 *  yet, the helper arms a background retry timer so the extension
 *  binds the moment a csf_core wins its active-instance race.
 * =================================================================== */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hSelf = hModule;
        CSFCore_InitOrWait(AttunedTuner_Init_Cb);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        /* Tear down the CSFCore_InitOrWait retry timer before our
         * code pages go away.  If the retry callback ever fires
         * after DLL_PROCESS_DETACH returns, it would jump into
         * freed memory and crash -Shutdown cancels the timer,
         * drains in-flight callbacks, and closes the handle.  Safe
         * to call even if InitOrWait never deferred (the helper
         * no-ops when the timer was never created). */
        CSFCore_Shutdown();
    }
    return TRUE;
}
