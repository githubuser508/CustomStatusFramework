# Custom Status Framework (betav0.5)

A framework for adding new status effects to Mewgenics through GON data files,
with optional C extensions for statuses that need custom logic.

Beta build. Might break. (Will break.) Ping @Maishul on Discord with the logs.

## How It Works

Every custom status is a clone of a vanilla "donor" status. The framework
hijacks the donor's factory call, swaps in a custom vtable, registers the new
status in the game's internal maps, and manages per-instance data in a DLL-side
sidecar so that the game's own memory layout is never touched.

GON-only statuses need zero C code. If you need custom tick logic, damage
reactions, immunity grants, or stat bonuses, the bundled behavior pack
(`basic_custom_extensions`) covers the common cases. Ping @Maishul on
the modding Discord if you think you have a basic case we should implement.
Full C extensions are available for anything the behavior pack doesn't cover.

## Quick Start

### Pure GON (no C)

```
custom_statuses {
    AccumulateDoomed {
        donor      Doomed
        display    "Doomed+"
        category   debuff
        apply_mode accumulate
    }
}
```

Clones Doomed and changes its stacking behavior. Nothing else required.

### GON with Bundled Behaviors

```
custom_statuses {
    GreenBleed {
        donor   Bleed
        display "GreenBleed"
        icon    8

        slot_overrides {
            OnTurnEnd   CSFExt_TickDecrement
            OnInit      CSFExt_DeregisterFromDonorPool
        }
    }
}
```

Clones Bleed, uses the Poison icon (frame 8), counts down each turn, and
removes itself from the donor's shared instance pool so it doesn't collide
with real Bleed instances.

### GON with a C Extension

```
custom_statuses {
    Tuner {
        donor    SoulLink
        display  "Tuner"
        category info

        slot_overrides {
            Destructor              Tuner_dtor
            LateUpdate              Tuner_dispatcher
            OnInit                  Tuner_on_init
            OnReceivedDamageReport  Tuner_collector
        }
    }
}
```

Each overridden slot name that isn't a recognized bundled behavior is treated
as a label. The C extension DLL binds the actual function pointer at load time
via `CSFCore_RegisterExtension(statusName, slotIndex, fnPtr)`.


## GON Fields

### Identity

| Field     | Type   | Required | Notes |
|-----------|--------|----------|-------|
| `donor`   | string | yes      | Vanilla status to clone (see `DonorLookup.inc` for the full list). |
| `display` | string | no       | Name shown in-game. Defaults to the block name. |
| `icon`    | int    | no       | Status icon frame number. Matches `iconNNNN.png` in the vanilla atlas (e.g. 5 = Freeze, 8 = Poison, 14 = Bleed). Omit to inherit the donor's icon. |

### Registry

| Field        | Values                              | Default           |
|--------------|-------------------------------------|--------------------|
| `category`   | `buff` (0), `debuff` (1), `info` (2) | clones donor     |
| `apply_mode` | `accumulate` (0), `presence` (1), `overwrite` (2) | clones donor |

### Parameters

```
params {
    stacks    5
    scale     0.75
}
```

Named values stored in the instance sidecar. Accessible from C extensions via
`CSF_SidecarInt(sc, "stacks")` / `CSF_SidecarDbl(sc, "scale")`.


## Bundled Behaviors (basic_custom_extensions)

These can be used by name in `slot_overrides` without writing any C code.

### Lifecycle

| Behavior | What it does |
|----------|-------------|
| `CSFExt_DeregisterFromDonorPool` | Runs after the donor's OnInit and removes this instance from the donor's shared pool, preventing cross-contamination between real and custom instances. |
| `CSFExt_No_Op` | Does nothing; replaces any VoidSelf slot you want to silence. |
| `CSFExt_No_Op_Destructor` | Does nothing; replaces the donor's destructor if you need a clean teardown without donor side effects. |

### Tick Counter (any VoidSelf slot)

| Behavior | What it does |
|----------|-------------|
| `CSFExt_TickDecrement` | Decreases stacks by 1 each tick and removes the status when it hits zero. |
| `CSFExt_TickDecrementRemoveAtZero` | Same as above (long-form name). |
| `CSFExt_TickIncrement` | Increases stacks by 1 each tick; never removes. |
| `CSFExt_TickDecrementAllowNegativeStacks` | Decreases stacks by 1 each tick but lets them go negative instead of removing. |

### Damage-Reactive Counter (slot 67 -- OnReceivedDamageReport)

| Behavior | What it does |
|----------|-------------|
| `CSFExt_CounterSubtractDamageTaken` | Subtracts the incoming damage from stacks. Removes the status at zero or below. |
| `CSFExt_CounterAddDamageTaken` | Adds the incoming damage to stacks; never removes. |

### Damage Cap (slot 63 -- OnReceivedDamage)

| Behavior | What it does |
|----------|-------------|
| `CSFExt_DamageCap` | Clamps incoming damage to a maximum. Uses the named GON param `damage_cap` if set, otherwise falls back to current stacks. |

### Element Reactions (slot 80 -- OnElementInfluence)

| Behavior | What it does |
|----------|-------------|
| `CSFExt_RemoveOnFire` | Removes the status when the bearer is hit by fire. |
| `CSFExt_RemoveOnWater` | Removes the status when the bearer is hit by water. |
| `CSFExt_RemoveOnCold` | Removes the status when the bearer is hit by cold. |
| `CSFExt_RemoveOnHeat` | Removes the status when the bearer is hit by heat. |

### Immunity Grants (slot 81 -- GetImmunity)

| Behavior | What it does |
|----------|-------------|
| `CSFExt_PublishImmunity_Burn` | While active, the bearer is immune to Burn. |
| `CSFExt_PublishImmunity_Wet` | While active, the bearer is immune to Wet. |
| `CSFExt_PublishImmunity_Cold` | While active, the bearer is immune to Cold. |
| `CSFExt_PublishImmunity_Bleed` | While active, the bearer is immune to Bleed. |
| `CSFExt_PublishImmunity_Immobilize` | While active, the bearer is immune to Immobilize. |
| `CSFExt_PublishImmunity_Incapacitate` | While active, the bearer is immune to Incapacitate. |
| `CSFExt_PublishImmunity_Petrify` | While active, the bearer is immune to Petrify. |
| `CSFExt_PublishImmunity_Freeze` | While active, the bearer is immune to Freeze. |

### Stat Bonuses (slot 82 -- OnStatModify)

Each adds the status's stack count to one bearer stat. Pair with
`params { stacks N }` to set the bonus amount.

| Behavior | Stat |
|----------|------|
| `CSFExt_StatBonus_Strength` | Strength |
| `CSFExt_StatBonus_Dexterity` | Dexterity |
| `CSFExt_StatBonus_Constitution` | Constitution |
| `CSFExt_StatBonus_Intelligence` | Intelligence |
| `CSFExt_StatBonus_Speed` | Speed |
| `CSFExt_StatBonus_Charisma` | Charisma |
| `CSFExt_StatBonus_Luck` | Luck |


## Slot Aliases for GON

Named aliases usable in `slot_overrides`. Only the named slots are listed here;
see [VtableSlotReference](docs/VtableSlotReference.md) for the full 172-slot table.

| Alias | Slot | Signature | Confidence | Notes |
|-------|------|-----------|------------|-------|
| `Destructor` | 5 | Dtor | proven | |
| `Start` | 6 | VoidSelf | low | |
| `LateUpdate` | 11 | VoidSelf | verified | Per-frame tick |
| `OnInit` | 33 | VoidSelf | proven | Fires once after creation |
| `OnTurnBegin` | 34 | VoidSelf | medium | |
| `OnTurnEnd` | 35 | VoidSelf | high | |
| `OnTurnBeginQueued` | 36 | VoidSelf | verified | |
| `OnTurnEndQueued` | 37 | VoidSelf | verified | |
| `OnTileEndMove` | 38 | VoidSelf | low | |
| `OnRoundRevive` | 39 | VoidSelf | low | |
| `OnRoundEndQueued` | 41 | VoidSelf | verified | |
| `Trap` | 42 | VoidSelf | low | |
| `CollectBy` | 43 | VoidSelf | low | |
| `PreAction` | 44 | VoidSelf | low | |
| `OnRemove` | 45 | VoidSelf | verified | Fires when the status is removed |
| `OnBattleStart` | 48 | VoidSelf | stub | |
| `OnSpawnIn` | 49 | VoidSelf | low | |
| `OnAnyCharacterTurnEnd` | 54 | SelfI64 | medium | |
| `OnTrigger` | 55 | SelfI64I64 | low | |
| `OnCastAbility` | 56 | SelfI64 | medium | |
| `OnAttackValidate` | 58 | SelfI64 | low | |
| `OnDealDamage` | 59 | SelfI64I64I8 | low | |
| `OnReceivedDamage` | 63 | SelfI64PI8 | verified | `void(self, damage_ctx*, char flag)`. Flag in R8B gates Brace/Stealth's attacker-vs-bearer check. |
| `OnWillDieFromDamage` | 65 | VoidSelf | stub | |
| `OnDamageReport` | 66 | SelfI64I64 | low | |
| `OnReceivedDamageReport` | 67 | SelfI64 | verified | |
| `OnDodgedDamage` | 68 | VoidSelf | stub | |
| `OnBlockedDamage` | 69 | VoidSelf | stub | |
| `OnElementInfluence` | 80 | SelfI32P | verified | Element reaction callback |
| `GetImmunity` | 81 | PtrRetOut | verified | Immunity publisher |
| `OnStatModify` | 82 | VoidSelf | verified | Stat bonus callback |
| `OnFormTransform` | 83 | VoidSelf | low | |
| `OnRefreshAbility` | 89 | SelfI64 | verified | |
| `OnPreIdleUpkeep` | 90 | VoidSelf | high | |

Confidence levels reflect how thoroughly each slot's behavior has been verified
through Ghidra analysis and runtime testing. "Proven" and "verified" slots are
safe to override; "low" and "stub" slots work but have seen less testing.

If you make a mod that verifies one of the above signatures, please let
@Maishul know on Discord.


## Building

Requires MSVC (Visual Studio Build Tools). Run `build.bat` from the repo
root. Output is `csf_core.dll`.
Extension DLLs build separately against `csf_core_api.h`.

Note: the .bat file is a personal dev tool that I included for convenience
and may require some fnangling on your side to get working.

## License

MIT. See [LICENSE](LICENSE).
