# Vtable Slot Reference

Status instances are 0xF0 bytes and share a 172-slot vtable. Names come from
lambda RTTI analysis and targeted decompilation of the game binary.


## Confidence Levels

| Level      | Meaning |
|------------|---------|
| `proven`   | Overridden, loaded, and observed firing at runtime in a shipped mod. |
| `verified` | Body decompiled and effect confirmed, but not yet exercised by a mod. |
| `high`     | Name attested by 3+ independent RTTI lambda classes. |
| `medium`   | Name attested by 2 classes. |
| `low`      | Name attested by 1 class. Treat as a hint. |

The higher label wins when a slot has both behavior-level and name-level confidence.


## Instance Layout

| Offset | Size | Field |
|-------:|:----:|-------|
| +0x00  | ptr  | Vtable pointer |
| +0x38  | ptr  | Bearer entity struct |
| +0x5C  | i32  | Stack count / duration counter |
| +0x74  | u8   | Pending flag (deferred work queued) |
| +0x75  | u8   | Death / removal-in-progress flag |

+0x5C is both the stack count and the duration counter. Turn-tick slots
decrement it and call the removal helper when it hits 0. Statuses with real
multi-stack behavior read the same field directly.


## Named Slots

Slots marked UNIQUE have one-off calling conventions and can't use the
framework's generic dispatcher. They're listed for reference but aren't
overridable through `slot_overrides` in GON.

### Identity (0--4)

| Slot | Name | Confidence | Notes |
|------|------|:----------:|-------|
| 0 | `GetName` | proven | Returns status name as MSVC SSO string. |
| 1 | `GetTypeId` | proven | Returns leaf type ID. |
| 2 | `TypeNameCheck` | proven | UNIQUE. Compares a name string against this status. |
| 3 | `GetTypeInfo` | proven | Returns `type_info*`. |
| 4 | `BaseInitializer` | low | UNIQUE. Only 4 donors use it. |

### Lifecycle (5--32)

| Slot | Name | Confidence | Notes |
|------|------|:----------:|-------|
| 5  | `Destructor` | proven | Instance cleanup. Free your sidecar here. |
| 6  | `Start` | low | |
| 11 | `LateUpdate` | verified | Per-frame tick. |
| 28 | `GetDisplayDesc` | high | UNIQUE. Returns the display descriptor the HUD uses for icon/grouping. |

Slots 7--10, 12--27, 29--32 are unnamed. Most are stubs across all 303 donors.

### Turn / Round (33--51)

| Slot | Name | Confidence | Notes |
|------|------|:----------:|-------|
| 33 | `OnInit` | proven | Fires once after creation. 136 donors use it. |
| 34 | `OnTurnBegin` | medium | |
| 35 | `OnTurnEnd` | high | Duration countdown and periodic effects. |
| 36 | `OnTurnBeginQueued` | verified | Burn uses this to apply damage scaled by stacks. |
| 37 | `OnTurnEndQueued` | verified | Rot uses this to spawn popups and decrement. |
| 38 | `OnTileEndMove` | low | |
| 39 | `OnRoundRevive` | low | |
| 41 | `OnRoundEndQueued` | verified | Bleed uses this for its per-round damage tick. |
| 42 | `Trap` | low | |
| 43 | `CollectBy` | low | |
| 44 | `PreAction` | low | |
| 45 | `OnRemove` | verified | Fires when the status is removed. 20 engine call sites. |
| 48 | `OnBattleStart` | medium | |
| 49 | `OnSpawnIn` | low | |

### Character / Damage Callbacks (52--69)

| Slot | Name | Confidence | Notes |
|------|------|:----------:|-------|
| 52 | `OnAnyCharacterMakesDecision` | high | UNIQUE. |
| 54 | `OnAnyCharacterTurnEnd` | medium | |
| 55 | `OnTrigger` | low | |
| 56 | `OnCastAbility` | medium | |
| 57 | `OnAttackHeal` | -- | UNIQUE. |
| 58 | `OnAttackValidate` | low | |
| 59 | `OnDealDamage` | low | |
| 61 | `OnDamageReceived` | -- | UNIQUE. |
| 63 | `OnReceivedDamage` | verified | Signature `void(self, damage_ctx*, char flag)` (3-arg, SelfI64PI8). Incoming damage modifier. Brace subtracts stacks from damage (floor 1). Marked forces crit. Flag in R8B is read by Brace / Stealth / OldStealth to gate an attacker-vs-bearer friendly-fire check; Marked / Zombie ignore it. |
| 64 | `OnAppliedStatuses` | high | UNIQUE. |
| 65 | `OnWillDieFromDamage` | medium | |
| 66 | `OnDamageReport` | low | |
| 67 | `OnReceivedDamageReport` | verified | Post-damage notification. SoulLink queues damage entries for later dispatch. |
| 68 | `OnDodgedDamage` | low | |
| 69 | `OnBlockedDamage` | low | |

### Element / Immunity / Stats (80--90)

| Slot | Name | Confidence | Notes |
|------|------|:----------:|-------|
| 80 | `OnElementInfluence` | verified | Reacts to element bitmask. Burn self-removes on Water (bit 3). Freeze self-removes on Fire (bit 2). |
| 81 | `GetImmunity` | verified | Returns bearer immunity bitmask. 16 engine call sites. |
| 82 | `OnStatModify` | verified | Called during stat aggregation. AllStatsUp adds stacks to each stat field. |
| 83 | `OnFormTransform` | low | |
| 89 | `OnRefreshAbility` | verified | Called per-ability during stat resolve. Hex adds stacks to ability cost. |
| 90 | `OnPreIdleUpkeep` | high | |

### Death (94--95)

| Slot | Name | Confidence | Notes |
|------|------|:----------:|-------|
| 94 | `OnDeath` | medium | |
| 95 | `OnDeathRattle` | low | |

### Late Slots (101+)

| Slot | Name | Confidence | Notes |
|------|------|:----------:|-------|
| 101 | `CharacterDies` | high | |
| 147 | `CharacterInAuraEndTurn` | high | |
| 150 | `CheckCounter` | high | |
| 151 | `Trap` | high | Separate from slot 42. |
| 154 | `CharacterInAuraOnCastAbility` | high | |
| 170 | `OnStackChange` | verified | Fires on every stack count change. 61 engine call sites -- the most-queried slot. |


## Signature Groups

Slot overrides in GON are type-checked against these signature families.
Two slots are compatible if they share a signature group.

| Signature | Calling convention | Slots |
|-----------|--------------------|-------|
| VoidSelf | `void(self)` | Most lifecycle and tick slots (6, 11, 33--45, 48--49, 65, 68--69, 82--83, 90, ...) |
| SelfI64 | `void(self, int64)` | 54, 56, 58, 63, 67, 85, 89 |
| SelfI64I64 | `void(self, int64, int64)` | 55, 66 |
| SelfI64I64I8 | `void(self, int64, int64, int8)` | 59 |
| SelfI32P | `void(self, int32*)` | 80 |
| PtrRetOut | `void*(self, void* out)` | 0, 29, 81 |
| Dtor | `void(self)` | 5 only (destructor-specific dispatch) |
| SelfRetI64 | `int64(self)` | 1 |
| SelfRetPtr | `void*(self)` | 3 |

Slots marked UNIQUE above have one-off conventions not in this table.
