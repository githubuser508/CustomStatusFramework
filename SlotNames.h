// SlotNames.h - Vtable slot name definitions for Custom Status Framework
// Auto-generated from Lambda RTTI analysis + proven CSF code + behavioral analysis
// Confidence: PROVEN (working code), HIGH (3+ RTTI classes), MEDIUM (2 classes), LOW (1 class/behavioral)
//
// Total: 172 slots, 64 named (10 proven, 14 high, 10 medium, 30 low)

#pragma once

// Slot name enum - maps vtable slot indices to human-readable names
enum VtableSlot {
    SLOT_GetName = 0,  // [proven]
    SLOT_GetTypeId = 1,  // [proven]
    SLOT_TypeNameCheck = 2,  // [proven]
    SLOT_GetTypeInfo = 3,  // [proven]
    SLOT_BaseInitializer = 4,  // [low]
    SLOT_Destructor = 5,  // [proven]
    SLOT_Start = 6,  // [low]
    SLOT_UNKNOWN_7 = 7,
    SLOT_UNKNOWN_8 = 8,
    SLOT_UNKNOWN_9 = 9,
    SLOT_UNKNOWN_10 = 10,
    SLOT_LateUpdate = 11,  // [high]
    SLOT_UNKNOWN_12 = 12,
    SLOT_UNKNOWN_13 = 13,
    SLOT_UNKNOWN_14 = 14,
    SLOT_UNKNOWN_15 = 15,
    SLOT_UNKNOWN_16 = 16,
    SLOT_UNKNOWN_17 = 17,
    SLOT_UNKNOWN_18 = 18,
    SLOT_UNKNOWN_19 = 19,
    SLOT_UNKNOWN_20 = 20,
    SLOT_UNKNOWN_21 = 21,
    SLOT_UNKNOWN_22 = 22,
    SLOT_UNKNOWN_23 = 23,
    SLOT_UNKNOWN_24 = 24,
    SLOT_UNKNOWN_25 = 25,
    SLOT_UNKNOWN_26 = 26,
    SLOT_UNKNOWN_27 = 27,
    SLOT_GetDisplayDesc = 28,  // [proven]
    SLOT_UNKNOWN_29 = 29,
    SLOT_UNKNOWN_30 = 30,
    SLOT_UNKNOWN_31 = 31,
    SLOT_init = 32,  // [low]
    SLOT_OnInit = 33,  // [proven]
    SLOT_OnTurnBegin = 34,  // [medium]
    SLOT_OnTurnEnd = 35,  // [high]
    SLOT_OnTurnBeginQueued = 36,  // [medium]
    SLOT_OnTurnEndQueued = 37,  // [proven]
    SLOT_OnTileEndMove = 38,  // [low]
    SLOT_OnRoundRevive = 39,  // [low]
    SLOT_UNKNOWN_40 = 40,
    SLOT_OnRoundEndQueued = 41,  // [proven]
    SLOT_Trap_42 = 42,  // [low] (dup of slot 151 'Trap' [high])
    SLOT_CollectBy = 43,  // [low]
    SLOT_PreAction = 44,  // [low]
    SLOT_OnRemove = 45,  // [high]
    SLOT_UNKNOWN_46 = 46,
    SLOT_UNKNOWN_47 = 47,
    SLOT_OnBattleStart = 48,  // [medium]
    SLOT_OnSpawnIn = 49,  // [low]
    SLOT_UNKNOWN_50 = 50,
    SLOT_UNKNOWN_51 = 51,
    SLOT_OnAnyCharacterMakesDecision = 52,  // [high]
    SLOT_UNKNOWN_53 = 53,
    SLOT_OnAnyCharacterTurnEnd = 54,  // [medium]
    SLOT_OnTrigger = 55,  // [low]
    SLOT_OnCastAbility = 56,  // [medium]
    SLOT_OnAttackHeal = 57,  // [low]
    SLOT_OnAttackValidate = 58,  // [low]
    SLOT_OnDealDamage = 59,  // [low]
    SLOT_UNKNOWN_60 = 60,
    SLOT_OnDamageReceived = 61,  // [low]
    SLOT_UNKNOWN_62 = 62,
    SLOT_OnReceivedDamage = 63,  // [medium]
    SLOT_OnAppliedStatuses = 64,  // [high]
    SLOT_OnWillDieFromDamage = 65,  // [medium]
    SLOT_OnDamageReport = 66,  // [low]
    SLOT_OnReceivedDamageReport = 67,  // [high]
    SLOT_OnDodgedDamage = 68,  // [low]
    SLOT_OnBlockedDamage = 69,  // [medium]
    SLOT_UNKNOWN_70 = 70,
    SLOT_UNKNOWN_71 = 71,
    SLOT_UNKNOWN_72 = 72,
    SLOT_UNKNOWN_73 = 73,
    SLOT_UNKNOWN_74 = 74,
    SLOT_UNKNOWN_75 = 75,
    SLOT_UNKNOWN_76 = 76,
    SLOT_UNKNOWN_77 = 77,
    SLOT_UNKNOWN_78 = 78,
    SLOT_UNKNOWN_79 = 79,
    SLOT_OnElementInfluence = 80,  // [high]
    SLOT_GetImmunity = 81,  // [proven]
    SLOT_OnStatModify = 82,  // [low]
    SLOT_OnFormTransform = 83,  // [low]
    SLOT_UNKNOWN_84 = 84,
    SLOT_UNKNOWN_85 = 85,
    SLOT_UNKNOWN_86 = 86,
    SLOT_UNKNOWN_87 = 87,
    SLOT_UNKNOWN_88 = 88,
    SLOT_OnRefreshAbility = 89,  // [high]
    SLOT_OnPreIdleUpkeep = 90,  // [high]
    SLOT_UNKNOWN_91 = 91,
    SLOT_SecondaryInit = 92,  // [low]
    SLOT_UNKNOWN_93 = 93,
    SLOT_OnDeath = 94,  // [medium]
    SLOT_OnDeathRattle = 95,  // [low]
    SLOT_UNKNOWN_96 = 96,
    SLOT_OnRunAway = 97,  // [low]
    SLOT_UNKNOWN_98 = 98,
    SLOT_OnKill = 99,  // [low]
    SLOT_UNKNOWN_100 = 100,
    SLOT_OnCharacterDies = 101,  // [high]
    SLOT_UNKNOWN_102 = 102,
    SLOT_UNKNOWN_103 = 103,
    SLOT_UNKNOWN_104 = 104,
    SLOT_UNKNOWN_105 = 105,
    SLOT_UNKNOWN_106 = 106,
    SLOT_OnSpawnCharacter = 107,  // [low]
    SLOT_UNKNOWN_108 = 108,
    SLOT_UNKNOWN_109 = 109,
    SLOT_UNKNOWN_110 = 110,
    SLOT_OnRequestAction = 111,  // [low]
    SLOT_OnCharacterRequestAction = 112,  // [medium]
    SLOT_UNKNOWN_113 = 113,
    SLOT_UNKNOWN_114 = 114,
    SLOT_UNKNOWN_115 = 115,
    SLOT_UNKNOWN_116 = 116,
    SLOT_UNKNOWN_117 = 117,
    SLOT_UNKNOWN_118 = 118,
    SLOT_UNKNOWN_119 = 119,
    SLOT_UNKNOWN_120 = 120,
    SLOT_UNKNOWN_121 = 121,
    SLOT_UNKNOWN_122 = 122,
    SLOT_UNKNOWN_123 = 123,
    SLOT_UNKNOWN_124 = 124,
    SLOT_UNKNOWN_125 = 125,
    SLOT_UNKNOWN_126 = 126,
    SLOT_UNKNOWN_127 = 127,
    SLOT_UNKNOWN_128 = 128,
    SLOT_UNKNOWN_129 = 129,
    SLOT_UNKNOWN_130 = 130,
    SLOT_OnHitOtherWithProjectile = 131,  // [low]
    SLOT_UNKNOWN_132 = 132,
    SLOT_UNKNOWN_133 = 133,
    SLOT_UNKNOWN_134 = 134,
    SLOT_UNKNOWN_135 = 135,
    SLOT_UNKNOWN_136 = 136,
    SLOT_UNKNOWN_137 = 137,
    SLOT_UNKNOWN_138 = 138,
    SLOT_UNKNOWN_139 = 139,
    SLOT_UNKNOWN_140 = 140,
    SLOT_UNKNOWN_141 = 141,
    SLOT_UNKNOWN_142 = 142,
    SLOT_UNKNOWN_143 = 143,
    SLOT_UNKNOWN_144 = 144,
    SLOT_CheckCounter_145 = 145,  // [low] (dup of slot 150 'CheckCounter' [high])
    SLOT_UNKNOWN_146 = 146,
    SLOT_CharacterInAuraEndTurn = 147,  // [high]
    SLOT_UNKNOWN_148 = 148,
    SLOT_UNKNOWN_149 = 149,
    SLOT_CheckCounter = 150,  // [high]
    SLOT_Trap = 151,  // [high]
    SLOT_UNKNOWN_152 = 152,
    SLOT_UNKNOWN_153 = 153,
    SLOT_CharacterInAuraOnCastAbility = 154,  // [high]
    SLOT_UNKNOWN_155 = 155,
    SLOT_CharacterInAuraSpendMana = 156,  // [low]
    SLOT_UNKNOWN_157 = 157,
    SLOT_UNKNOWN_158 = 158,
    SLOT_UNKNOWN_159 = 159,
    SLOT_UNKNOWN_160 = 160,
    SLOT_UNKNOWN_161 = 161,
    SLOT_UNKNOWN_162 = 162,
    SLOT_UNKNOWN_163 = 163,
    SLOT_UNKNOWN_164 = 164,
    SLOT_UNKNOWN_165 = 165,
    SLOT_CharacterInAuraOnDamageReport = 166,  // [low]
    SLOT_UNKNOWN_167 = 167,
    SLOT_UNKNOWN_168 = 168,
    SLOT_UNKNOWN_169 = 169,
    SLOT_OnStackChange = 170,  // [low]
    SLOT_CanCollect = 171,  // [low]
    VTABLE_SLOT_COUNT = 172
};

// String name lookup table
static const char* const SLOT_NAMES[172] = {
    "GetName",  // slot 0 [proven]
    "GetTypeId",  // slot 1 [proven]
    "TypeNameCheck",  // slot 2 [proven]
    "GetTypeInfo",  // slot 3 [proven]
    "BaseInitializer",  // slot 4 [low]
    "Destructor",  // slot 5 [proven]
    "Start",  // slot 6 [low]
    "Unknown_7",  // slot 7
    "Unknown_8",  // slot 8
    "Unknown_9",  // slot 9
    "Unknown_10",  // slot 10
    "LateUpdate",  // slot 11 [high]
    "Unknown_12",  // slot 12
    "Unknown_13",  // slot 13
    "Unknown_14",  // slot 14
    "Unknown_15",  // slot 15
    "Unknown_16",  // slot 16
    "Unknown_17",  // slot 17
    "Unknown_18",  // slot 18
    "Unknown_19",  // slot 19
    "Unknown_20",  // slot 20
    "Unknown_21",  // slot 21
    "Unknown_22",  // slot 22
    "Unknown_23",  // slot 23
    "Unknown_24",  // slot 24
    "Unknown_25",  // slot 25
    "Unknown_26",  // slot 26
    "Unknown_27",  // slot 27
    "GetDisplayDesc",  // slot 28 [proven]
    "Unknown_29",  // slot 29
    "Unknown_30",  // slot 30
    "Unknown_31",  // slot 31
    "init",  // slot 32 [low]
    "OnInit",  // slot 33 [proven]
    "OnTurnBegin",  // slot 34 [medium]
    "OnTurnEnd",  // slot 35 [high]
    "OnTurnBeginQueued",  // slot 36 [medium]
    "OnTurnEndQueued",  // slot 37 [proven]
    "OnTileEndMove",  // slot 38 [low]
    "OnRoundRevive",  // slot 39 [low]
    "Unknown_40",  // slot 40
    "OnRoundEndQueued",  // slot 41 [proven]
    "Trap_42",  // slot 42 [low] (dup of slot 151 'Trap' [high])
    "CollectBy",  // slot 43 [low]
    "PreAction",  // slot 44 [low]
    "OnRemove",  // slot 45 [high]
    "Unknown_46",  // slot 46
    "Unknown_47",  // slot 47
    "OnBattleStart",  // slot 48 [medium]
    "OnSpawnIn",  // slot 49 [low]
    "Unknown_50",  // slot 50
    "Unknown_51",  // slot 51
    "OnAnyCharacterMakesDecision",  // slot 52 [high]
    "Unknown_53",  // slot 53
    "OnAnyCharacterTurnEnd",  // slot 54 [medium]
    "OnTrigger",  // slot 55 [low]
    "OnCastAbility",  // slot 56 [medium]
    "OnAttackHeal",  // slot 57 [low]
    "OnAttackValidate",  // slot 58 [low]
    "OnDealDamage",  // slot 59 [low]
    "Unknown_60",  // slot 60
    "OnDamageReceived",  // slot 61 [low]
    "Unknown_62",  // slot 62
    "OnReceivedDamage",  // slot 63 [medium]
    "OnAppliedStatuses",  // slot 64 [high]
    "OnWillDieFromDamage",  // slot 65 [medium]
    "OnDamageReport",  // slot 66 [low]
    "OnReceivedDamageReport",  // slot 67 [high]
    "OnDodgedDamage",  // slot 68 [low]
    "OnBlockedDamage",  // slot 69 [medium]
    "Unknown_70",  // slot 70
    "Unknown_71",  // slot 71
    "Unknown_72",  // slot 72
    "Unknown_73",  // slot 73
    "Unknown_74",  // slot 74
    "Unknown_75",  // slot 75
    "Unknown_76",  // slot 76
    "Unknown_77",  // slot 77
    "Unknown_78",  // slot 78
    "Unknown_79",  // slot 79
    "OnElementInfluence",  // slot 80 [high]
    "GetImmunity",  // slot 81 [proven]
    "OnStatModify",  // slot 82 [low]
    "OnFormTransform",  // slot 83 [low]
    "Unknown_84",  // slot 84
    "Unknown_85",  // slot 85
    "Unknown_86",  // slot 86
    "Unknown_87",  // slot 87
    "Unknown_88",  // slot 88
    "OnRefreshAbility",  // slot 89 [high]
    "OnPreIdleUpkeep",  // slot 90 [high]
    "Unknown_91",  // slot 91
    "SecondaryInit",  // slot 92 [low]
    "Unknown_93",  // slot 93
    "OnDeath",  // slot 94 [medium]
    "OnDeathRattle",  // slot 95 [low]
    "Unknown_96",  // slot 96
    "OnRunAway",  // slot 97 [low]
    "Unknown_98",  // slot 98
    "OnKill",  // slot 99 [low]
    "Unknown_100",  // slot 100
    "OnCharacterDies",  // slot 101 [high]
    "Unknown_102",  // slot 102
    "Unknown_103",  // slot 103
    "Unknown_104",  // slot 104
    "Unknown_105",  // slot 105
    "Unknown_106",  // slot 106
    "OnSpawnCharacter",  // slot 107 [low]
    "Unknown_108",  // slot 108
    "Unknown_109",  // slot 109
    "Unknown_110",  // slot 110
    "OnRequestAction",  // slot 111 [low]
    "OnCharacterRequestAction",  // slot 112 [medium]
    "Unknown_113",  // slot 113
    "Unknown_114",  // slot 114
    "Unknown_115",  // slot 115
    "Unknown_116",  // slot 116
    "Unknown_117",  // slot 117
    "Unknown_118",  // slot 118
    "Unknown_119",  // slot 119
    "Unknown_120",  // slot 120
    "Unknown_121",  // slot 121
    "Unknown_122",  // slot 122
    "Unknown_123",  // slot 123
    "Unknown_124",  // slot 124
    "Unknown_125",  // slot 125
    "Unknown_126",  // slot 126
    "Unknown_127",  // slot 127
    "Unknown_128",  // slot 128
    "Unknown_129",  // slot 129
    "Unknown_130",  // slot 130
    "OnHitOtherWithProjectile",  // slot 131 [low]
    "Unknown_132",  // slot 132
    "Unknown_133",  // slot 133
    "Unknown_134",  // slot 134
    "Unknown_135",  // slot 135
    "Unknown_136",  // slot 136
    "Unknown_137",  // slot 137
    "Unknown_138",  // slot 138
    "Unknown_139",  // slot 139
    "Unknown_140",  // slot 140
    "Unknown_141",  // slot 141
    "Unknown_142",  // slot 142
    "Unknown_143",  // slot 143
    "Unknown_144",  // slot 144
    "CheckCounter_145",  // slot 145 [low] (dup of slot 150 'CheckCounter' [high])
    "Unknown_146",  // slot 146
    "CharacterInAuraEndTurn",  // slot 147 [high]
    "Unknown_148",  // slot 148
    "Unknown_149",  // slot 149
    "CheckCounter",  // slot 150 [high]
    "Trap",  // slot 151 [high]
    "Unknown_152",  // slot 152
    "Unknown_153",  // slot 153
    "CharacterInAuraOnCastAbility",  // slot 154 [high]
    "Unknown_155",  // slot 155
    "CharacterInAuraSpendMana",  // slot 156 [low]
    "Unknown_157",  // slot 157
    "Unknown_158",  // slot 158
    "Unknown_159",  // slot 159
    "Unknown_160",  // slot 160
    "Unknown_161",  // slot 161
    "Unknown_162",  // slot 162
    "Unknown_163",  // slot 163
    "Unknown_164",  // slot 164
    "Unknown_165",  // slot 165
    "CharacterInAuraOnDamageReport",  // slot 166 [low]
    "Unknown_167",  // slot 167
    "Unknown_168",  // slot 168
    "Unknown_169",  // slot 169
    "OnStackChange",  // slot 170 [low]
    "CanCollect",  // slot 171 [low]
};

// Resolve a slot name string (e.g. "OnReceivedDamage") to its index (0-171).
// Returns -1 if the name is not found. Case-sensitive exact match.
// "Unknown_N" slot names are not resolvable -use the numeric form instead.
#include <string.h>
static int CSF_ResolveSlotName(const char* name)
{
    int i;
    if (!name) return -1;
    // Skip Unknown_ slots -they're placeholders, not real names
    if (strncmp(name, "Unknown_", 8) == 0) return -1;
    for (i = 0; i < 172; i++)
    {
        if (strcmp(name, SLOT_NAMES[i]) == 0)
            return i;
    }
    return -1;
}
