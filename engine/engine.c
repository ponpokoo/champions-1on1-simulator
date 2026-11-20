// Pokémon Champions 1on1 battle engine (fast C core)
// Scope: 341 initial species (351 form rows), 511 selectable moves + Struggle,
//        217 abilities, 85 items + 81 Mega stones (Regulation M-C, Lv.50, 66 pts).
//        Legacy Metagross 17-move (MGO1/g12) and 68-move (MGO2/obsh3) paths remain
//        supported and frozen for regression compatibility.
// Build: gcc -O3 -march=native -o engine/engine engine/engine.c -lm
// Usage:
//   ./engine/engine battle "<specA>" "<specB>" <n> <seed>
//   ./engine/engine matrix [n] [seed]
//   ./engine/engine screen <cand_file> <pool_file> <games> <seed>
//   ./engine/engine screenx <cand_file> <pool_file> <games> <seed>
//   ./engine/engine trace "<specA>" "<specB>" <seed>
//   ./engine/engine policy-version
// Spec v3 (all species):
//   <species>:<H,A,B,C,D,S>|<mv1,mv2,mv3,mv4>|<policy>|<item>|<mega>|<ability>|<up,down>|<sex>
//   points 0..32 totaling 66; mega 0/1/2; nature up,down in 1..5 or 0,0; sex m/f/n.
// Legacy spec (Metagross):
//   "HP,A,B,C,D,S|MV,MV,MV,MV|policy|item|mega[|ability[|up,down]]"
// Policies:
//   mirror noise fast stall generic obsdemo obsh2 learned obsdemo2 obsh3 learned2 obsdemo3 obsh4
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <time.h>

// Turn cap: the real game has a 20-minute timer, so games never reach 100 turns.
#define MAX_TURN 100

// ---------- RNG (splitmix64) ----------
typedef struct { uint64_t s; } Rng;
static uint64_t rng_next(Rng *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static double rng_dbl(Rng *r) { return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0); }
static int rng_int(Rng *r, int n) { return (int)(rng_next(r) % (uint64_t)n); }

// ---------- Types ----------
enum { T_NORMAL, T_FIRE, T_WATER, T_ELEC, T_GRASS, T_ICE, T_FIGHT, T_POISON,
       T_GROUND, T_FLY, T_PSY, T_BUG, T_ROCK, T_GHOST, T_DRAGON, T_DARK, T_STEEL, T_FAIRY };
// ---------- Stage 8b-1: generated data foundation ----------
#include "stage8/species_data.h"
static double eff_vs(int atk, int def) {
    return def<0 ? 1.0 : S8_TYPE_CHART[atk][def];
}

// ---------- Moves ----------
enum { MV_BP, MV_BD, MV_EQ, MV_SCL, MV_SB, MV_MS, MV_AGI, MV_CP, MV_ID, MV_PRO,
       MV_PU, MV_REST, MV_SF, MV_ST, MV_SUB, MV_TRK, MV_PN,
       MV_AA, MV_SP, MV_PC, MV_SE, MV_BUL, MV_RND, MV_BSW, MV_PSH,
       MV_RS, MV_IH, MV_ZH, MV_SN, MV_RT, MV_IW, MV_PSY, MV_FC, MV_MM, MV_TB, MV_HA,
       MV_IP, MV_TP, MV_BSL, MV_SLB, MV_SWG, MV_FAC,
       MV_SUN, MV_RAIN, MV_SAND, MV_SR, MV_REF, MV_LS, MV_BB, MV_PF, MV_GRAV, MV_MR, MV_END,
       MV_FP, MV_TAN, MV_SD, MV_DE, MV_EXP, MV_ROLL, MV_GI, MV_HB, MV_GYRO, MV_HP, MV_GK, MV_BEAM, MV_FS, MV_MB, MV_EF, MV_COUNT };
// Vocabulary sizes are protocol constants, never internal action sentinels.
enum { V1_MOVE_COUNT=17, V2_MOVE_COUNT=68, V1_STRUGGLE_WIRE=17, V2_STRUGGLE_WIRE=68 };
typedef enum { ACTION_MOVE, ACTION_STRUGGLE } ActionKind;
typedef struct { ActionKind kind; int move; } MoveAction;
static int action_wire_id(MoveAction action, int version) {
    int count=version==1?V1_MOVE_COUNT:version==2?V2_MOVE_COUNT:version==3?512:0;
    if(!count) return -1;
    if(action.kind==ACTION_STRUGGLE) return version==1?V1_STRUGGLE_WIRE:version==2?V2_STRUGGLE_WIRE:512;
    return action.kind==ACTION_MOVE && action.move>=0 && action.move<count ? action.move : -1;
}
// Bit 0 = no lock; bit move+1 = that move. Canonical transport is two LE64
// words, low word first; bits 69..127 must be zero. No native struct transport.
typedef struct { uint64_t word[2]; } MoveHypotheses;
static MoveHypotheses lock_single(int move) {
    MoveHypotheses s={{0,0}};
    if(move < -1 || move >= V2_MOVE_COUNT) abort();
    unsigned bit=(unsigned)(move+1); s.word[bit/64]=UINT64_C(1)<<(bit%64); return s;
}
static inline int lock_has(MoveHypotheses s, int move) {
    if(move < -1 || move >= V2_MOVE_COUNT) return 0;
    unsigned bit=(unsigned)(move+1); return (int)((s.word[bit/64]>>(bit%64))&1);
}
static MoveHypotheses lock_add(MoveHypotheses s, int move) {
    MoveHypotheses bit=lock_single(move);
    s.word[0]|=bit.word[0]; s.word[1]|=bit.word[1]; return s;
}
static int lock_v1(MoveHypotheses s) { return !s.word[1] && !(s.word[0]>>18); }
static inline void lock_serialize(MoveHypotheses s, unsigned char bytes[16]) {
    if(s.word[1]>>5) abort();
    for(int w=0;w<2;w++) for(int b=0;b<8;b++) bytes[8*w+b]=(unsigned char)(s.word[w]>>(8*b));
}
enum { CAT_PHYS, CAT_SPEC, CAT_STATUS };
typedef struct { const char *code; int type; int cat; int power; int acc; int prio; int contact; int pp; } MoveDef;
static const MoveDef MOVES[MV_COUNT] = {
    {"BP",   T_FIGHT, CAT_PHYS,   80, 100, 0, 1, 12},  // ボディプレス (uses own B)
    {"BD",   T_GROUND,CAT_PHYS,   60, 100, 0, 0, 20},  // じならし (foe S-1)
    {"EQ",   T_GROUND,CAT_PHYS,  100, 100, 0, 0, 12},  // じしん
    {"SCL",  T_GHOST, CAT_PHYS,   70, 100, 0, 1, 16},  // シャドークロー (crit 1/8)
    {"SB",   T_GHOST, CAT_SPEC,   80, 100, 0, 0, 16},  // シャドーボール (20% foe D-1)
    {"MS",   T_GROUND,CAT_SPEC,   20, 100, 0, 0, 12},  // どろかけ (foe acc-1)
    {"AGI",  T_PSY,   CAT_STATUS,  0, 101, 0, 0, 20},  // こうそくいどう (own S+2)
    {"CP",   T_PSY,   CAT_STATUS,  0, 101, 0, 0, 20},  // コスモパワー (own B/D+1)
    {"ID",   T_STEEL, CAT_STATUS,  0, 101, 0, 0, 16},  // てっぺき (own B+2)
    {"PRO",  T_NORMAL,CAT_STATUS,  0, 101, 4, 0,  8},  // まもる (priority +4)
    {"PU",   T_NORMAL,CAT_STATUS,  0, 101, 0, 0, 12},  // じこあんじ (copy foe stages)
    {"REST", T_PSY,   CAT_STATUS,  0, 101, 0, 0,  8},  // ねむる
    {"SF",   T_NORMAL,CAT_STATUS,  0, 100, 0, 0, 12},  // こわいかお (foe S-2)
    {"ST",   T_NORMAL,CAT_STATUS,  0, 101, 0, 0, 12},  // ねごと
    {"SUB",  T_NORMAL,CAT_STATUS,  0, 101, 0, 0, 12},  // みがわり
    {"TRK",  T_PSY,   CAT_STATUS,  0, 100, 0, 0, 12},  // トリック (swap items)
    {"PN",   T_PSY,   CAT_SPEC,   75, 100, 0, 0, 12},  // サイコノイズ (heal block)
    {"AA", T_FLY, CAT_PHYS, 60, 101, 0, 1, 20}, // Aerial Ace
    {"SP", T_GHOST, CAT_PHYS, 60, 101, 0, 1, 20}, // Shadow Punch
    {"PC", T_PSY, CAT_PHYS, 70, 100, 0, 0, 20}, // Psycho Cut
    {"SE", T_ROCK, CAT_PHYS, 100, 80, 0, 0, 8}, // Stone Edge
    {"BUL", T_STEEL, CAT_PHYS, 40, 100, 1, 1, 20}, // Bullet Punch
    {"RND", T_NORMAL, CAT_SPEC, 60, 100, 0, 0, 16}, // Round
    {"BSW", T_DARK, CAT_PHYS, 60, 100, 0, 1, 20}, // Brutal Swing
    {"PSH", T_PSY, CAT_SPEC, 80, 100, 0, 0, 12}, // Psyshock
    {"RS", T_ROCK, CAT_PHYS, 75, 90, 0, 0, 12}, // Rock Slide
    {"IH", T_STEEL, CAT_PHYS, 80, 100, 0, 1, 16}, // Iron Head
    {"ZH", T_PSY, CAT_PHYS, 80, 90, 0, 1, 16}, // Zen Headbutt
    {"SN", T_NORMAL, CAT_SPEC, 50, 100, 0, 0, 16}, // Snore
    {"RT", T_ROCK, CAT_PHYS, 60, 95, 0, 0, 16}, // Rock Tomb
    {"IW", T_ICE, CAT_SPEC, 55, 95, 0, 0, 16}, // Icy Wind
    {"PSY", T_PSY, CAT_SPEC, 90, 100, 0, 0, 12}, // Psychic
    {"FC", T_STEEL, CAT_SPEC, 80, 100, 0, 0, 12}, // Flash Cannon
    {"MM", T_STEEL, CAT_PHYS, 90, 90, 0, 1, 12}, // Meteor Mash
    {"TB", T_GRASS, CAT_PHYS, 50, 100, 0, 1, 20}, // Trailblaze
    {"HA", T_FIGHT, CAT_PHYS, 100, 90, 0, 1, 12}, // Hammer Arm
    {"IP", T_ICE, CAT_PHYS, 75, 100, 0, 1, 16}, // Ice Punch
    {"TP", T_ELEC, CAT_PHYS, 75, 100, 0, 1, 16}, // Thunder Punch
    {"BSL", T_NORMAL, CAT_PHYS, 85, 100, 0, 1, 16}, // Body Slam
    {"SLB", T_POISON, CAT_SPEC, 90, 100, 0, 0, 12}, // Sludge Bomb
    {"SWG", T_NORMAL, CAT_STATUS, 0, 85, 0, 0, 16}, // Swagger
    {"FAC", T_NORMAL, CAT_PHYS, 70, 100, 0, 1, 20}, // Facade
    {"SUN", T_FIRE, CAT_STATUS, 0, 101, 0, 0, 8}, // Sunny Day
    {"RAIN", T_WATER, CAT_STATUS, 0, 101, 0, 0, 8}, // Rain Dance
    {"SAND", T_ROCK, CAT_STATUS, 0, 101, 0, 0, 8}, // Sandstorm
    {"SR", T_ROCK, CAT_STATUS, 0, 101, 0, 0, 20}, // Stealth Rock
    {"REF", T_PSY, CAT_STATUS, 0, 101, 0, 0, 20}, // Reflect
    {"LS", T_PSY, CAT_STATUS, 0, 101, 0, 0, 20}, // Light Screen
    {"BB", T_FIGHT, CAT_PHYS, 75, 100, 0, 1, 16}, // Brick Break
    {"PF", T_PSY, CAT_PHYS, 85, 100, 0, 1, 12}, // Psychic Fangs
    {"GRAV", T_PSY, CAT_STATUS, 0, 101, 0, 0, 8}, // Gravity
    {"MR", T_ELEC, CAT_STATUS, 0, 101, 0, 0, 12}, // Magnet Rise
    {"END", T_NORMAL, CAT_STATUS, 0, 101, 4, 0, 12}, // Endure
    {"FP", T_FIGHT, CAT_PHYS, 150, 100, -3, 1, 20}, // Focus Punch
    {"TAN", T_GROUND, CAT_PHYS, 75, 100, 0, 1, 12}, // Stomping Tantrum
    {"SD", T_NORMAL, CAT_PHYS, 200, 100, 0, 0, 8}, // Self-Destruct
    {"DE", T_NORMAL, CAT_PHYS, 120, 100, 0, 1, 16}, // Double-Edge
    {"EXP", T_NORMAL, CAT_PHYS, 250, 100, 0, 0, 8}, // Explosion
    {"ROLL", T_STEEL, CAT_PHYS, 130, 100, 0, 1, 8}, // Steel Roller
    {"GI", T_NORMAL, CAT_PHYS, 150, 90, 0, 1, 8}, // Giga Impact
    {"HB", T_NORMAL, CAT_SPEC, 150, 90, 0, 0, 8}, // Hyper Beam
    {"GYRO", T_STEEL, CAT_PHYS, 0, 100, 0, 1, 8}, // Gyro Ball
    {"HP", T_STEEL, CAT_PHYS, 0, 100, 0, 1, 12}, // Hard Press
    {"GK", T_GRASS, CAT_SPEC, 0, 100, 0, 1, 20}, // Grass Knot
    {"BEAM", T_STEEL, CAT_SPEC, 140, 95, 0, 0, 8}, // Steel Beam
    {"FS", T_PSY, CAT_SPEC, 120, 100, 0, 0, 12}, // Future Sight
    {"MB", T_ROCK, CAT_SPEC, 120, 90, 0, 0, 12}, // Meteor Beam
    {"EF", T_PSY, CAT_SPEC, 80, 100, 0, 0, 12}, // Expanding Force
};
// Separate shared traits keep every original MoveDef row byte-identical.
typedef struct { int crit_bonus, sound, defensive_stat; } MoveTraits;
static const MoveTraits MOVE_TRAITS[MV_COUNT] = {
    [MV_SCL]={1,0,0}, [MV_PC]={1,0,0}, [MV_SE]={1,0,0},
    [MV_RND]={0,1,0}, [MV_SN]={0,1,0}, [MV_PSH]={0,0,2},
};
enum { STATUS_NONE, STATUS_PAR, STATUS_FRZ, STATUS_PSN, STATUS_TOX, STATUS_BRN };
enum { WEATHER_NONE, WEATHER_SUN, WEATHER_RAIN, WEATHER_SAND, WEATHER_SNOW, WEATHER_HAIL };
enum { TERRAIN_NONE, TERRAIN_PSYCHIC, TERRAIN_OTHER, TERRAIN_ELECTRIC, TERRAIN_MISTY, TERRAIN_GRASSY };
enum { EFFECT_NONE, EFFECT_STAGE, EFFECT_FLINCH, EFFECT_STATUS };
typedef struct { int kind, chance, self, stage, delta; } MoveEffect;
static const MoveEffect MOVE_EFFECTS[MV_COUNT] = {
    [MV_SB]={EFFECT_STAGE,20,0,3,-1},
    [MV_BD]={EFFECT_STAGE,100,0,4,-1},
    [MV_MS]={EFFECT_STAGE,100,0,5,-1},
    [MV_RS]={EFFECT_FLINCH,30,0,0,0},
    [MV_IH]={EFFECT_FLINCH,20,0,0,0}, // Champions override
    [MV_ZH]={EFFECT_FLINCH,20,0,0,0},
    [MV_SN]={EFFECT_FLINCH,30,0,0,0},
    [MV_RT]={EFFECT_STAGE,100,0,4,-1},
    [MV_IW]={EFFECT_STAGE,100,0,4,-1},
    [MV_PSY]={EFFECT_STAGE,10,0,3,-1},
    [MV_FC]={EFFECT_STAGE,10,0,3,-1},
    [MV_MM]={EFFECT_STAGE,20,1,0,1},
    [MV_TB]={EFFECT_STAGE,100,1,4,1},
    [MV_HA]={EFFECT_STAGE,100,1,4,-1},
    [MV_IP]={EFFECT_STATUS,10,0,STATUS_FRZ,0},
    [MV_TP]={EFFECT_STATUS,10,0,STATUS_PAR,0},
    [MV_BSL]={EFFECT_STATUS,30,0,STATUS_PAR,0},
    [MV_SLB]={EFFECT_STATUS,30,0,STATUS_PSN,0},
};
// ---------- Stage 8c-1: reviewed basic attacks, generated execution metadata ----------
// Only these audited table IDs are executable beyond the frozen 68 rows.
// Additional critical/recoil attacks are reviewed separately in Stage 8c-2.
static const int S8_BASIC_IDS[] = {
    68, // accelerock
    82, // aquajet
    85, // aquatail
    90, // aurasphere
    112, // boomburst
    153, // dazzlinggleam
    168, // dragonclaw
    171, // dragonpulse
    176, // drillpeck
    195, // extremespeed
    254, // highhorsepower
    261, // hydropump
    262, // hypervoice
    267, // iceshard
    279, // jetpunch
    282, // kowtowcleave
    299, // machpunch
    306, // megakick
    307, // megahorn
    334, // overdrive
    341, // petalblizzard
    354, // pound
    355, // powergem
    360, // powerwhip
    365, // quickattack
    394, // seedbomb
    396, // shadowsneak
    413, // smartstrike
    446, // surf
    488, // vacuumwave
    508, // xscissor
};
static int s8_basic_supported(int move) {
    for(size_t i=0;i<sizeof(S8_BASIC_IDS)/sizeof(*S8_BASIC_IDS);i++)
        if(move==S8_BASIC_IDS[i]) return 1;
    return 0;
}
// ---------- Stage 8c-2: critical hits, damage recoil and crash ----------
static const int S8_CRIT_RECOIL_IDS[] = {
    74, // aircutter
    81, // aquacutter
    145, // crabhammer
    146, // crosschop
    177, // drillrun
    219, // flowertrick
    229, // frostbreath
    287, // leafblade
    328, // nightslash
    409, // slash
    416, // snipeshot
    435, // stormthrow
    114, // bravebird
    244, // headsmash
    255, // highjumpkick
    292, // lightofruin
    444, // supercellslam
    496, // wavecrash
    501, // wildcharge
    505, // woodhammer
};
static int s8_crit_recoil_supported(int move) {
    for(size_t i=0;i<sizeof(S8_CRIT_RECOIL_IDS)/sizeof(*S8_CRIT_RECOIL_IDS);i++)
        if(move==S8_CRIT_RECOIL_IDS[i]) return 1;
    return 0;
}
// ---------- Stage 8c-3: purely stat-changing attack effects ----------
#include "stage8/secondary_stats_8c3.h"
static const S8StatEffect *s8_stat_effect(int move) {
    for(size_t i=0;i<sizeof(S8_STAT_EFFECTS)/sizeof(*S8_STAT_EFFECTS);i++)
        if(move==S8_STAT_EFFECTS[i].move) return &S8_STAT_EFFECTS[i];
    return NULL;
}
// ---------- Stage 8c-4: complete status/flinch/confusion attack effects ----------
#include "stage8/secondary_status_8c4.h"
static const S8StatusMove *s8_status_move(int move) {
    for(size_t i=0;i<sizeof(S8_STATUS_MOVES)/sizeof(*S8_STATUS_MOVES);i++)
        if(move==S8_STATUS_MOVES[i].move) return &S8_STATUS_MOVES[i];
    return NULL;
}
static int s8_turn_native_flinch(int move);
static int s8_native_flinch(int move) {
    if(s8_turn_native_flinch(move)) return 1;
    const S8StatusMove *m=s8_status_move(move);
    if(m) for(int i=0;i<m->count;i++) if(m->effects[i].kind==S8_EXTRA_FLINCH) return 1;
    return 0;
}
// ---------- Stage 8c-5: reviewed multihit attacks ----------
#include "stage8/multihit_8c5.h"
// ---------- Stage 8c-6: special base power, fixed damage and self KO ----------
#include "stage8/variable_damage_8c6.h"
// ---------- Stage 8c-7: status boosts and stored-stat operations ----------
#include "stage8/boost_stats_8c7.h"
// ---------- Stage 8c-8a: primary status and confusion moves ----------
#include "stage8/status_moves_8c8a.h"
// ---------- Stage 8c-8b: complete drain and simple healing moves ----------
#include "stage8/drain_heal_8c8b.h"
// ---------- Stage 8c-8c: complete stateful and delayed healing ----------
#include "stage8/stateful_heal_8c8c.h"
// ---------- Stage 8c-9a: complete field, terrain and side conditions ----------
#include "stage8/field_8c9a.h"
// ---------- Stage 8c-9b: complete trapping and hazards ----------
#include "stage8/trap_hazard_8c9b.h"
// ---------- Stage 8c-9c: complete control volatiles ----------
#include "stage8/control_8c9c.h"
// ---------- Stage 8c-9d: complete rampage and sound control ----------
#include "stage8/rampage_8c9d.h"
// ---------- Stage 8c-10a: complete protection and recharge ----------
#include "stage8/protect_8c10a.h"
// ---------- Stage 8c-10b: two-turn, priority and called actions ----------
#include "stage8/two_turn_8c10b.h"
// ---------- Stage 8c-11a: current types and ability operations ----------
#include "stage8/type_ability_8c11a.h"
// ---------- Stage 8c-11b: item operations and no-reserve switching ----------
#define S8_ITEM_OPERATIONS S8_LEGACY_ITEM_OPERATIONS
#include "stage8/item_switch_8c11b.h"
#undef S8_ITEM_OPERATIONS
// ---------- Stage 8c-11c: remaining complete SPECIAL moves ----------
static int S8_SPECIAL_ACTIVE; // legacy-68 Steel Roller stays frozen outside this unit
#include "stage8/special_rest_8c11c.h"
static int s8_variable_supported(int move) {
    for(size_t i=0;i<sizeof(S8_VARIABLE_IDS)/sizeof(*S8_VARIABLE_IDS);i++)
        if(move==S8_VARIABLE_IDS[i]) return 1;
    return 0;
}
// Stage 8i-8: Rapid Spin uses the common single-hit and secondary lifecycle.
static int s8_rapidspin_move(int move) {
    return move>=0 && move<S8_MOVES_COUNT && !strcmp(S8_MOVES[move].id,"rapidspin");
}
static const S8Multihit *s8_multihit(int move) {
    // Single-hit new attacks share the exact hit lifecycle, without changing
    // any of the legacy-68 damage or random draw paths.
    if((s8_variable_supported(move) && move!=S8V_MEMENTO) ||
       (s8_boost_move(move) && S8_MOVES[move].category!=CAT_STATUS) ||
       (s8_heal_move(move) && s8_heal_move(move)->kind==S8H_DRAIN) ||
       (s8_trap_move(move) && S8_MOVES[move].category!=CAT_STATUS) ||
       (s8_control_move(move) && S8_MOVES[move].category!=CAT_STATUS) || s8_rampage_move(move) ||
       (s8_protect_move(move) && S8_MOVES[move].category!=CAT_STATUS) ||
       (s8_turn_move(move) && S8_MOVES[move].category!=CAT_STATUS) ||
       (s8_type_ability_move(move) && S8_MOVES[move].category!=CAT_STATUS) ||
       (s8_item_switch_move(move) && S8_MOVES[move].category!=CAT_STATUS) ||
       (s8_special_move(move) && S8_MOVES[move].category!=CAT_STATUS) || s8_rapidspin_move(move)) {
        static S8Multihit plans[S8_MOVES_COUNT];
        int lo=move==S8V_WATERSHURIKEN?2:move==S8V_TRIPLEAXEL?3:1;
        int hi=move==S8V_WATERSHURIKEN?5:lo;
        plans[move]=(S8Multihit){move,lo,hi,move==S8V_TRIPLEAXEL,{0}};
        return &plans[move];
    }
    for(size_t i=0;i<sizeof(S8_MULTIHITS)/sizeof(*S8_MULTIHITS);i++)
        if(move==S8_MULTIHITS[i].move) return &S8_MULTIHITS[i];
    return NULL;
}
static int s8_transform_move(int move) {
    return move>=0 && move<S8_MOVES_COUNT && !strcmp(S8_MOVES[move].id,"transform");
}
static int move_executable(int move) {
    return (move>=0 && move<MV_COUNT) || s8_basic_supported(move) || s8_crit_recoil_supported(move) || s8_stat_effect(move) || s8_status_move(move) || s8_multihit(move) || s8_variable_supported(move) || s8_boost_move(move) || s8_primary_status(move) || s8_heal_move(move) || s8_stateful_heal(move) || s8_field_move(move) || s8_trap_move(move) || s8_control_move(move) || s8_protect_move(move) || s8_turn_move(move) || s8_type_ability_move(move) || s8_item_switch_move(move) || s8_special_move(move) || s8_transform_move(move);
}
// Scoped active-move copy: Electrify never mutates the shared definitions.
static int S8_CONTROL_OVERRIDE_ID=-1;
static MoveDef S8_CONTROL_OVERRIDE;
static const MoveDef *move_def(int move) {
    if(S8_CONTROL_OVERRIDE_ID>=0 && move==S8_CONTROL_OVERRIDE_ID) return &S8_CONTROL_OVERRIDE;
    if(move>=0 && move<MV_COUNT) return &MOVES[move];
    if(!s8_basic_supported(move) && !s8_crit_recoil_supported(move) && !s8_stat_effect(move) && !s8_status_move(move) && !s8_multihit(move) && !s8_variable_supported(move) && !s8_boost_move(move) && !s8_primary_status(move) && !s8_heal_move(move) && !s8_stateful_heal(move) && !s8_field_move(move) && !s8_trap_move(move) && !s8_control_move(move) && !s8_protect_move(move) && !s8_turn_move(move) && !s8_type_ability_move(move) && !s8_item_switch_move(move) && !s8_special_move(move) && !s8_transform_move(move)) {
        fprintf(stderr,"move execution rejected: unimplemented table ID %d\n",move); exit(2);
    }
    // The engine is single-threaded. Cache adapters, never duplicate mechanics
    // data: every field comes from the unchanged generated S8_MOVES table.
    static MoveDef definitions[S8_MOVES_COUNT];
    if(!definitions[move].code) {
        const S8Move *row=&S8_MOVES[move];
        definitions[move]=(MoveDef){row->code,row->type,row->category,row->power,
            row->accuracy,row->priority,row->contact,row->pp};
    }
    return &definitions[move];
}
static const MoveTraits *move_traits(int move) {
    if(move>=0 && move<MV_COUNT) return &MOVE_TRAITS[move];
    (void)move_def(move); // fail closed, including direct fixture callers
    static MoveTraits traits[S8_MOVES_COUNT];
    traits[move]=(MoveTraits){S8_MOVES[move].crit_ratio-1,
        !!(S8_MOVES[move].flags&S8_FLAG_BYPASSSUB),0};
    return &traits[move];
}

static int move_from_code(const char *s) {
    const S8Species *species=&S8_SPECIES[S8_METAGROSS];
    for (int i = 0; i < MV_COUNT; i++) if (strcmp(s, MOVES[i].code) == 0) {
        for(int j=0;j<species->move_count;j++)
            if(S8_LEARNSETS[species->move_offset+j]==i) return i;
        return -1;
    }
    return -1;
}

// ---------- Battler ----------
enum { POL_MIRROR, POL_NOISE, POL_FAST, POL_STALL, POL_GENERIC, POL_OBSDEMO, POL_OBSH2, POL_LEARNED, POL_OBSDEMO2, POL_OBSH3, POL_LEARNED2, POL_OBSDEMO3, POL_OBSH4 };
static int is_v2_policy(int policy) { return policy==POL_OBSDEMO2 || policy==POL_OBSH3 || policy==POL_LEARNED2; }
static int is_v3_policy(int policy) { return policy==POL_OBSDEMO3 || policy==POL_OBSH4; }
static int is_observable_policy(int policy) { return is_v3_policy(policy) || policy==POL_OBSDEMO || policy==POL_OBSH2 || policy==POL_LEARNED || is_v2_policy(policy); }

enum { AB_NONE, AB_CB, AB_LM };   // abilities: none / Clear Body / Light Metal (Mega = Tough Claws via the mega flag)
enum { ITEM_NONE, ITEM_HELMET, ITEM_STONE,
       ITEM_LEFTOVERS, ITEM_SITRUS, ITEM_SCARF, ITEM_LIFEORB, ITEM_MUSCLE,
       ITEM_WISE, ITEM_BELT, ITEM_TAG, ITEM_SAND, ITEM_SPOON, ITEM_BBELT,
       ITEM_METRO, ITEM_SHELLBELL, ITEM_LENS, ITEM_KINGS, ITEM_WIDE,
       ITEM_ZOOM, ITEM_POWDER, ITEM_CLAW, ITEM_IRONBALL, ITEM_SHUCA,
       ITEM_CHOPLE, ITEM_KASIB, ITEM_PAYAPA, ITEM_CHESTO, ITEM_LUM,
       ITEM_LEPPA, ITEM_ORAN, ITEM_SASH, ITEM_FBAND, ITEM_BALLOON, ITEM_HERB,
       ITEM_BLACKGLASSES, ITEM_CHARCOAL, ITEM_DRAGONFANG, ITEM_FAIRYFEATHER, ITEM_HARDSTONE,
       ITEM_MAGNET, ITEM_METALCOAT, ITEM_MIRACLESEED, ITEM_MYSTICWATER, ITEM_NEVERMELTICE,
       ITEM_POISONBARB, ITEM_SHARPBEAK, ITEM_SILKSCARF, ITEM_SILVERPOWDER,
       ITEM_BABIRI, ITEM_CHARTI, ITEM_CHILAN, ITEM_COBA, ITEM_COLBUR, ITEM_HABAN, ITEM_KEBIA,
       ITEM_OCCA, ITEM_PASSHO, ITEM_RINDO, ITEM_ROSELI, ITEM_TANGA, ITEM_WACAN, ITEM_YACHE,
       ITEM_ASPEAR, ITEM_CHERI, ITEM_PECHA, ITEM_PERSIM, ITEM_RAWST, ITEM_MENTALHERB,
       ITEM_DAMPROCK, ITEM_HEATROCK, ITEM_ICYROCK, ITEM_SMOOTHROCK, ITEM_LIGHTCLAY, ITEM_TERRAINEXTENDER,
       ITEM_ELECTRICSEED, ITEM_GRASSYSEED, ITEM_MISTYSEED, ITEM_PSYCHICSEED,
       ITEM_BIGROOT, ITEM_BINDINGBAND, ITEM_LEEK, ITEM_LIGHTBALL, ITEM_NORMALGEM,
       ITEM_EJECTBUTTON, ITEM_REDCARD, ITEM_SHEDSHELL, ITEM_COUNT };
// Stage 8i-1: one BasePower-priority-15 type-item table for all 18 types.
// Keep legacy numeric IDs and the generated 8c-11b table unchanged.
typedef struct {
    int item, type;
    const char *key;
    S8ItemOperation operation;
} S8TypeBoostItem;
static const S8TypeBoostItem S8_TYPE_BOOST_ITEMS[] = {
    {ITEM_BBELT,T_FIGHT,"blackbelt",{5,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_SAND,T_GROUND,"softsand",{74,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_TAG,T_GHOST,"spelltag",{75,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_SPOON,T_PSY,"twistedspoon",{78,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_BLACKGLASSES,T_DARK,"blackglasses",{6,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_CHARCOAL,T_FIRE,"charcoal",{8,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_DRAGONFANG,T_DRAGON,"dragonfang",{18,0,70,S8IE_NONE,S8IE_NONE}},
    {ITEM_FAIRYFEATHER,T_FAIRY,"fairyfeather",{22,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_HARDSTONE,T_ROCK,"hardstone",{27,0,100,S8IE_NONE,S8IE_NONE}},
    {ITEM_MAGNET,T_ELEC,"magnet",{41,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_METALCOAT,T_STEEL,"metalcoat",{43,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_MIRACLESEED,T_GRASS,"miracleseed",{45,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_MYSTICWATER,T_WATER,"mysticwater",{48,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_NEVERMELTICE,T_ICE,"nevermeltice",{49,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_POISONBARB,T_POISON,"poisonbarb",{57,0,70,S8IE_NONE,S8IE_NONE}},
    {ITEM_SHARPBEAK,T_FLY,"sharpbeak",{66,0,50,S8IE_NONE,S8IE_NONE}},
    {ITEM_SILKSCARF,T_NORMAL,"silkscarf",{70,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_SILVERPOWDER,T_BUG,"silverpowder",{71,0,10,S8IE_NONE,S8IE_NONE}},
};
static int s8_type_boost_matches(int item,int type) {
    for(size_t i=0;i<sizeof(S8_TYPE_BOOST_ITEMS)/sizeof(*S8_TYPE_BOOST_ITEMS);i++)
        if(S8_TYPE_BOOST_ITEMS[i].item==item) return S8_TYPE_BOOST_ITEMS[i].type==type;
    return 0;
}
static int s8_type_boost_parse(const char *key) {
    for(size_t i=0;i<sizeof(S8_TYPE_BOOST_ITEMS)/sizeof(*S8_TYPE_BOOST_ITEMS);i++)
        if(S8_TYPE_BOOST_ITEMS[i].item>ITEM_HERB && !strcmp(key,S8_TYPE_BOOST_ITEMS[i].key))
            return S8_TYPE_BOOST_ITEMS[i].item;
    return ITEM_NONE;
}
// Stage 8i-2: onSourceModifyDamage resist berries; empty onEat, no Fling.
// The operation prefix and the four legacy resist berries remain frozen.
typedef struct {
    int item, type;
    const char *key;
    int super_effective;
    S8ItemOperation operation;
} S8ResistBerry;
static const S8ResistBerry S8_RESIST_BERRIES[] = {
    {ITEM_BABIRI,T_STEEL,"babiriberry",1,{2,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_CHARTI,T_ROCK,"chartiberry",1,{9,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_CHILAN,T_NORMAL,"chilanberry",0,{12,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_COBA,T_FLY,"cobaberry",1,{15,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_COLBUR,T_DARK,"colburberry",1,{16,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_HABAN,T_DRAGON,"habanberry",1,{26,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_KEBIA,T_POISON,"kebiaberry",1,{32,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_OCCA,T_FIRE,"occaberry",1,{51,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_PASSHO,T_WATER,"passhoberry",1,{53,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_RINDO,T_GRASS,"rindoberry",1,{62,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_ROSELI,T_FAIRY,"roseliberry",1,{64,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_TANGA,T_BUG,"tangaberry",1,{76,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_WACAN,T_ELEC,"wacanberry",1,{79,1,0,S8IE_NONE,S8IE_NONE}},
    {ITEM_YACHE,T_ICE,"yacheberry",1,{83,1,0,S8IE_NONE,S8IE_NONE}},
};
static int s8_resist_berry_parse(const char *key) {
    for(size_t i=0;i<sizeof(S8_RESIST_BERRIES)/sizeof(*S8_RESIST_BERRIES);i++)
        if(!strcmp(key,S8_RESIST_BERRIES[i].key)) return S8_RESIST_BERRIES[i].item;
    return ITEM_NONE;
}
// Stage 8i-3: selective onUpdate/onEat cures, plus Mental Herb's useItem.
enum { S8IE_SELECTIVE_STATUS=S8IE_FLINCH+1, S8IE_MENTAL };
typedef struct {
    int item;
    const char *key;
    unsigned status_mask;
    int confusion;
    S8ItemOperation operation;
} S8StatusItem;
static const S8StatusItem S8_STATUS_ITEMS[] = {
    {ITEM_ASPEAR,"aspearberry",1u<<STATUS_FRZ,0,{1,1,0,S8IE_SELECTIVE_STATUS,S8IE_NONE}},
    {ITEM_CHERI,"cheriberry",1u<<STATUS_PAR,0,{10,1,0,S8IE_SELECTIVE_STATUS,S8IE_NONE}},
    {ITEM_PECHA,"pechaberry",(1u<<STATUS_PSN)|(1u<<STATUS_TOX),0,{55,1,0,S8IE_SELECTIVE_STATUS,S8IE_NONE}},
    {ITEM_PERSIM,"persimberry",0,1,{56,1,0,S8IE_SELECTIVE_STATUS,S8IE_NONE}},
    {ITEM_RAWST,"rawstberry",1u<<STATUS_BRN,0,{60,1,0,S8IE_SELECTIVE_STATUS,S8IE_NONE}},
    {ITEM_MENTALHERB,"mentalherb",0,0,{42,0,10,S8IE_NONE,S8IE_MENTAL}},
};
static const S8StatusItem *s8_status_item(int item) {
    for(size_t i=0;i<sizeof(S8_STATUS_ITEMS)/sizeof(*S8_STATUS_ITEMS);i++)
        if(S8_STATUS_ITEMS[i].item==item) return &S8_STATUS_ITEMS[i];
    return NULL;
}
static int s8_status_item_parse(const char *key) {
    for(size_t i=0;i<sizeof(S8_STATUS_ITEMS)/sizeof(*S8_STATUS_ITEMS);i++)
        if(!strcmp(key,S8_STATUS_ITEMS[i].key)) return S8_STATUS_ITEMS[i].item;
    return ITEM_NONE;
}
// Stage 8i-4: duration callbacks inspect the setter's effective held item.
enum { S8D_WEATHER, S8D_SCREEN, S8D_TERRAIN };
typedef struct {
    int item, kind, weather;
    const char *key;
    S8ItemOperation operation;
} S8DurationItem;
static const S8DurationItem S8_DURATION_ITEMS[] = {
    {ITEM_DAMPROCK,S8D_WEATHER,WEATHER_RAIN,"damprock",{17,0,60,S8IE_NONE,S8IE_NONE}},
    {ITEM_HEATROCK,S8D_WEATHER,WEATHER_SUN,"heatrock",{28,0,60,S8IE_NONE,S8IE_NONE}},
    {ITEM_ICYROCK,S8D_WEATHER,WEATHER_SNOW,"icyrock",{29,0,40,S8IE_NONE,S8IE_NONE}},
    {ITEM_SMOOTHROCK,S8D_WEATHER,WEATHER_SAND,"smoothrock",{73,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_LIGHTCLAY,S8D_SCREEN,0,"lightclay",{39,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_TERRAINEXTENDER,S8D_TERRAIN,0,"terrainextender",{77,0,60,S8IE_NONE,S8IE_NONE}},
};
static int s8_duration_item_parse(const char *key) {
    for(size_t i=0;i<sizeof(S8_DURATION_ITEMS)/sizeof(*S8_DURATION_ITEMS);i++)
        if(!strcmp(key,S8_DURATION_ITEMS[i].key)) return S8_DURATION_ITEMS[i].item;
    return ITEM_NONE;
}
// Stage 8i-5: seeds use terrain identity, with no groundedness predicate.
typedef struct {
    int item, terrain, stat;
    const char *key;
    S8ItemOperation operation;
} S8SeedItem;
static const S8SeedItem S8_SEED_ITEMS[] = {
    {ITEM_ELECTRICSEED,TERRAIN_ELECTRIC,1,"electricseed",{20,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_GRASSYSEED,TERRAIN_GRASSY,1,"grassyseed",{25,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_MISTYSEED,TERRAIN_MISTY,3,"mistyseed",{46,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_PSYCHICSEED,TERRAIN_PSYCHIC,3,"psychicseed",{58,0,10,S8IE_NONE,S8IE_NONE}},
};
static int s8_seed_parse(const char *key) {
    for(size_t i=0;i<sizeof(S8_SEED_ITEMS)/sizeof(*S8_SEED_ITEMS);i++)
        if(!strcmp(key,S8_SEED_ITEMS[i].key)) return S8_SEED_ITEMS[i].item;
    return ITEM_NONE;
}
// Stage 8i-6: miscellaneous items retain the frozen operation prefix.
enum { S8IE_PARALYZE = S8IE_MENTAL+1 };
typedef struct { int item; const char *key; S8ItemOperation operation; } S8MiscItem;
static const S8MiscItem S8_MISC_ITEMS[] = {
    {ITEM_BIGROOT,"bigroot",{3,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_BINDINGBAND,"bindingband",{4,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_LEEK,"leek",{34,0,60,S8IE_NONE,S8IE_NONE}},
    {ITEM_LIGHTBALL,"lightball",{38,0,30,S8IE_NONE,S8IE_PARALYZE}},
    {ITEM_NORMALGEM,"normalgem",{50,0,0,S8IE_NONE,S8IE_NONE}},
};
static int s8_misc_item_parse(const char *key) {
    for(size_t i=0;i<sizeof(S8_MISC_ITEMS)/sizeof(*S8_MISC_ITEMS);i++)
        if(!strcmp(key,S8_MISC_ITEMS[i].key)) return S8_MISC_ITEMS[i].item;
    return ITEM_NONE;
}
// Stage 8i-7: switch items; operation IDs retain the frozen legacy prefix.
typedef struct { int item; const char *key; S8ItemOperation operation; } S8SwitchItem;
static const S8SwitchItem S8_SWITCH_ITEMS[] = {
    {ITEM_EJECTBUTTON,"ejectbutton",{19,0,30,S8IE_NONE,S8IE_NONE}},
    {ITEM_REDCARD,"redcard",{61,0,10,S8IE_NONE,S8IE_NONE}},
    {ITEM_SHEDSHELL,"shedshell",{67,0,10,S8IE_NONE,S8IE_NONE}},
};
static int s8_switch_item_parse(const char *key) {
    for(size_t i=0;i<sizeof(S8_SWITCH_ITEMS)/sizeof(*S8_SWITCH_ITEMS);i++)
        if(!strcmp(key,S8_SWITCH_ITEMS[i].key)) return S8_SWITCH_ITEMS[i].item;
    return ITEM_NONE;
}
// All consumers (including prior ability headers) see the extended operations
// table. Copy the frozen generated prefix once, then register type-item rows.
static const S8ItemOperation *s8_item_operations(void) {
    static S8ItemOperation operations[ITEM_COUNT];
    static int initialized;
    _Static_assert(sizeof(S8_LEGACY_ITEM_OPERATIONS)/sizeof(*S8_LEGACY_ITEM_OPERATIONS)==ITEM_HERB+1,
                   "legacy item operation prefix changed");
    if(!initialized) {
        memcpy(operations,S8_LEGACY_ITEM_OPERATIONS,sizeof(S8_LEGACY_ITEM_OPERATIONS));
        for(size_t i=0;i<sizeof(S8_TYPE_BOOST_ITEMS)/sizeof(*S8_TYPE_BOOST_ITEMS);i++)
            operations[S8_TYPE_BOOST_ITEMS[i].item]=S8_TYPE_BOOST_ITEMS[i].operation;
        for(size_t i=0;i<sizeof(S8_RESIST_BERRIES)/sizeof(*S8_RESIST_BERRIES);i++)
            operations[S8_RESIST_BERRIES[i].item]=S8_RESIST_BERRIES[i].operation;
        for(size_t i=0;i<sizeof(S8_STATUS_ITEMS)/sizeof(*S8_STATUS_ITEMS);i++)
            operations[S8_STATUS_ITEMS[i].item]=S8_STATUS_ITEMS[i].operation;
        for(size_t i=0;i<sizeof(S8_DURATION_ITEMS)/sizeof(*S8_DURATION_ITEMS);i++)
            operations[S8_DURATION_ITEMS[i].item]=S8_DURATION_ITEMS[i].operation;
        for(size_t i=0;i<sizeof(S8_SEED_ITEMS)/sizeof(*S8_SEED_ITEMS);i++)
            operations[S8_SEED_ITEMS[i].item]=S8_SEED_ITEMS[i].operation;
        for(size_t i=0;i<sizeof(S8_MISC_ITEMS)/sizeof(*S8_MISC_ITEMS);i++)
            operations[S8_MISC_ITEMS[i].item]=S8_MISC_ITEMS[i].operation;
        for(size_t i=0;i<sizeof(S8_SWITCH_ITEMS)/sizeof(*S8_SWITCH_ITEMS);i++)
            operations[S8_SWITCH_ITEMS[i].item]=S8_SWITCH_ITEMS[i].operation;
        initialized=1;
    }
    return operations;
}
#define S8_ITEM_OPERATIONS (s8_item_operations())

enum { MON_UNKNOWN_VOCAB=1u, MON_POST_V1_STATE=2u };
typedef struct { int id, started, fallen; unsigned generation; uint64_t flags; int checked_berserk, ending; int boosts[7]; int berry, counter, gluttony, berry_weaken, unnerved; } S8AbilityEffectState;
typedef struct Mon {
    int stats[6];          // H A B C D S (final values)
    // Stage 8b-2: generated-table IDs NEVER enter legacy mechanics/wire fields.
    // v3 retains the allocation; legacy builds retain their final-stat inverse.
    int spec_v3, table_base_id, table_species_id, table_mega_target;
    int table_ability_id, table_item_id, table_moves[4];
    int points[6], nature_up, nature_down, sex, weight_hg;
    int hp, maxhp;
    int st[5];             // stages A B C D S
    int acc;               // accuracy stage
    int evasion;           // 8c-7: seventh boost, excluded from old wire schemas
    int stockpile, stock_def, stock_spd; // layers and signed successful-change debts
    int power_trick, no_retreat, boost_stats_active;
    int aquaring, ingrain, roost;
    int partial_trap, partial_move; // duration and source move; opposite seat owns link
    int trapped, trapper, leech_seed, octolock, cursed;
    int spikes, toxic_spikes, sticky_web; // side state, like stealth_rock
    int gmax_steelsurge; // Stage 8i-8: removal seam only; no 1on1 producer
    int wish_pending, wish_start, wish_hp; // slot state in the no-switch 1on1 model
    int control[S8C_COUNT], disabled_move, encored_move;
    int rampage_duration, rampage_true, rampage_move;
    int uproar, uproar_struggle, throatchop;
    int sleep, healblock;
    int sub_hp;
    int protect_streak, protected_now;
    int shield_now, quick_guard, wide_guard, guard_stall; // sole member owns its side guards
    int item, mega;
    // Stage 7: mega is the CURRENT form; mode=2 starts normal. Private own state.
    int mega_mode, mega_stats[6], mega_stats_valid;
    int ability;      // AB_*; only Clear Body has an effect within the 17-move pool
    int last_item, last_item_table; // consumed identity belongs to the Pokemon, never the transferred item
    int unburden, used_item_this_turn; // 8d-4b: volatile and per-turn item history
    int ate_berry, stats_raised_turn; // 8c-11c: persistent consumption / per-turn boost history
    int gem; // duration 1; survives item consumption and every hit until residual
    int fling_item; // prepared Fling item until the first Update, including failed hits
    int item_used;    // one-shot item state; travels with the item through Trick
    int choice_lock;  // move code locked by Choice Scarf; -1 = none
    int last_move;    // last move code used (Metronome item streak); -1 = none
    int metro_streak; // consecutive prior uses of last_move
    int flinched;     // set by native effects or King's Rock; checked when this mon's action comes
    int crit_bonus;   // kept in sync with Scope Lens at init; calc reads the item directly
    int type1, type2;
    int added_type; // type+1; zero means absent, preserving zeroed legacy fixtures
    int dragon_cheer, gmax_chi_layers, laser_focus; // copied crit volatiles; no new producers
    int switch_flag, force_switch_flag, switch_trapped, maybe_trapped, bond_triggered;
    // Stage 8i-7: private attempts, independent of successful bench resolution.
    int switch_item_attempts, item_drag_out_attempts;
    int is_sky_dropped, commanding, commanded; // eligibility seams; no new producers
    int transformed, terastallized, dynamax; // form eligibility; tera/dynamax remain unavailable
    const struct Mon *illusion; // appearance only, never mechanics identity
    int hero_message_displayed;
    int active_turns, being_called_back; // Pokemon field lifetime, not ability lifetime
    int flash_fire; // noCopy volatile; End clears it, including suppression
    int ability_changed, ability_suppressed; // ownership and Gastro Acid are distinct
    S8AbilityEffectState ability_state;
    uint64_t ability_individual_flags; // Pokemon lifetime, never reset by End/Start
    int moves[5], pp[5], nmoves; // slot 4 is a called two-turn release, outside the known set
    int policy;
    // Unsupported parsed vocabulary and active post-v1 systems register here
    // (including future field state).
    // Zero is the only state representable by MGO1/g12.
    // Sleep keeps its frozen legacy timer; status stores the other major statuses.
    int status, freeze_turns, toxic_stage, confusion;
    // Compatibility projection only: active battles read the battle-owned FIELD.
    // Direct G3 harnesses may inject weather while no field is attached.
    int weather;
    int reflect, light_screen, stealth_rock, magnet_rise, endured_now;
    int aurora_veil, safeguard, tailwind; // single-slot side timers
    int attempt_prev, attempt_this; // detailed results; zero means no attempt
    int recharge, charging, focusing, focus_lost, round_chained;
    // IDs use move+1 so zero-initialized legacy fixtures have no history.
    int charge_move, beak_hot, turn_cancelled, ally_timer, ally_streak;
    int action_move, used_mask;
    int future_turns; // target slot reservation: 3 -> impact at end of T+2
    // 8c-6: battle history, not observation evidence. Counter records are
    // per matching body hit; attackedBy is the final numeric hit of a move.
    int hurt_this_turn, stats_lowered_turn, times_attacked;
    int attacked_turn, attacked_positive, last_hit_damage;
    int counter_valid, counter_damage, coat_valid, coat_damage;
    unsigned state_features;
} Mon;

// Stage 8d-1d: active move properties are scoped across called/reflected moves.
#include "stage8/ability_property_8d1d.h"
typedef struct {
    const Mon *user, *target;
    int prankster, priority, tracks_target;
    int move, converted, sheer, stench, parental, hit, bypass, infiltrates, scrappy, ignore, ignore_evasion;
    int pledge_combo; // active-move flag seam; no producer in single-member sides
} S8PropertyContext;
static S8PropertyContext *S8_PROPERTY;
static int s8_property_ignored(const Mon *);
static int s8_guard_implemented(int);
static int s8_form_implemented(int);
static int s8_property_flinch(int mv) {
    if(S8_PROPERTY && S8_PROPERTY->move==mv) return S8_PROPERTY->stench || (!S8_PROPERTY->sheer && (S8_PROPERTY->parental?S8_PROPERTY_MOVES[mv][1]:s8_native_flinch(mv)));
    return s8_native_flinch(mv);
}
static int s8_sub_blocks(const Mon *m,int mv) {
    return m->sub_hp>0 && !(S8_PROPERTY && S8_PROPERTY->move==mv && S8_PROPERTY->infiltrates);
}
// Current ownership is authoritative. Legacy initial forms still project AB_*.
static int s8_current_ability(const Mon *m) {
    if(m->spec_v3 || m->ability_changed) return m->table_ability_id;
    if(m->mega) return S8TA_AB_TOUGHCLAWS;
    return m->ability==AB_CB?S8TA_AB_CLEARBODY:m->ability==AB_LM?S8TA_AB_LIGHTMETAL:-1;
}
static unsigned s8_ability_flags(const Mon *m) {
    int id=s8_current_ability(m);
    return id>=0 && id<S8_ABILITIES_COUNT?S8_ABILITY_OPERATION_FLAGS[id]:0;
}
static int s8_form_transform_suppressed(const Mon *,int);
static int s8_has_ability(const Mon *m,int id) {
    return s8_current_ability(m)==id && !s8_form_transform_suppressed(m,id) &&
        (!m->ability_suppressed || (s8_ability_flags(m)&S8TA_CANTSUPPRESS));
}
static int s8_property_sound(int mv) {
    if(S8_PROPERTY && (((s8_property_implemented(s8_current_ability(S8_PROPERTY->user)) || s8_guard_implemented(s8_current_ability(S8_PROPERTY->user)) || s8_form_implemented(s8_current_ability(S8_PROPERTY->user))) && s8_has_ability(S8_PROPERTY->user,s8_current_ability(S8_PROPERTY->user))) ||
       ((s8_property_implemented(s8_current_ability(S8_PROPERTY->target)) || s8_guard_implemented(s8_current_ability(S8_PROPERTY->target)) || s8_form_implemented(s8_current_ability(S8_PROPERTY->target))) && s8_has_ability(S8_PROPERTY->target,s8_current_ability(S8_PROPERTY->target)))))
        return !!(S8_MOVES[mv].flags&S8_FLAG_SOUND);
    return move_traits(mv)->sound;
}
static int s8_ability_projection(int table_id);
static int s8_speed_stat(const Mon *m);
static void s8_set_form(Mon *m, int species_id, int ability_id);
static void s8_calculate_stats(const Mon *m, int species_id, int stats[6]);
static void s8_require_battle(const Mon *a, const Mon *b);
static int s8_old_observation_scope(const Mon *m) {
    // Legacy constructors (including direct pre-v3 harnesses) remain frozen.
    // v3 Metagross has the same old initial state; sex is inert in old vocabulary.
    return !m->spec_v3 || (m->table_base_id==S8_METAGROSS &&
        m->table_species_id==(m->mega?S8_METAGROSS_MEGA:S8_METAGROSS) &&
        m->item>=ITEM_NONE && m->ability>=AB_NONE);
}

// Like OBS, the field belongs to the current serial battle, never to a pilot.
// init_mon invalidates membership before reusing a seat; fixtures bind explicitly.
enum { ATTEMPT_NONE, ATTEMPT_SUCCESS, ATTEMPT_FAIL, ATTEMPT_NOT_FAIL, ATTEMPT_SKIP_FAIL, ATTEMPT_SKIP_NOT_FAIL };
enum { FAINT_DAMAGE, FAINT_SELF_DESTRUCT, FAINT_ATTEMPT_COST, FAINT_RECOIL };
typedef struct { int seat, cause; const Mon *source; const MoveDef *move; } FaintEntry;
typedef struct {
    Mon *seat[2]; int weather, weather_turns, gravity, terrain, turn;
    int terrain_turns, trick_room, magic_room, wonder_room, fairy_lock;
    const Mon *weather_source, *terrain_source;
    int item_ability_context, item_residual; // 8d-4b: isolated mechanics and residual queue state
    int item_context; // real ownership/consumption, isolated from the frozen legacy pool
    int control_residual_active, control_residual_speed[2];
    int track_attempts, trap_context, control_context, guard_context, committed[2], acted[2], consumed[2];
    int turn_context, last_used, move_serial, instructed[2], committed_priority[2];
    int fractional_priority[2], prankster_boosted[2];
    double ability_residual_speed[2];
    FaintEntry faint_queue[2]; int faint_count, faint_processed;
    int move_queued[2]; // includes Struggle; absence is distinct from move ID -1
    int total_fainted[2]; // side lifetime; incremented once per resolved faint
    Mon *party[2][6]; int party_count[2]; // sole member in admitted 1on1
    Mon *power_allies[2][5]; int power_ally_count[2]; // empty in admitted 1on1
    int ignore_breakable; // scoped move-resolution input for subsequent ignore-ability producers
} BattleField;
static BattleField FIELD;
static int field_member(const Mon *m) { return FIELD.seat[0]==m || FIELD.seat[1]==m; }
// ---------- Stage 8d-0: ability lifecycle and event layer ----------
// Stage 8d-0. Ownership, activation state and Pokemon lifetime are separate.
// The 8d-1a rows extend the original three handlers. Queries never consume RNG.
typedef enum {
    S8AE_SWITCH_IN, S8AE_START, S8AE_END, S8AE_MODIFY_MOVE,
    S8AE_PRIORITY, S8AE_SPEED, S8AE_TARGET, S8AE_ACCURACY,
    S8AE_BEFORE_HIT, S8AE_AFTER_HIT, S8AE_AFTER_MOVE, S8AE_RESIDUAL,
    S8AE_TRY_BOOST, S8AE_WEIGHT, S8AE_BASE_POWER,
    S8AE_STATUS, S8AE_ITEM, S8AE_GROUNDED, S8AE_WEATHER, S8AE_TYPE,
    S8AE_REVEAL, S8AE_SUPPRESS, S8AE_RESTORE,
    S8AE_ATK, S8AE_SPA, S8AE_STAB, S8AE_CRIT, S8AE_DAMAGE,
    S8AE_SET_STATUS, S8AE_UPDATE, S8AE_BURN, S8AE_HEAL_PULSE,
    S8AE_DEF, S8AE_SPD, S8AE_TRY_HIT, S8AE_TRY_HIT_SIDE,
    S8AE_INDIRECT, S8AE_IMMUNITY, S8AE_WEATHER_TICK,
    S8AE_HIT, S8AE_DAMAGING_HIT, S8AE_SOURCE_DAMAGING_HIT,
    S8AE_AFTER_STATUS, S8AE_FLINCH, S8AE_AFTER_BOOST,
    S8AE_BEFORE_DAMAGE, S8AE_TRY_EAT, S8AE_AFTER_SECONDARY,
    S8AE_MODIFY_TYPE, S8AE_HP_DAMAGE, S8AE_DRAIN, S8AE_HIT_PROTECT, S8AE_SOURCE_AFTER_FAINT,
    S8AE_TRY_VOLATILE, S8AE_CRITICAL_HIT, S8AE_MODIFY_SECONDARIES, S8AE_STATUS_IMMUNITY,
    S8AE_CHANGE_BOOST, S8AE_AFTER_EACH_BOOST, S8AE_FOE_AFTER_BOOST,
    S8AE_MODIFY_ACCURACY, S8AE_SOURCE_MODIFY_ACCURACY, S8AE_ALWAYS_ACCURACY,
    S8AE_INVULNERABILITY, S8AE_TRY_MOVE, S8AE_ANY_DAMAGE, S8AE_DRAG_OUT,
    S8AE_AFTER_SWITCH_IN, S8AE_AFTER_MEGA, S8AE_AFTER_TERA,
    S8AE_WEATHER_CHANGE, S8AE_TERRAIN_CHANGE, S8AE_WEATHER_DAMAGE,
    S8AE_FRACTIONAL_PRIORITY, S8AE_FOE_TRY_MOVE, S8AE_SLEEP_COUNTER,
    S8AE_BEFORE_SWITCH_IN, S8AE_SWITCH_OUT, S8AE_FAINT, S8AE_ALLY_FAINT,
    S8AE_PREPARE_HIT, S8AE_EFFECTIVENESS,
    S8AE_EAT_ITEM, S8AE_TAKE_ITEM, S8AE_AFTER_USE_ITEM, S8AE_FOE_TRY_EAT,
    S8AE_ITEM_HEAL, S8AE_AFTER_SECONDARY_SELF, S8AE_DEDUCT_PP,
    S8AE_EMERGENCY_EXIT, S8AE_TRAP, S8AE_MAYBE_TRAP, S8AE_FOE_TRAP, S8AE_FOE_MAYBE_TRAP
} S8AbilityEventId;
enum { S8A_INTERNAL, S8A_PUBLIC };
typedef struct {
    S8AbilityEventId event;
    const Mon *source, *target;
    const MoveDef *move;
    int value, detail;
} S8AbilityEvent;
typedef struct {
    int ability;
    S8AbilityEventId event;
    int order, priority, suborder;
    void (*callback)(const Mon *, S8AbilityEvent *);
} S8AbilityHandler;
static int S8_ABILITY_TRACE;
static const Mon *S8_FIELD_AURA;
static int S8_FIELD_ABILITY_DEPTH; // Pokemon.effectiveWeather distinguishes Move from Ability effects.
static Rng *BATTLE_RNG;
static int damage_chain(int modifier,int next);
static double eff_speed(const Mon *m);
static void obs3_ability(const Mon *,int,int,int);
static void s8_ability_record(const Mon *m,S8AbilityEventId event,int visibility) {
    int identity=event==S8AE_SUPPRESS && visibility==S8A_PUBLIC?-1:s8_current_ability(m);
    // Internal lifecycle notifications are available to fixtures/debug sinks.
    // Only explicit battle messages reveal an ability. Passive power/weight
    // queries and Start of these three passive abilities reveal nothing.
#ifdef ENGINE_ABILITY_EVENT_SINK
    ENGINE_ABILITY_EVENT_SINK(event,m,identity,visibility);
#endif
    obs3_ability(m,identity,event,visibility);
    if(S8_ABILITY_TRACE && visibility==S8A_PUBLIC) {
        int id=identity;
        printf("ability public %c %d %s\n",m==FIELD.seat[1]?'B':'A',event,
               id>=0 && id<S8_ABILITIES_COUNT?S8_ABILITIES[id].id:"none");
    }
    (void)m;(void)event;
}
static void s8_ability_clearbody(const Mon *holder,S8AbilityEvent *e) {
    if(holder!=e->source && e->value<0) e->value=0;
}
static void s8_ability_lightmetal(const Mon *holder,S8AbilityEvent *e) {
    (void)holder;e->value/=2; // trunc(weighthg / 2), before minimum weight
}
static void s8_ability_toughclaws(const Mon *holder,S8AbilityEvent *e) {
    (void)holder;
    if(e->move && e->move->contact) e->value=damage_chain(e->value,5325);
}
// Stage 8d-1a extends the registry here; the predecessor artifact stays intact.
static int item_mod(int value,int modifier);
static int has_type(const Mon *m,int type);
static void clear_major_status(Mon *m);
static const S8Move *s8_power_move(const MoveDef *move) {
    if(move) for(int i=0;i<S8_MOVES_COUNT;i++)
        if(!strcmp(move->code,S8_MOVES[i].code)) return &S8_MOVES[i];
    static const S8Move empty={0};return &empty;
}
static int s8_power_primary_status(const MoveDef *move) {
    if(move) for(int i=0;i<S8_MOVES_COUNT;i++) if(!strcmp(move->code,S8_MOVES[i].code)) {
        const S8PrimaryStatus *row=s8_primary_status(i);return row && row->status;
    }
    return 0;
}
static int s8_power_other_will_move(const Mon *holder) {
    if(!field_member(holder)) return 0;
    int other=holder!=FIELD.seat[1];const Mon *m=FIELD.seat[other];
    // willMove is about the remaining move queue, not successful execution.
    return m && m->hp>0 && !m->turn_cancelled && !FIELD.consumed[other] && FIELD.move_queued[other];
}
static int s8_power_has_partner(const Mon *holder);
#include "stage8/ability_power_8d1a.h"
static int s8_power_has_partner(const Mon *holder) {
    if(!field_member(holder)) return 0;
    int seat=holder==FIELD.seat[1];
    for(int i=0;i<FIELD.power_ally_count[seat];i++) {
        const Mon *ally=FIELD.power_allies[seat][i];
        if(ally && ally!=holder && ally->hp>0 &&
           (s8_has_ability(ally,S8P_PLUS) || s8_has_ability(ally,S8P_MINUS))) return 1;
    }
    return 0;
}
// Effect identity for Damage/Immunity; direct HP costs bypass Damage entirely.
enum { S8R_OTHER, S8R_MOVE, S8R_RECOIL, S8R_BURN, S8R_SAND, S8R_HAIL, S8R_POWDER, S8R_POISON };
static int s8_resist_allies(const Mon *a,const Mon *b);
static void s8_resist_try_effect(const Mon *,S8AbilityEvent *,int);
static void s8_resist_damage_effect(const Mon *,S8AbilityEvent *,int);
static void s8_resist_weather_effect(const Mon *,S8AbilityEvent *);
static void s8_resist_redirect(const Mon *,S8AbilityEvent *);
static void s8_resist_side_effect(const Mon *,S8AbilityEvent *,int);
#include "stage8/ability_resist_8d1b.h"
// Stage 8d-1c: move-local hit data survives nested status callbacks, while
// called moves receive their own frame. total excludes Substitute damage.
typedef struct {
    Rng *rng;
    const Mon *user;
    int used_move; // root action move+1; called moves do not replace lastMove
    int crit, damage, total, last_damage, last_hp, has_last, multihit, smart_target, finished;
} S8HitContext;
static S8HitContext *S8_HIT_CONTEXT;
static int S8_SLEEP_CALLER;
static const char *S8_STATUS_CAUSE;
static int S8_BOOST_EFFECT=-1;
static int s8_change_boost(Mon *,Mon *,const int[7]);
static int s8_control_add(Mon *,int,int);
static int s8_hit_contact(const MoveDef *,const Mon *);
static int s8_hit_powder(const Mon *);
static int s8_hit_has_item(const Mon *,const char *);
static int s8_hit_has_named_ability(const Mon *,const char *);
static int s8_hit_random(int);
static int s8_hit_chance(int,int);
static void s8_hit_retaliate(Mon *,int);
static void s8_hit_inflict(Mon *,Mon *,int,const char *);
static void s8_hit_attract(Mon *,Mon *);
static void s8_hit_disable(Mon *,const MoveDef *);
static void s8_hit_berserk(Mon *,S8AbilityEvent *);
#include "stage8/ability_hit_8d1c.h"
// Stage 8d-2a: reviewed guards share status/volatile and secondary admission.
static void s8_guard_callback(const Mon *,S8AbilityEvent *);
#include "stage8/ability_guard_8d2a.h"
// Stage 8d-2b: boost objects, accuracy and reflected moves share event dispatch.
static void s8_boost_callback(const Mon *,S8AbilityEvent *);
#include "stage8/ability_boost_8d2b.h"
static void s8_field_callback(const Mon *,S8AbilityEvent *);
#include "stage8/ability_field_8d3a.h"
static void s8_weather_callback(const Mon *,S8AbilityEvent *);
#include "stage8/ability_weather_8d3b.h"
#include "stage8/ability_form_8d4a.h"
#include "stage8/ability_item_8d4b.h"
#include "stage8/ability_switch_8d4c.h"
static void s8_switch_reset(void);
static void s8_after_faint(void);
static int s8_can_switch(const Mon *);
static int s8_drag_out(Mon *,Mon *,const MoveDef *);
static void s8_switch_traps(Mon *);
static int S8_ITEM_DAMAGE_EXEC;
static int S8_ITEM_EFFECT; // scoped berry effect for ChangeBoost, never a move
static int s8_items_try_eat(Mon *m);
static int s8_items_blocked(const Mon *m);
static void s8_field_item_update(Mon *,Mon *);
static void s8_item_enable(Mon *,Mon *);
static void s8_items_residual(Mon *,Mon *,int);
static void s8_seed_check(Mon *);
static int s8_items_context(const Mon *a,const Mon *b) {
    return (s8_items_implemented(s8_current_ability(a)) && s8_current_ability(a)!=S8I_LIGHTMETAL && s8_current_ability(a)!=S8I_HEAVYMETAL) ||
        (b && s8_items_implemented(s8_current_ability(b)) && s8_current_ability(b)!=S8I_LIGHTMETAL && s8_current_ability(b)!=S8I_HEAVYMETAL);
}
static void s8_items_activate(Mon *m) {
    if(field_member(m) && s8_items_context(m,NULL)) {
        FIELD.item_ability_context=1;s8_item_enable(FIELD.seat[0],FIELD.seat[1]);
    }
}
static int s8_form_transform_suppressed(const Mon *m,int id) {
    return m->transformed && id>=0 && id<S8_ABILITIES_COUNT && (S8_FORM_FLAGS[id]&S8F_NOTRANSFORM);
}
static int s8_form_change(Mon *,const char *,int);
static int s8_transform_into(Mon *,const Mon *);
static int s8_swap_abilities(Mon *,Mon *);
static void s8_form_faint(Mon *);
enum { S8Q_MIRROR=-20, S8Q_COPY=-21, S8Q_MIRROR_HERB=-22, S8Q_ZPOWER=-23 };
static int S8_BOUNCED;
static int s8_change_boost_event(Mon *,Mon *,const int[7],int);
static int s8_semi(const Mon *);
static int s8_boost_modern(const Mon *a,const Mon *b) {
    return (a && (a->evasion || s8_has_ability(a,S8W_INTIMIDATE) || s8_has_ability(a,S8W_SUPERSWEETSYRUP))) ||
           (b && (b->evasion || s8_has_ability(b,S8W_INTIMIDATE) || s8_has_ability(b,S8W_SUPERSWEETSYRUP))) ||
           (a && s8_current_ability(a)!=S8Q_CLEARBODY && s8_boost_implemented(s8_current_ability(a))) ||
           (b && s8_current_ability(b)!=S8Q_CLEARBODY && s8_boost_implemented(s8_current_ability(b)));
}
static int s8_boost_any(Mon *,Mon *,S8AbilityEventId,const MoveDef *,int,int);
static int s8_resist_scope_of(const S8AbilityHandler *row) {
    if(row->ability==S8I_RIPEN && row->event==S8AE_DAMAGE) return 1;
    if(row->ability==S8W_FAIRYAURA && row->event==S8AE_BASE_POWER) return 3;
    if(row->ability==S8G_PURIFYINGSALT && (row->event==S8AE_ATK || row->event==S8AE_SPA)) return 1;
    int scope=s8_resist_scope(row);return scope<0?s8_power_scope(row):scope;
}
static int s8_resist_is_breakable(int id) { return s8_property_breakable(id) || s8_power_breakable(id) || s8_resist_breakable(id) || (s8_boost_ability(id) && s8_boost_ability(id)->breakable) || id==S8F_DISGUISE || id==S8I_STICKYHOLD || id==S8I_HEAVYMETAL || id==S8I_LIGHTMETAL; }
static int s8_property_ignored(const Mon *m) {
    return S8_PROPERTY && S8_PROPERTY->ignore && S8_PROPERTY->user!=m &&
        (s8_property_breakable(s8_current_ability(m)) || s8_current_ability(m)==S8F_DISGUISE ||
         s8_current_ability(m)==S8I_STICKYHOLD || s8_current_ability(m)==S8I_HEAVYMETAL || s8_current_ability(m)==S8I_LIGHTMETAL);
}
static int s8_property_rank(const Mon *m,int stat,int value) {
    if(!S8_PROPERTY || m==NULL) return value;
    const Mon *user=S8_PROPERTY->user,*target=S8_PROPERTY->target;
    if(m==target && user!=target && s8_has_ability(user,S8D_UNAWARE) && (stat==1 || stat==3 || stat==6)) return 0;
    if(m==user && user!=target && s8_has_ability(target,S8D_UNAWARE) && !s8_property_ignored(target) && (stat==0 || stat==1 || stat==2 || stat==5)) return 0;
    return value;
}
static void s8_property_modify(const Mon *,S8AbilityEvent *);
static void s8_property_power(const Mon *,S8AbilityEvent *);
static void s8_property_survive(const Mon *,S8AbilityEvent *);
static void s8_property_sturdy_try(const Mon *,S8AbilityEvent *);
static void s8_property_ooze(const Mon *,S8AbilityEvent *);
static void s8_property_eelevate(const Mon *,S8AbilityEvent *);
static void s8_property_start(const Mon *holder,S8AbilityEvent *e) {
    (void)e;s8_ability_record(holder,S8AE_START,S8A_PUBLIC);
}
static void s8_property_scrappy_boost(const Mon *holder,S8AbilityEvent *e) {
    (void)holder;if(e->detail==0 && S8_BOOST_EFFECT==S8_HIT_INTIMIDATE) e->value=0;
}
static const S8AbilityHandler S8_ABILITY_HANDLERS[]={
    S8_SWITCH_HANDLER_ROWS
    S8_ITEM_HANDLER_ROWS
    S8_FORM_HANDLER_ROWS
    S8_WEATHER_HANDLER_ROWS
    {S8V3_EARLYBIRD,S8AE_SLEEP_COUNTER,0,0,0,s8_weather_callback},
    S8_FIELD_HANDLER_ROWS
    S8_BOOST_HANDLER_ROWS
    {S8TA_AB_CLEARBODY,S8AE_TRY_BOOST,0,0,0,s8_ability_clearbody},
    {S8TA_AB_LIGHTMETAL,S8AE_WEIGHT,0,0,0,s8_ability_lightmetal},
    {S8TA_AB_TOUGHCLAWS,S8AE_BASE_POWER,0,21,0,s8_ability_toughclaws},
    S8_POWER_HANDLER_ROWS
    S8_RESIST_HANDLER_ROWS
    S8_HIT_HANDLER_ROWS
    S8_GUARD_HANDLER_ROWS
    {S8D_AERILATE,S8AE_MODIFY_TYPE,0,-1,0,s8_property_modify},
    {S8D_DRAGONIZE,S8AE_MODIFY_TYPE,0,-1,0,s8_property_modify},
    {S8D_PIXILATE,S8AE_MODIFY_TYPE,0,-1,0,s8_property_modify},
    {S8D_REFRIGERATE,S8AE_MODIFY_TYPE,0,-1,0,s8_property_modify},
    {S8D_LIQUIDVOICE,S8AE_MODIFY_TYPE,0,-1,0,s8_property_modify},
    {S8D_AERILATE,S8AE_BASE_POWER,0,23,0,s8_property_power},
    {S8D_DRAGONIZE,S8AE_BASE_POWER,0,23,0,s8_property_power},
    {S8D_PIXILATE,S8AE_BASE_POWER,0,23,0,s8_property_power},
    {S8D_REFRIGERATE,S8AE_BASE_POWER,0,23,0,s8_property_power},
    {S8D_SHEERFORCE,S8AE_BASE_POWER,0,21,0,s8_property_power},
    {S8D_INFILTRATOR,S8AE_MODIFY_MOVE,0,0,0,s8_property_modify},
    {S8D_LONGREACH,S8AE_MODIFY_MOVE,0,0,0,s8_property_modify},
    {S8D_MOLDBREAKER,S8AE_MODIFY_MOVE,0,0,0,s8_property_modify},
    {S8D_SCRAPPY,S8AE_MODIFY_MOVE,0,-5,0,s8_property_modify},
    {S8D_SHEERFORCE,S8AE_MODIFY_MOVE,0,0,0,s8_property_modify},
    {S8D_STENCH,S8AE_MODIFY_MOVE,0,-1,0,s8_property_modify},
    {S8D_SKILLLINK,S8AE_MODIFY_MOVE,0,0,0,s8_property_modify},
    {S8D_PARENTALBOND,S8AE_MODIFY_MOVE,0,0,0,s8_property_modify},
    {S8D_MOLDBREAKER,S8AE_START,0,0,0,s8_property_start},
    {S8D_SCRAPPY,S8AE_TRY_BOOST,0,0,0,s8_property_scrappy_boost},
    {S8D_STURDY,S8AE_TRY_HIT,0,0,0,s8_property_sturdy_try},
    {S8D_STURDY,S8AE_HP_DAMAGE,0,-30,0,s8_property_survive},
    {S8D_LIQUIDOOZE,S8AE_DRAIN,0,0,7,s8_property_ooze},
    {S8D_EELEVATE,S8AE_SOURCE_AFTER_FAINT,0,0,0,s8_property_eelevate},
    {S8D_PIERCINGDRILL,S8AE_HIT_PROTECT,0,0,0,s8_property_modify},
    {S8D_UNSEENFIST,S8AE_HIT_PROTECT,0,0,0,s8_property_modify},
};
// Pinned comparePriority: Order (zero = unspecified), descending Priority,
// descending holder Speed, ascending SubOrder, ascending effectOrder.
// Calls capture Speed before callbacks, so a callback cannot reorder its peers.
typedef struct {
    const Mon *holder;
    const S8AbilityHandler *handler;
    double speed;
    unsigned effect_order;
} S8AbilityCall;
static int s8_ability_handler_before(const S8AbilityHandler *a,const S8AbilityHandler *b) {
    int64_t ao=a->order?a->order:INT64_C(4294967296);
    int64_t bo=b->order?b->order:INT64_C(4294967296);
    if(ao!=bo) return ao<bo;
    if(a->priority!=b->priority) return a->priority>b->priority;
    return a->suborder<b->suborder;
}
static int s8_ability_call_compare(const S8AbilityCall *a,const S8AbilityCall *b) {
    S8AbilityHandler ar=*a->handler,br=*b->handler;
    ar.suborder=br.suborder=0;
    if(s8_ability_handler_before(&ar,&br)) return -1;
    if(s8_ability_handler_before(&br,&ar)) return 1;
    if(a->speed!=b->speed) return a->speed>b->speed?-1:1;
    if(a->handler->suborder!=b->handler->suborder)
        return a->handler->suborder<b->handler->suborder?-1:1;
    if(a->effect_order!=b->effect_order) return a->effect_order<b->effect_order?-1:1;
    return 0;
}
static int s8_ability_run_queue(S8AbilityCall *calls,int count,S8AbilityEvent *e,Rng *tie_rng) {
    // Optional cross-holder tie RNG is supplied by the collecting event owner.
    // Single-holder queries and the passive legacy lifecycle never draw.
    for(int n=0;n<count;n++) {
        int best=n,ties=1;
        for(int i=n+1;i<count;i++) {
            int cmp=s8_ability_call_compare(&calls[i],&calls[best]);
            if(cmp<0) { best=i;ties=1; }
            else if(!cmp) ties++;
        }
        if(tie_rng && ties>1) {
            int pick=rng_int(tie_rng,ties);
            for(int i=n;i<count;i++) if(!s8_ability_call_compare(&calls[i],&calls[best]) && !pick--) { best=i;break; }
        }
        S8AbilityCall chosen=calls[best];
        // Stable removal preserves definition order when no RNG is requested.
        for(int i=best;i>n;i--) calls[i]=calls[i-1];
        calls[n]=chosen;
        S8_FIELD_ABILITY_DEPTH++;
        chosen.handler->callback(chosen.holder,e);
        if(e->event==S8AE_RESIDUAL && chosen.handler->order==28 && FIELD.item_ability_context && FIELD.item_residual)
            s8_field_item_update(FIELD.seat[0],FIELD.seat[1]);
        S8_FIELD_ABILITY_DEPTH--;
    }
    return e->value;
}
static int s8_ability_dispatch_rows(const Mon *holder,S8AbilityEvent *e,
                                   const S8AbilityHandler *rows,int count) {
    if(s8_property_ignored(holder)) return e->value;
    S8AbilityCall calls[count>0?count:1];int used=0;
    for(int i=0;i<count;i++) if(rows[i].event==e->event && s8_resist_scope_of(&rows[i])!=1 && s8_resist_scope_of(&rows[i])!=4 && s8_has_ability(holder,rows[i].ability))
        calls[used++]=(S8AbilityCall){holder,&rows[i],0,0};
    return s8_ability_run_queue(calls,used,e,NULL);
}
static int s8_ability_event(const Mon *holder,S8AbilityEventId event,const Mon *source,
                           const Mon *target,const MoveDef *move,int value,int detail) {
    S8AbilityEvent e={event,source,target,move,value,detail};
    if(event==S8AE_DRAG_OUT && holder!=source && s8_current_ability(holder)==S8SC_SUCTIONCUPS &&
       (FIELD.ignore_breakable || (S8_PROPERTY && S8_PROPERTY->ignore))) return value;
    if(FIELD.ignore_breakable && ((holder!=source && s8_current_ability(holder)==S8F_DISGUISE) ||
       ((event==S8AE_WEIGHT || event==S8AE_TAKE_ITEM) && (s8_current_ability(holder)==S8I_HEAVYMETAL || s8_current_ability(holder)==S8I_LIGHTMETAL || s8_current_ability(holder)==S8I_STICKYHOLD)))) return value;
    const S8BoostAbility *boost=s8_boost_ability(s8_current_ability(holder));
    if(boost && boost->breakable && holder!=source &&
       (FIELD.ignore_breakable || (S8_PROPERTY && S8_PROPERTY->ignore && S8_PROPERTY->user!=holder))) return value;
    int guard_attack=s8_guard_implemented(s8_current_ability(holder)) &&
        (event==S8AE_TRY_BOOST || event==S8AE_HIT);
    if((guard_attack || event==S8AE_TRY_VOLATILE || event==S8AE_CRITICAL_HIT ||
       event==S8AE_MODIFY_SECONDARIES || event==S8AE_STATUS_IMMUNITY ||
       event==S8AE_SET_STATUS || event==S8AE_TRY_HIT || event==S8AE_DEF || event==S8AE_SPD ||
       s8_current_ability(holder)==S8H_THERMALEXCHANGE) && holder!=source &&
       FIELD.ignore_breakable && s8_resist_is_breakable(s8_current_ability(holder))) return value;
#ifdef ENGINE_ABILITY_DISPATCH_SINK
    ENGINE_ABILITY_DISPATCH_SINK(holder,event,detail);
#endif
    return s8_ability_dispatch_rows(holder,&e,S8_ABILITY_HANDLERS,
        (int)(sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS)));
}
static Rng *S8_POWER_RNG; // scoped to execution; deterministic read-only estimates
static int held_item(const Mon *m);
static int s8_item_base_species(const Mon *m,const char *key) {
    return m->spec_v3 && !strcmp(S8_SPECIES[m->table_base_id].id,key);
}
static void s8_lightball_stat(const Mon *holder,S8AbilityEvent *e) {
    (void)holder;e->value=damage_chain(e->value,8192);
}
static int s8_power_damage_item_active(const Mon *holder);
static void s8_power_item_damage(const Mon *holder,S8AbilityEvent *e);
static void s8_power_resist_damage(const Mon *holder,S8AbilityEvent *e) {
    (void)holder;e->value=damage_chain(e->value,2048);
}
static int s8_power_damage_event_range(const Mon *source,const Mon *target,S8AbilityEventId event,
                                 const MoveDef *move,int value,int detail,int low,int high) {
    S8AbilityEvent e={event,source,target,move,value,detail};
    const Mon *holders[12]={source,target};int count=2;
    if(field_member(source)) {
        int seat=source==FIELD.seat[1];
        for(int side=0;side<2;side++) for(int i=0;i<FIELD.power_ally_count[side] && count<12;i++) {
            const Mon *ally=FIELD.power_allies[side][i];
            if(ally && ally!=source && ally!=target && ally->hp>0 &&
               (side==seat || s8_has_ability(ally,S8R_FRIENDGUARD))) holders[count++]=ally;
        }
    }
    S8AbilityCall calls[sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS)*12+3];int used=0,modern=0;
    const Mon *saved_aura=S8_FIELD_AURA;S8_FIELD_AURA=NULL;
    for(int k=0;k<count;k++) if(holders[k])
        for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
            const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
            if(row->priority<low || row->priority>high) continue;
            if(row->ability==S8W_FAIRYAURA && holders[k]->hp<=0) continue;
            if(row->ability==S8Q_HUSTLE && event==S8AE_ATK) continue;
            int scope=s8_resist_scope_of(row);
            if(row->event!=event || (scope==3?0:scope==1?k!=1:scope==2?(k==1 || !s8_resist_allies(source,holders[k])):k!=0)) continue;
            if(!s8_has_ability(holders[k],row->ability) || s8_property_ignored(holders[k])) continue;
            if(FIELD.ignore_breakable && holders[k]!=source && s8_resist_is_breakable(row->ability)) continue;
            calls[used++]=(S8AbilityCall){holders[k],row,eff_speed(holders[k]),0};
            modern|=s8_field_implemented(row->ability) || s8_power_implemented(row->ability) || s8_resist_implemented(row->ability) || s8_guard_implemented(row->ability);
        }
    const S8AbilityHandler lightball={-1,event,0,1,0,s8_lightball_stat};
    if((event==S8AE_ATK || event==S8AE_SPA) && low<=1 && high>=1 &&
       held_item(source)==ITEM_LIGHTBALL && s8_item_base_species(source,"pikachu"))
        calls[used++]=(S8AbilityCall){source,&lightball,eff_speed(source),0};
    // The source item shares ModifyDamage priority 0 with Sniper/Punk Rock.
    // Keep it in the same speed-sorted chain; never multiply a prebuilt chain.
    const S8AbilityHandler item={-1,S8AE_DAMAGE,0,0,0,s8_power_item_damage};
    if(event==S8AE_DAMAGE && s8_power_damage_item_active(source))
        calls[used++]=(S8AbilityCall){source,&item,eff_speed(source),0};
    const S8AbilityHandler resist={-1,S8AE_DAMAGE,0,0,0,s8_power_resist_damage};
    if(event==S8AE_DAMAGE && target && (detail&2))
        calls[used++]=(S8AbilityCall){target,&resist,eff_speed(target),0};
    int result=s8_ability_run_queue(calls,used,&e,modern?S8_POWER_RNG:NULL);
    S8_FIELD_AURA=saved_aura;return result;
}
static int s8_power_damage_event(const Mon *source,const Mon *target,S8AbilityEventId event,
                                 const MoveDef *move,int value,int detail) {
    return s8_power_damage_event_range(source,target,event,move,value,detail,INT_MIN,INT_MAX);
}
static int s8_resist_side(const Mon *m) {
    for(int side=0;side<2;side++) {
        if(FIELD.seat[side]==m) return side;
        for(int i=0;i<FIELD.power_ally_count[side];i++) if(FIELD.power_allies[side][i]==m) return side;
    }
    return -1;
}
static int s8_resist_allies(const Mon *a,const Mon *b) {
    int side=s8_resist_side(a);return a && b && (a==b || (side>=0 && side==s8_resist_side(b)));
}
static int s8_heal_hp(Mon *m,int amount);
static int s8_property_drain(Mon *,Mon *,int,int);
static int s8_change_boost(Mon *target,Mon *source,const int delta[7]);
static int current_weather(const Mon *m);
static int s8_resist_matches(const Mon *holder,const Mon *source,const MoveDef *move,int ab) {
    if(!move) return 0;
    uint64_t flags=s8_power_move(move)->flags;
    if(ab==S8R_BULLETPROOF) return !!(flags&S8_FLAG_BULLET);
    if(holder==source) return 0;
    if(ab==S8R_GOODASGOLD) return move->cat==CAT_STATUS;
    if(ab==S8R_TELEPATHY) return move->cat!=CAT_STATUS && s8_resist_allies(holder,source);
    if(ab==S8R_OVERCOAT) return (flags&S8_FLAG_POWDER) && !has_type(holder,T_GRASS);
    if(ab==S8R_SOUNDPROOF) return !!(flags&S8_FLAG_SOUND);
    int type=ab==S8R_DRYSKIN || ab==S8R_WATERABSORB?T_WATER:
        ab==S8R_EARTHEATER?T_GROUND:ab==S8R_FLASHFIRE?T_FIRE:
        ab==S8R_LIGHTNINGROD || ab==S8R_MOTORDRIVE || ab==S8R_VOLTABSORB?T_ELEC:
        ab==S8R_SAPSIPPER?T_GRASS:-1;
    return type>=0 && move->type==type;
}
static void s8_resist_try_effect(const Mon *holder,S8AbilityEvent *e,int ab) {
    if(!e->value || !s8_resist_matches(holder,e->source,e->move,ab)) return;
    Mon *m=(Mon *)holder;
    if(ab==S8R_DRYSKIN || ab==S8R_EARTHEATER || ab==S8R_VOLTABSORB || ab==S8R_WATERABSORB)
        s8_heal_hp(m,m->maxhp/4>0?m->maxhp/4:1);
    else if(ab==S8R_LIGHTNINGROD || ab==S8R_MOTORDRIVE || ab==S8R_SAPSIPPER) {
        int delta[7]={0};delta[ab==S8R_LIGHTNINGROD?2:ab==S8R_MOTORDRIVE?4:0]=1;
        s8_change_boost(m,m,delta);
    } else if(ab==S8R_FLASHFIRE) m->flash_fire=1;
    s8_ability_record(holder,S8AE_TRY_HIT,S8A_PUBLIC);e->value=0;
}
static void s8_resist_damage_effect(const Mon *holder,S8AbilityEvent *e,int ab) {
    (void)holder;
    if(ab==S8R_MAGICGUARD && e->detail!=S8R_MOVE) e->value=0;
    if(ab==S8R_ROCKHEAD && e->detail==S8R_RECOIL) e->value=0;
    if(ab==S8R_HEATPROOF && e->detail==S8R_BURN && e->value>0) {
        e->value/=2;if(!e->value) e->value=1;
    }
}
static int s8_resist_loss(const Mon *m,int amount,int effect) {
    if(amount>0) s8_ability_event(m,S8AE_BEFORE_DAMAGE,m,m,NULL,amount,1);
    if(effect==S8R_SAND || effect==S8R_HAIL)
        if(!s8_ability_event(m,S8AE_IMMUNITY,m,m,NULL,1,effect)) return 0;
    return s8_ability_event(m,S8AE_INDIRECT,m,m,NULL,amount,effect);
}
static void s8_resist_weather_effect(const Mon *holder,S8AbilityEvent *e) {
    Mon *m=(Mon *)holder;
    if(m->hp<=0 || current_weather(m)!=e->detail) return;
    int amount=m->maxhp/8;if(amount<1) amount=1;
    if(e->detail==WEATHER_RAIN) s8_heal_hp(m,amount);
    else if(e->detail==WEATHER_SUN) {
        amount=s8_resist_loss(m,amount,S8R_OTHER);
        int before=m->hp;m->hp=amount<before?before-amount:0;
        // Notification lives at the HP-owning weather seam below.
        e->value=before-m->hp;
    }
}
static void s8_resist_redirect(const Mon *holder,S8AbilityEvent *e) {
    if(e->detail || !e->move || e->move->type!=T_ELEC || holder==e->source) return;
    const char *target=s8_power_move(e->move)->target;
    // Only single Pokemon target classes can be redirected. The admitted duel
    // has exactly one valid recipient; side/field/self moves retain their target.
    if(!strcmp(target,"normal") || !strcmp(target,"any") || !strcmp(target,"randomNormal") ||
       (!strcmp(target,"adjacentFoe") && !s8_resist_allies(holder,e->source))) {
        if(e->target!=holder) s8_ability_record(holder,S8AE_TARGET,S8A_PUBLIC);
        e->target=holder;e->detail=1;
    }
}
static void s8_resist_side_effect(const Mon *holder,S8AbilityEvent *e,int ab) {
    if(!e->move || !s8_resist_allies(holder,e->target)) return;
    if(ab==S8R_SOUNDPROOF && (s8_power_move(e->move)->flags&S8_FLAG_SOUND))
        s8_ability_record(holder,S8AE_TRY_HIT_SIDE,S8A_PUBLIC); // raw callback has no return
    if(ab==S8R_SAPSIPPER && holder!=e->source && s8_resist_allies(e->target,e->source) && e->move->type==T_GRASS) {
        int delta[7]={1,0,0,0,0,0,0};s8_change_boost((Mon *)holder,(Mon *)holder,delta);
    }
}
static Mon *s8_resist_select_target(Mon *source,Mon *target,const MoveDef *move) {
    if(s8_has_ability(source,S8F_STALWART) || (S8_PROPERTY && S8_PROPERTY->tracks_target)) return target;
    const Mon *holders[12]={source,target};int count=2;
    if(field_member(source)) for(int side=0;side<2;side++)
        for(int i=0;i<FIELD.power_ally_count[side] && count<12;i++) {
            const Mon *m=FIELD.power_allies[side][i];
            if(m && m!=source && m!=target && m->hp>0) holders[count++]=m;
        }
    S8AbilityCall calls[12];int used=0;
    for(int k=0;k<count;k++) if(holders[k] && holders[k]->hp>0)
        for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
            const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
            if(row->event!=S8AE_TARGET || s8_resist_scope(row)!=4 || !s8_has_ability(holders[k],row->ability)) continue;
            if(FIELD.ignore_breakable && holders[k]!=source && s8_resist_breakable(row->ability)) continue;
            calls[used++]=(S8AbilityCall){holders[k],row,eff_speed(holders[k]),0};
        }
    S8AbilityEvent e={S8AE_TARGET,source,target,move,1,0};
    s8_ability_run_queue(calls,used,&e,S8_POWER_RNG);return (Mon *)e.target;
}
static int s8_ability_boost(const Mon *target,const Mon *source,int delta) {
    return s8_ability_event(target,S8AE_TRY_BOOST,source,target,NULL,delta,0);
}
static void s8_boost_phase(const Mon *source,const Mon *target,S8AbilityEventId event,int mv,int detail) {
    const Mon *holders[]={source,target};
    int form_residual=event==S8AE_RESIDUAL &&
        (s8_has_ability(source,S8F_HUNGERSWITCH) || (target && s8_has_ability(target,S8F_HUNGERSWITCH)));
    S8AbilityCall calls[2];int n=0;
    for(int k=0;k<2;k++) {
        const Mon *m=holders[k];
        if(!m || m->hp<=0 || (k && m==source) || (!s8_has_ability(m,S8Q_OPPORTUNIST) && !(event==S8AE_RESIDUAL && s8_has_ability(m,S8F_HUNGERSWITCH)))) continue;
        for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
            const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
            if((row->ability==S8Q_OPPORTUNIST || (event==S8AE_RESIDUAL && row->ability==S8F_HUNGERSWITCH)) &&
               s8_has_ability(m,row->ability) && row->event==event) {
                double speed=form_residual && field_member(m)?FIELD.ability_residual_speed[m==FIELD.seat[1]]:eff_speed(m);
                calls[n++]=(S8AbilityCall){m,row,speed,0};
            }
        }
    }
    S8AbilityEvent e={event,source,target,mv>=0?move_def(mv):NULL,1,detail};
    s8_ability_run_queue(calls,n,&e,form_residual?BATTLE_RNG:S8_POWER_RNG);
}
static void s8_ability_phase(const Mon *source,const Mon *target,S8AbilityEventId event,int mv,int detail) {
    const MoveDef *move=mv>=0?move_def(mv):NULL;
    int item_source=event==S8AE_RESIDUAL && s8_items_implemented(s8_current_ability(source));
    int item_target=event==S8AE_RESIDUAL && target && s8_items_implemented(s8_current_ability(target));
    if(!item_source && !s8_has_ability(source,S8Q_OPPORTUNIST) && !(event==S8AE_RESIDUAL && (s8_weather_implemented(s8_current_ability(source)) || s8_current_ability(source)==S8F_HUNGERSWITCH)))
        s8_ability_event(source,event,source,target,move,1,detail);
    if(!item_target && target && target!=source && !s8_has_ability(target,S8Q_OPPORTUNIST) && !(event==S8AE_RESIDUAL && (s8_weather_implemented(s8_current_ability(target)) || s8_current_ability(target)==S8F_HUNGERSWITCH)))
        s8_ability_event(target,event,source,target,move,1,detail);
    // Opportunist's residual (29) is called from the ordered residual owner.
    if(event!=S8AE_RESIDUAL) s8_boost_phase(source,target,event,mv,detail);
}
static void s8_ability_new_state(Mon *m) {
    unsigned generation=m->ability_state.generation+1;
    m->ability_state=(S8AbilityEffectState){.id=s8_current_ability(m),.generation=generation};
}
static void s8_ability_start(Mon *m) {
    int id=s8_current_ability(m);
    if(id<0 || m->hp<=0 || !s8_has_ability(m,id)) return;
    s8_items_activate(m);
    m->ability_state.started=1;
    s8_ability_record(m,S8AE_START,S8A_INTERNAL);
    s8_ability_event(m,S8AE_START,m,m,NULL,1,0);
}
static void s8_ability_end(Mon *m) {
    // End is permitted even while suppressed (singleEvent's End exception).
    if(s8_current_ability(m)>=0) {
        s8_ability_record(m,S8AE_END,S8A_INTERNAL);
        // End callbacks bypass suppression, while retaining old ownership.
        int suppressed=m->ability_suppressed;m->ability_suppressed=0;
        s8_ability_event(m,S8AE_END,m,m,NULL,1,0);
        m->ability_suppressed=suppressed;
    }
    m->ability_state.started=0;
}
static void s8_ability_suppress(Mon *m,int suppressed) {
    if(!!m->ability_suppressed==!!suppressed || (s8_ability_flags(m)&S8TA_CANTSUPPRESS)) return;
    if(suppressed) s8_ability_end(m);
    m->ability_suppressed=!!suppressed;
    m->state_features|=MON_POST_V1_STATE;
    s8_ability_record(m,suppressed?S8AE_SUPPRESS:S8AE_RESTORE,S8A_INTERNAL);
    // Suppression preserves effectState and Pokemon flags; replacement resets
    // effectState. Restoration never synthesizes a SwitchIn.
    if(!suppressed) s8_ability_start(m);
}
static void s8_ability_battle_start(Mon *a,Mon *b) {
    Mon *ms[]={a,b};
    // Existing passive abilities have no cross-holder entry handlers. Keep
    // equal-speed seat order and zero extra draws at this compatibility boundary.
    double speed_a=eff_speed(a),speed_b=eff_speed(b);
    int first=speed_b>speed_a;
    // Only newly admitted field entry effects opt into the pinned random tie.
    // The old passive compatibility boundary consumes no additional RNG.
    if(speed_a==speed_b && BATTLE_RNG &&
       (s8_field_implemented(s8_current_ability(a)) || s8_field_implemented(s8_current_ability(b)) ||
        s8_form_implemented(s8_current_ability(a)) || s8_form_implemented(s8_current_ability(b)) ||
        s8_switch_implemented(s8_current_ability(a)) || s8_switch_implemented(s8_current_ability(b))))
        first=rng_int(BATTLE_RNG,2);
    s8_ability_new_state(a);s8_ability_new_state(b);
    s8_ability_event(a,S8AE_BEFORE_SWITCH_IN,a,a,NULL,1,0);
    s8_ability_event(b,S8AE_BEFORE_SWITCH_IN,b,b,NULL,1,0);
    for(int i=0;i<2;i++) {
        Mon *m=ms[first^i];
        s8_ability_record(m,S8AE_SWITCH_IN,S8A_INTERNAL);
        s8_ability_event(m,S8AE_SWITCH_IN,m,m,NULL,1,0);
        if(!m->transformed) s8_ability_start(m);
    }
    // Item onSwitchInPriority -1: Start after all ability entry callbacks.
    for(int i=0;i<2;i++) s8_seed_check(ms[first^i]);
    s8_ability_phase(a,b,S8AE_AFTER_SWITCH_IN,-1,0);
    s8_switch_traps(a);s8_switch_traps(b);
}

static int s8_field_weather(const Mon *m) {
    if(!field_member(m)) return m->weather;
    for(int i=0;i<2;i++) {
        const Mon *holder=FIELD.seat[i];
        if(holder && holder->hp>0 && !holder->ability_state.ending && s8_has_ability(holder,S8W_CLOUDNINE)) return WEATHER_NONE;
    }
    return FIELD.weather;
}
static int current_weather(const Mon *m) {
    int weather=s8_field_weather(m);
    // Move effects use Pokemon.effectiveWeather; ability callbacks use the
    // actual field weather. Mega Sol never changes Field.effectiveWeather.
    if(!S8_FIELD_ABILITY_DEPTH && S8_PROPERTY && s8_has_ability(S8_PROPERTY->user,S8W_MEGASOL)) weather=WEATHER_SUN;
    return s8_ability_event(m,S8AE_WEATHER,m,m,NULL,weather,0);
}
static int current_gravity(const Mon *m) { return field_member(m)?FIELD.gravity:0; }
// Suppression changes effects, never ownership (Trick, Acrobatics and Mega).
static int held_item(const Mon *m) {
    return s8_ability_event(m,S8AE_ITEM,m,m,NULL,field_member(m) && FIELD.magic_room?ITEM_NONE:m->item,0);
}
static int S8_YAWN_END; // Safeguard blocks Yawn on addition, not its sleep onEnd.
static const MoveDef *S8_STATUS_EFFECT;
static Mon *S8_EFFECT_SOURCE; // scoped by execute_move, including nested called moves
static void s8_item_enable(Mon *a,Mon *b);
static void field_bind(Mon *a, Mon *b) {
    FIELD=(BattleField){.seat={a,b},.party={{a},{b}},.party_count={1,1}};
    s8_switch_reset();
    S8_SPECIAL_ACTIVE=0;
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(FIELD.seat[j]->moves[i]>=MV_COUNT && s8_special_move(FIELD.seat[j]->moves[i])) S8_SPECIAL_ACTIVE=1;
    // Import only the explicit, legacy harness projection; normal init is neutral.
    FIELD.weather=a->weather?a->weather:b->weather;
    if(FIELD.weather) FIELD.weather_turns=5;
    a->weather=b->weather=FIELD.weather;
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(FIELD.seat[j]->moves[i]>=MV_FP) FIELD.track_attempts=1;
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(s8_trap_move(FIELD.seat[j]->moves[i])) FIELD.trap_context=1;
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(s8_control_move(FIELD.seat[j]->moves[i]) || s8_rampage_move(FIELD.seat[j]->moves[i])) FIELD.control_context=1;
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(s8_turn_move(FIELD.seat[j]->moves[i])) FIELD.turn_context=FIELD.control_context=1;
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(s8_stalling_move(FIELD.seat[j]->moves[i])) FIELD.guard_context=1;
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(s8_item_switch_move(FIELD.seat[j]->moves[i]) || s8_special_move(FIELD.seat[j]->moves[i])) s8_item_enable(a,b);
    s8_items_activate(a);s8_items_activate(b);
    if(s8_status_item(a->item) || s8_status_item(b->item) ||
       a->item==ITEM_EJECTBUTTON || a->item==ITEM_REDCARD || a->item==ITEM_SHEDSHELL ||
       b->item==ITEM_EJECTBUTTON || b->item==ITEM_REDCARD || b->item==ITEM_SHEDSHELL) s8_item_enable(a,b);
    // Activate before first-turn ordering, including item rounding without ranks.
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(s8_boost_move(FIELD.seat[j]->moves[i]) || s8_primary_status(FIELD.seat[j]->moves[i]) || s8_heal_move(FIELD.seat[j]->moves[i]) || s8_stateful_heal(FIELD.seat[j]->moves[i]) || s8_field_move(FIELD.seat[j]->moves[i]) || s8_trap_move(FIELD.seat[j]->moves[i]) || s8_control_move(FIELD.seat[j]->moves[i]) || s8_rampage_move(FIELD.seat[j]->moves[i]) || s8_protect_move(FIELD.seat[j]->moves[i]) || s8_turn_move(FIELD.seat[j]->moves[i]) || s8_type_ability_move(FIELD.seat[j]->moves[i]) || s8_item_switch_move(FIELD.seat[j]->moves[i]) || s8_special_move(FIELD.seat[j]->moves[i]) || s8_rapidspin_move(FIELD.seat[j]->moves[i])) a->boost_stats_active=b->boost_stats_active=1;
}
// Legacy-only Protect keeps its historical flag handling. New individual
// shields share protected_now, with their reviewed Status/protect-flag rules.
static int s8_is_protected(const Mon *foe,int mv) {
    if(!foe->protected_now || mv==S8P_M_FEINT || mv==S8U_M_PHANTOMFORCE) return 0;
    if(S8_PROPERTY && S8_PROPERTY->target==foe && S8_PROPERTY->move==mv &&
       (!foe->shield_now || ((S8_MOVES[mv].flags&S8_FLAG_PROTECT) &&
       (foe->shield_now!=S8P_KINGSSHIELD || move_def(mv)->cat!=CAT_STATUS)))) {
        if(!s8_ability_event(S8_PROPERTY->user,S8AE_HIT_PROTECT,S8_PROPERTY->user,foe,move_def(mv),1,0)) return 0;
    }
    if(!foe->shield_now) return 1;
    return (S8_MOVES[mv].flags&S8_FLAG_PROTECT) &&
        (foe->shield_now!=S8P_KINGSSHIELD || move_def(mv)->cat!=CAT_STATUS);
}
// Effective typing leaves permanent/form types intact, including a form change
// during Roost. A pure Flying user receives the common getTypes Normal fallback.
static int s8_effective_type_core(const Mon *m,int slot) {
    if(slot==2) return m->added_type?m->added_type-1:-1;
    int type=slot?m->type2:m->type1;
    if(!m->roost || type!=T_FLY) return type;
    if(!slot && (m->type2==-1 || m->type2==T_FLY)) return T_NORMAL;
    return -1;
}
static int s8_effective_type(const Mon *m,int slot) {
    return s8_ability_event(m,S8AE_TYPE,m,m,NULL,s8_effective_type_core(m,slot),slot);
}
static int is_grounded_core(const Mon *m) {
    if(current_gravity(m) || m->ingrain || m->control[S8C_SMACKDOWN] || held_item(m)==ITEM_IRONBALL) return 1;
    int flying=0,typeless=0;
    for(int i=0;i<3;i++) { flying|=s8_effective_type(m,i)==T_FLY;typeless|=s8_effective_type(m,i)==-2; }
    if((flying && !(m->roost && typeless)) || m->magnet_rise) return 0;
    if(!s8_property_ignored(m) && (s8_has_ability(m,S8D_LEVITATE) || s8_has_ability(m,S8D_EELEVATE))) return 0;
    return held_item(m)!=ITEM_BALLOON || m->item_used;
}
static int is_grounded(const Mon *m) {
    return s8_ability_event(m,S8AE_GROUNDED,m,m,NULL,is_grounded_core(m),0);
}
static int s8_status_prevented_core(const Mon *m,int sleep,int confusion) {
    if(sleep && field_member(m)) for(int i=0;i<2;i++)
        if(FIELD.seat[i]->hp>0 && FIELD.seat[i]->uproar) return 1;
    if(!S8_YAWN_END && m->safeguard && !(S8_PROPERTY && S8_PROPERTY->infiltrates) && S8_EFFECT_SOURCE && m!=S8_EFFECT_SOURCE) return 1;
    if(!field_member(m) || !is_grounded(m) || s8_semi(m)>=0) return 0;
    return FIELD.terrain==TERRAIN_MISTY || (sleep && !confusion && FIELD.terrain==TERRAIN_ELECTRIC);
}
static int s8_status_prevented(const Mon *m,int sleep,int confusion) {
    return s8_ability_event(m,S8AE_STATUS,S8_EFFECT_SOURCE,m,NULL,
        s8_status_prevented_core(m,sleep,confusion),sleep?1:confusion?2:0);
}
// Champions overrides getActionSpeed: negate Speed, without mainline 13-bit wrap.
static double eff_speed(const Mon *m);
static double s8_action_speed(const Mon *m) {
    if(!field_member(m) || !FIELD.trick_room) return eff_speed(m);
    return -s8_speed_stat(m);
}
static int field_v1_unsupported(const Mon *m) {
    for(int i=0;i<S8C_COUNT;i++) if(m->control[i]) return 1;
    return m->gem || m->shield_now || m->quick_guard || m->wide_guard || m->guard_stall || m->rampage_duration || m->uproar || m->throatchop || m->partial_trap || m->trapped || m->trapper || m->leech_seed || m->octolock || m->cursed ||
           m->spikes || m->toxic_spikes || m->sticky_web || m->gmax_steelsurge || m->aquaring || m->ingrain || m->roost || m->wish_pending || m->charge_move || m->beak_hot || m->turn_cancelled || m->ally_timer || m->ally_streak || m->action_move || m->used_mask || m->attempt_prev || m->attempt_this || m->recharge || m->charging || m->focusing || m->focus_lost || m->round_chained || m->future_turns ||
           m->aurora_veil || m->safeguard || m->tailwind || m->reflect || m->light_screen || m->stealth_rock || m->magnet_rise || m->endured_now ||
           (field_member(m) && (FIELD.weather || FIELD.gravity || FIELD.terrain || FIELD.trick_room || FIELD.magic_room || FIELD.wonder_room || FIELD.fairy_lock));
}

// New trap battles must end before another residual can reverse a decisive KO.
// The legacy matrix never owns these volatiles, so its end-turn path is frozen.
static int s8_trap_battle_ended(const Mon *a,const Mon *b) {
    return (a->hp<=0 || b->hp<=0) && ((field_member(a) && (FIELD.trap_context || FIELD.control_context)) || a->partial_trap || a->trapped || a->trapper ||
        a->leech_seed || a->octolock || a->cursed || b->partial_trap || b->trapped ||
        b->trapper || b->leech_seed || b->octolock || b->cursed);
}

static int g5_tracking(const Mon *m) {
    if(field_member(m) && FIELD.track_attempts) return 1;
    for(int i=0;i<m->nmoves;i++) if(m->moves[i]>=MV_FP) return 1;
    return 0;
}
static void attempt_result(Mon *m, int result) {
    if(!g5_tracking(m)) return;
    m->attempt_this=result; m->state_features|=MON_POST_V1_STATE;
}
static int tantrum_failed(const Mon *m) { return m->attempt_prev==ATTEMPT_FAIL || m->attempt_prev==ATTEMPT_SKIP_FAIL; }
static void queue_faint_from(Mon *m,int cause,const Mon *source,const MoveDef *move) {
    if(m->hp>0 || !field_member(m)) return;
    int seat=m==FIELD.seat[1];
    for(int i=0;i<FIELD.faint_count;i++) if(FIELD.faint_queue[i].seat==seat) return;
    if(FIELD.faint_count>=2) abort();
    // Attempt costs resolve before recipient faint; ordinary post-damage
    // recoil appends after it. This is a resolver queue, not a final-HP rule.
    if(cause==FAINT_ATTEMPT_COST && FIELD.faint_count) {
        FIELD.faint_queue[1]=FIELD.faint_queue[0]; FIELD.faint_queue[0]=(FaintEntry){seat,cause,source,move};
    } else FIELD.faint_queue[FIELD.faint_count]=(FaintEntry){seat,cause,source,move};
    FIELD.faint_count++;
    FIELD.total_fainted[seat]++;
    s8_form_faint(m);
}
static void queue_faint(Mon *m,int cause) { queue_faint_from(m,cause,NULL,NULL); }
static int ordered_faint_result(void) {
    s8_after_faint();
    return FIELD.faint_count?1-FIELD.faint_queue[0].seat:2;
}
static void g5_begin_turn(Mon *a,Mon *b) {
    if(field_member(a)) FIELD.turn++;
    Mon *ms[]={a,b};
    for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if(m->hp>0) m->active_turns++;
        if(g5_tracking(m)) { m->attempt_prev=m->attempt_this; m->attempt_this=ATTEMPT_NONE; }
        m->hurt_this_turn=m->stats_lowered_turn=m->stats_raised_turn=m->attacked_turn=m->attacked_positive=0;
        m->used_item_this_turn=0;
        m->counter_valid=m->coat_valid=0;
        m->focusing=m->focus_lost=m->round_chained=0;
        m->beak_hot=m->turn_cancelled=0;
        if(!m->charging) m->charge_move=0;
        FIELD.instructed[i]=0;
        FIELD.move_queued[i]=0; FIELD.committed[i]=-1; FIELD.committed_priority[i]=0; FIELD.acted[i]=FIELD.consumed[i]=0;
    }
}
static int s8_move_priority(const Mon *m,int mv) {
    if(field_member(m) && FIELD.move_queued[m==FIELD.seat[1]] && FIELD.committed[m==FIELD.seat[1]]==mv) return FIELD.committed_priority[m==FIELD.seat[1]];
    return s8_ability_event(m,S8AE_PRIORITY,m,NULL,move_def(mv),
        move_def(mv)->prio+(mv==S8U_M_GRASSYGLIDE && FIELD.terrain==TERRAIN_GRASSY && is_grounded(m)),0);
}
static void obs_item(const Mon *,int);
static void s8_commit_move(Mon *m,int mv) {
    int seat=m==FIELD.seat[1];
    s8_switch_traps(m);
    // Queue-time modification seam precedes post-Mega priority/speed.
    // Execution-time ModifyMove also runs for called moves and current types.
    if(mv>=0) s8_ability_phase(m,FIELD.seat[1-seat],S8AE_MODIFY_MOVE,mv,0);
    FIELD.move_queued[seat]=0;
    FIELD.committed_priority[seat]=mv>=0?s8_move_priority(m,mv):-1;
    FIELD.prankster_boosted[seat]=mv>=0 && move_def(mv)->cat==CAT_STATUS && s8_has_ability(m,S8V3_PRANKSTER);
    FIELD.fractional_priority[seat]=mv>=0?s8_ability_event(m,S8AE_FRACTIONAL_PRIORITY,m,NULL,move_def(mv),0,0):0;
    if((s8_has_ability(FIELD.seat[0],S8V3_QUICKDRAW) || s8_has_ability(FIELD.seat[1],S8V3_QUICKDRAW) || s8_has_ability(FIELD.seat[0],S8V3_STALL) || s8_has_ability(FIELD.seat[1],S8V3_STALL)) &&
       FIELD.fractional_priority[seat]<=0 && held_item(m)==ITEM_CLAW && BATTLE_RNG && rng_int(BATTLE_RNG,5)==0) {
        FIELD.fractional_priority[seat]=1;obs_item(m,0);
    }
    FIELD.move_queued[seat]=1;FIELD.committed[seat]=mv;
}
static int g5_priority(const Mon *m,int slot) {
    return m->recharge?0:slot>=0?s8_move_priority(m,m->moves[slot]):-1;
}
static int s8_charge_id(const Mon *m) { return m->charge_move?m->charge_move-1:MV_MB; }
static int s8_charge_slot(Mon *m) {
    int mv=s8_charge_id(m),slot=-1;
    for(int i=0;i<m->nmoves;i++) if(m->moves[i]==mv) slot=i;
    if(slot<0) { m->moves[4]=mv;m->pp[4]=0;slot=4; }
    return slot;
}
static int s8_charge_release(const Mon *m,int mv) { return m->charging && mv==s8_charge_id(m); }
static int s8_semi(const Mon *m) {
    const S8TurnMove *row=m->charging?s8_turn_move(s8_charge_id(m)):NULL;
    return row && row->semi?s8_charge_id(m):-1;
}
static int s8_semi_multiplier(const Mon *m,int mv,int phase) {
    int charge=s8_semi(m);
    if(charge<0) return 4096;
    for(size_t i=0;i<sizeof(S8_SEMI_EXCEPTIONS)/sizeof(*S8_SEMI_EXCEPTIONS);i++)
        if(S8_SEMI_EXCEPTIONS[i].charge==charge && S8_SEMI_EXCEPTIONS[i].attack==mv && S8_SEMI_EXCEPTIONS[i].phase==phase) return 8192;
    return 4096;
}
static int s8_cancel_air_charge(Mon *m) {
    int mv=s8_semi(m);
    if(mv!=S8U_M_FLY && mv!=S8U_M_BOUNCE) return 0;
    m->charging=m->charge_move=0;
    if(field_member(m) && !FIELD.consumed[m==FIELD.seat[1]]) m->turn_cancelled=1;
    return 1;
}
static void s8_spend_pp(Mon *m,int slot) {
    m->pp[slot]--;
    if(FIELD.turn_context) { m->used_mask|=1<<slot; m->action_move=m->moves[slot]+1; }
}

static int g5_sleep_callable(int mv) {
    return mv!=MV_FP && mv!=MV_MB && (mv<MV_COUNT || !(S8_MOVES[mv].flags&S8_FLAG_NOSLEEPTALK));
}
static void future_residual(Mon *a,Mon *b,Rng *r);
// BATTLE_RNG is declared with the ability lifecycle entry hooks.

static double smult(int s) { return s >= 0 ? (2.0 + s) / 2.0 : 2.0 / (2.0 - s); }
static double acc_mult(int s) { return s >= 0 ? (3.0 + s) / 3.0 : 3.0 / (3.0 - s); }
static int clamp_stage(int v) { return v < -6 ? -6 : (v > 6 ? 6 : v); }

static int find_move(const Mon *m, int mv) {
    for (int i = 0; i < m->nmoves; i++) if (m->moves[i] == mv) return i;
    return -1;
}
static int pp_of(const Mon *m, int mv) { int i = find_move(m, mv); return i < 0 ? 0 : m->pp[i]; }

// ---------- metagross_observation_v1 / rule metadata v1 ----------
// Public metadata: species-data/species_dataset.json, metagross[mega].baseStats.
// No pointers, native struct bytes or battle RNG cross the policy boundary.
static const int *const OBS_BASE[2] = {S8_SPECIES[S8_METAGROSS].base_stats,
                                       S8_SPECIES[S8_METAGROSS_MEGA].base_stats};
enum { OBS_REGULAR=1, OBS_COMPAT=2, AB_TC=3 };
enum { EV_START, EV_BEGIN, EV_OPPORTUNITY, EV_USE, EV_NOTE, EV_EXEC,
       EV_HP, EV_HIT, EV_STAGE, EV_COPY, EV_HERB, EV_SLEEP, EV_BLOCK,
       EV_SUB, EV_PROTECT, EV_ITEM, EV_TRANSFER, EV_ABILITY, EV_LEPPA,
       EV_END, EV_TERMINAL, EV_FAINT, EV_RULES, EV_PREPARE, EV_RECHARGE,
       EV_STATUS_START, EV_STATUS_END, EV_CONFUSION_START, EV_CONFUSION_END,
       EV_WEATHER_START, EV_WEATHER_END, EV_TERRAIN_START, EV_TERRAIN_END,
       EV_SCREEN_START, EV_SCREEN_END, EV_ROCKS, EV_GRAVITY_START, EV_GRAVITY_END,
       EV_RISE_START, EV_RISE_END, EV_FUTURE_SET, EV_FUTURE_IMPACT,
       EV_CHARGE_START, EV_CHARGE_END, EV_RECHARGE_START,
       EV_TOXIC_TICK, EV_BLOCK_TICK, EV_SLEEP_TICK, EV_SKIP,
       EV_ATTACK_START, EV_ATTACK_END, EV_OWN_MAXHP, EV_FUTURE_TICK, EV_MEGA,
       EV3_IDENTITY, EV3_TYPES, EV3_ABILITY, EV3_ITEM, EV3_REVEAL, EV3_TRANSFORM,
       EV3_VOLATILE, EV3_DURATION, EV3_HAZARD, EV3_RANK, EV3_HIT_BEGIN, EV3_COURT, EV3_PP, EV3_OWN_STAT, EV3_OWN_CONTEXT, EV3_OWN_SPEED };
enum { HIT_FAIL, HIT_MISS, HIT_PROTECT, HIT_SUB, HIT_CRIT, HIT_IMMUNE, HIT_EFFECT };
enum { HP_INITIAL, HP_HIT, HP_HEAL, HP_RECOIL, HP_COST };
enum { ACT_NONE, ACT_USED, ACT_SLEEP, ACT_FLINCH, ACT_SKIPPED };
// Every Event is already actor-relative and sanitized. a=0 self, 1 foe.
// x/y/z are kind-specific public values, never raw damage or hidden slot IDs.
typedef struct { int seq, turn, kind, a, x, y, z; } ObsEvent;
typedef struct { int valid, id, uses, spent, called, pp_valid, lo, hi; } Reveal;
typedef struct {
    int mega, type1, type2, hp, faint, stage[6], sleep, block, sub, protect, protected_now;
    int sleep_set, block_set;
    Reveal moves[4];
    int item_known, item, item_state, abilities, ability_known, ability;
    MoveHypotheses lock_set;
    int last, streak;
} PublicMon;
typedef struct { int move_valid, move, call_valid, call, outcome, crit; } Activity;
typedef struct { int start, end, loss, gain, present; } HPSummary;
typedef struct {
    int order, nominal_valid[2], nominal[2], relation, claw[2], qualified;
    Activity activity[2]; HPSummary hp[2];
    int opportunities, first;
} TurnSummary;
typedef struct {
    PublicMon side[2]; TurnSummary current, previous;
    int turn, profile, terminal, counts[3];
} Knowledge;
// The fixed vector is assembled explicitly below, in spec section 5 order.
// v1 offsets: context 0, identities 3/13, own numeric 23, own moves 48,
// foe visible 64, durations 77, reveals 79, item 111, ability 114,
// lock 117, repetition 118, substitute belief 120, stat bounds 124,
// order 142, totals 151, activities 154/160, HP summaries 166/171,
// mask 176, reserved zero 181..255. Enum IDs are those declared in this file.
// Move/lock "none" uses 0, moves use id+1 in last/lock fields; valid-tagged
// records use raw move IDs. Item-state 0/1/2 = unknown/unused/used.
// Signed PP replay intentionally preserves legacy fast/stall overspending.
#define OBS_WORDS 256
#define OBS_BYTES (8 + 4 * OBS_WORDS)
typedef struct { int32_t v[OBS_WORDS]; } Observation;
typedef struct { unsigned char legal[5]; } ActionMask;
// v2 public result classes. UNKNOWN is deliberately distinct from NONE.
enum { PUBLIC_NONE, PUBLIC_SUCCESS, PUBLIC_FAIL, PUBLIC_NOT_FAIL,
       PUBLIC_SKIP_FAIL, PUBLIC_SKIP_NOT_FAIL, PUBLIC_UNKNOWN };
enum { SKIP_FREEZE, SKIP_FLINCH, SKIP_CONFUSION, SKIP_PARALYSIS,
       SKIP_RECHARGE, SKIP_FOCUS, SKIP_UNANNOUNCED };
enum { GROUND_UNKNOWN, GROUND_YES, GROUND_NO };
typedef struct {
    int hits, lo, hi, crits, sub_hits, sub_broke;
    int clean_valid, clean_lo, clean_hi, clean_hits;
} SeenDamage;
typedef struct {
    Knowledge base;
    int status[2], status_since[2], toxic[2], confused[2], confusion_since[2];
    int weather, weather_turns, terrain, terrain_turns, gravity;
    int reflect[2], light_screen[2], rocks[2], rise[2], future[2];
    int charge[2], recharge[2], recharge_move[2], attempt[2], previous_attempt[2];
    int attack_active, attacker, attack_move, attack_crit, own_maxhp, attack_future, saved_attempt;
    int result_valid, result, cap;
    SeenDamage seen[2][4];
} Knowledge2;
#define OBS2_WORDS 512
#define OBS2_BYTES (8 + 4 * OBS2_WORDS)
typedef struct { int32_t v[OBS2_WORDS]; } Observation2;
// ---------- Stage 8e-2a: MGO3 (versioned table IDs, not legacy enums) ----------
#define OBS3_WORDS 1024
#define OBS3_BYTES (8+4*OBS3_WORDS)
// Only Copycat/Sleep Talk/Instruct/Transform can introduce a called/copied move
// in the admitted 1on1 closure. They draw from the two initial four-slot sets:
// at most eight distinct moves per actor. No historical record is evicted.
#define OBS3_RECORDS 8
// Public volatile values: no random remaining timers or hidden item locks.
enum { O3_STOCK=S8C_COUNT, O3_POWERTRICK, O3_NORETREAT, O3_RING, O3_INGRAIN,
       O3_ROOST, O3_PARTIAL, O3_TRAPPED, O3_SEED, O3_OCTO, O3_CURSE, O3_WISH,
       O3_RAMPAGE, O3_UPROAR, O3_THROAT, O3_BLOCK, O3_SUB, O3_PROTECT,
       O3_SHIELD, O3_QUICK, O3_WIDE, O3_RISE, O3_CHARGE, O3_RECHARGE,
       O3_FOCUS, O3_FLASH, O3_VOL_COUNT };
enum { O3_WEATHER, O3_TERRAIN, O3_GRAVITY, O3_TRICK, O3_MAGIC, O3_WONDER,
       O3_FAIRY, O3_SIDE, O3_WISH_DUR=O3_SIDE+2*7, O3_DUR_COUNT=O3_WISH_DUR+2 };
// mode: 0 absent, 1 exact remaining, 2 possible remaining endpoints (5/8),
// 3 elapsed only. Bounds are not a continuous interval in mode 2.
typedef struct { int active, mode, since, lo, hi, elapsed; } Obs3Duration;
typedef struct {
    int valid, id, uses, spent, called, forced, cancelled, pp_valid, lo, hi;
    SeenDamage damage;
    int before_lo, before_hi, after_lo, after_hi, endpoints, generation;
} Obs3Record;
typedef struct {
    int species, form, types[3], mega, transformed, transform_from;
    int hp, faint, rank[7], status, status_since, toxic, confused, confusion_since;
    int vol[O3_VOL_COUNT], since[O3_VOL_COUNT], hazard[5];
    int item_known, item, item_state, item_reveal, item_use;
    int ability_known, ability, suppressed, changed, generation;
    int protect_streak, last, streak, attempt, previous_attempt, disabled_move, encored_move;
    Obs3Record records[OBS3_RECORDS];
} Obs3Mon;
// Stage 8e-2b: public-history hypotheses. Each bit is an allocation/nature
// parameter, not a hidden stat. Per-stat projections deliberately retain
// correlations we cannot disprove; form changes reuse the surviving parameters.
typedef struct {
    int valid, form, population, item_id[88], source[6];
    uint32_t abilities[7], items[3];
    uint64_t params[6][3]; // nature - / neutral / +, points 0..32
    int replace[6], lo[6], hi[6];
} Obs3Belief;
typedef struct {
    int stats[6], ability, suppressed, item, used, speed, valid;
} Obs3Private;
typedef struct {
    Obs3Mon side[2]; Obs3Duration duration[O3_DUR_COUNT];
    Obs3Belief belief[2]; Obs3Private own;
    uint64_t speed_possible[88][3];
    int speed_valid, speed_form, end_healed[2], type_uncertain[2];
    TurnSummary current, previous;
    int turn, profile, terminal, counts[3], result_valid, result, cap, own_maxhp, weather, terrain;
    // Nested Copycat/reflection contexts have independent direct-hit ownership.
    struct { int actor, move, crit, future, saved_attempt, body, landed, recoil, context_changed; } attack[16];
    int depth;
} Knowledge3;
typedef struct { int32_t v[OBS3_WORDS]; } Observation3;
enum { O3_CONTEXT=0, O3_SELF_ID=16, O3_SELF_NUM=48, O3_FOE_ID=192,
       O3_FOE_VISIBLE=224, O3_FIELD=352, O3_RECORDS=480,
       O3_CONFIRMED=896, O3_BELIEF=912, O3_SUMMARY=960 };

typedef struct {
    Mon *seat[2]; Knowledge view[2]; int seq, turn, profile;
    int version; Knowledge2 v2[2]; Knowledge3 v3[2];
    int published_types[2][3], published[2][O3_VOL_COUNT], published_rank[2][7], published_hazard[2][5], published_field[O3_DUR_COUNT];
} Observer;
// Like CUR_SECOND, the adapter is scoped to one active battle (not reentrant).
static Observer OBS_STORAGE;
static Observer *OBS;
static int OBS_FORCE = -1, OBS_PROFILE = 0;
static unsigned long obs_fallbacks;
// Optional sanitized-event sink, used by replay tests; never used by a policy.
static void (*obs_sink)(int viewer, const ObsEvent *event);
static int obs_clamp_hp(int hp, int maxhp) { return hp < 0 ? 0 : hp > maxhp ? maxhp : hp; }
static int obs_percent(int hp, int maxhp) {
    return (int)((200LL * obs_clamp_hp(hp,maxhp) + maxhp) / (2LL * maxhp));
}
static int obs_dec_set(int s) { return (s & 1) | (s >> 1); }
static Reveal *obs_reveal(PublicMon *p, int mv) {
    for (int i=0;i<4;i++) if (p->moves[i].valid && p->moves[i].id==mv) return &p->moves[i];
    for (int i=0;i<4;i++) if (!p->moves[i].valid) {
        Reveal *r=&p->moves[i]; r->valid=1; r->id=mv; r->pp_valid=1;
        r->lo=r->hi=MOVES[mv].pp; return r;
    }
    fprintf(stderr,"observer: more than four distinct moves\n"); abort();
}
static void obs_replay(Knowledge *k, const ObsEvent *e) {
    PublicMon *p=&k->side[e->a]; TurnSummary *t=&k->current;
    switch(e->kind) {
    case EV_RULES: k->profile=e->x; k->turn=e->turn; break;
    case EV_START:
        memset(p,0,sizeof(*p)); p->mega=e->x; p->type1=e->y; p->type2=e->z;
        p->sleep_set=p->block_set=1; p->lock_set=lock_single(-1); p->last=-1;
        p->abilities=p->mega ? 1<<AB_TC : (1<<AB_CB)|(1<<AB_LM)|(k->profile==OBS_COMPAT ? 1<<AB_NONE : 0);
        break;
    case EV_MEGA:
        p->mega=1; p->abilities=1<<AB_TC; p->ability_known=1; p->ability=AB_TC;
        break;
    case EV_BEGIN:
        k->turn=e->turn; memset(t,0,sizeof(*t));
        for(int i=0;i<2;i++) { t->hp[i].start=t->hp[i].end=k->side[i].hp; t->activity[i].outcome=ACT_SKIPPED; }
        break;
    case EV_OPPORTUNITY:
        if (!t->opportunities) t->first=e->a;
        t->opportunities++; t->activity[e->a].outcome=e->x;
        if(e->x!=ACT_FLINCH) {
            p->block_set=obs_dec_set(p->block_set); p->block=p->block_set!=1;
            if(e->x==ACT_SLEEP) { p->sleep_set=obs_dec_set(p->sleep_set); p->sleep=p->sleep_set!=1; }
        }
        break;
    case EV_USE:
        t->activity[e->a].move_valid=1; t->activity[e->a].move=e->x;
        if(e->y==1) { t->activity[e->a].call_valid=1; t->activity[e->a].call=e->x; }
        if(e->y==0 && t->activity[e->a].outcome==ACT_USED) { t->nominal_valid[e->a]=1; t->nominal[e->a]=e->x; }
        if(e->x<V1_MOVE_COUNT) {
            Reveal *r=obs_reveal(p,e->x); r->uses++;
            if(e->y==1) r->called++; else { r->spent++; r->lo--; r->hi--; }
        }
        break;
    case EV_NOTE:
        if(e->x==V1_STRUGGLE_WIRE) { p->last=-1; p->streak=0; break; }
        p->streak=p->last==e->x ? (p->streak<5?p->streak+1:5) : 1; p->last=e->x;
        if(p->item_known) { if(p->item==ITEM_SCARF) p->lock_set=lock_single(e->x); }
        else p->lock_set=lock_add(p->lock_set,e->x);
        break;
    case EV_EXEC: if(e->x!=MV_PRO) p->protect=0; break;
    case EV_HP:
        p->hp=e->y;
        if(e->z) {
            HPSummary *h=&t->hp[e->a]; h->end=e->y; h->present=1;
            if(e->x>e->y) h->loss+=e->x-e->y; else h->gain+=e->y-e->x;
        }
        break;
    case EV_FAINT: p->faint=1; break;
    case EV_HIT: if(e->x==HIT_CRIT) t->activity[e->a].crit=1; break;
    case EV_STAGE: p->stage[e->x]=clamp_stage(p->stage[e->x]+e->y); break;
    case EV_COPY: memcpy(p->stage,k->side[1-e->a].stage,sizeof(p->stage)); break;
    case EV_HERB: for(int i=0;i<6;i++) if(p->stage[i]<0) p->stage[i]=0; break;
    case EV_SLEEP: p->sleep=e->x; p->sleep_set=e->x?(e->y?e->y:4):1; break;
    case EV_BLOCK: p->block=e->x; p->block_set=e->x?4:1; break;
    case EV_SUB: p->sub=e->x; break;
    case EV_PROTECT: p->protected_now=e->x; p->protect=e->x?p->protect+1:0; break;
    case EV_ITEM:
        p->item_known=1; p->item=e->x; p->item_state=e->y;
        if(e->x==ITEM_CLAW) t->claw[e->a]=1;
        break;
    case EV_TRANSFER:
        p->item_known=1; p->item=e->x; p->item_state=e->y;
        if(e->x!=ITEM_SCARF) p->lock_set=lock_single(-1);
        break;
    case EV_ABILITY: p->abilities=1<<e->x; p->ability_known=1; p->ability=e->x; break;
    case EV_LEPPA: {
        Reveal *r=obs_reveal(p,e->x); r->lo+=10; r->hi+=10;
        if(r->lo>MOVES[e->x].pp) r->lo=MOVES[e->x].pp;
        if(r->hi>MOVES[e->x].pp) r->hi=MOVES[e->x].pp;
        break; }
    case EV_END:
        for(int i=0;i<2;i++) k->side[i].protected_now=0;
        t->order=t->opportunities==2 ? (t->first==0?1:2) : 3;
        if(t->nominal_valid[0] && t->nominal_valid[1]) {
            int a=t->nominal[0]==V1_STRUGGLE_WIRE?-1:MOVES[t->nominal[0]].prio;
            int b=t->nominal[1]==V1_STRUGGLE_WIRE?-1:MOVES[t->nominal[1]].prio;
            t->relation=a==b?1:a>b?2:3;
        }
        t->qualified=t->opportunities==2 && t->relation==1 && !t->claw[0] && !t->claw[1];
        k->counts[t->qualified ? t->first : 2]++;
        k->previous=*t; break;
    case EV_TERMINAL: k->terminal=1; break;
    }
}
static void obs2_replay(Knowledge2 *k, const ObsEvent *e);
static void obs3_replay(Knowledge3 *,const ObsEvent *);
static void obs3_publish(void);
static void obs3_private(void);
static int s8_item_table(const Mon *);
static void obs_deliver(int viewer, int kind, int seat, int x, int y, int z) {
    ObsEvent e={OBS->seq,OBS->turn,kind,seat==viewer?0:1,x,y,z};
    if(OBS->version==3) obs3_replay(&OBS->v3[viewer],&e);
    else if(OBS->version==2) obs2_replay(&OBS->v2[viewer],&e);
    else obs_replay(&OBS->view[viewer],&e);
    if(obs_sink) obs_sink(viewer,&e);
}
static void obs_emit(int kind, const Mon *m, int x, int y, int z) {
#ifdef ENGINE_EVENT_SINK
    ENGINE_EVENT_SINK(kind,m,x,y,z);
#endif
    if(!OBS) return;
    if(OBS->version==3 && (kind==EV_ITEM || kind==EV_TRANSFER)) {
        z=kind==EV_ITEM && (y==2 || x!=ITEM_BALLOON);kind=EV3_ITEM;x=x==ITEM_NONE?-1:s8_item_table(m);
    }
    if(OBS->version==3 && (kind==EV_OPPORTUNITY || kind==EV_ATTACK_START)) obs3_private();
    int seat=m==OBS->seat[1]; ++OBS->seq;
    for(int v=0;v<2;v++) obs_deliver(v,kind,seat,x,y,z);
}
static void obs_emit_action(int kind, const Mon *m, MoveAction action, int called) {
#ifdef ENGINE_EVENT_SINK
    if(!OBS) { obs_emit(kind,m,action_wire_id(action,2),called,0); return; }
#endif
    if(!OBS) return;
    int wire=action_wire_id(action,OBS->version==3?3:OBS->version==2?2:1);
    if(OBS->version>=2 && kind==EV_USE && action.kind==ACTION_MOVE && (m->charging || (OBS->version==3 && (m->rampage_duration || m->uproar)))) called=3;
    if(wire<0) { fprintf(stderr,"MGO1/g12 rejects post-v1 action\n"); exit(2); }
    obs_emit(kind,m,wire,called,0);
}
// Sanitize at source, before creating either public event. hp before/after are
// local resolution values, never retained by a shared event object.
static void obs_hp(Mon *m, int before, int cause) {
    // spreadDamage marks real damage, including recoil/confusion. Direct
    // HP costs (Substitute, self KO) do not set hurtThisTurn in the mod.
    if(m->hp<before && (cause==HP_HIT || cause==HP_RECOIL)) m->hurt_this_turn=m->hp>0;
#ifdef ENGINE_EVENT_SINK
    if(!OBS) {
        obs_emit(EV_HP,m,obs_clamp_hp(before,m->maxhp),obs_clamp_hp(m->hp,m->maxhp),cause);
        if(m->hp<=0 && before>0) obs_emit(EV_FAINT,m,0,0,0);
        return;
    }
#endif
    if(!OBS) return;
    int seat=m==OBS->seat[1]; ++OBS->seq;
    for(int v=0;v<2;v++) obs_deliver(v,EV_HP,seat,
        v==seat?obs_clamp_hp(before,m->maxhp):obs_percent(before,m->maxhp),
        v==seat?obs_clamp_hp(m->hp,m->maxhp):obs_percent(m->hp,m->maxhp),cause);
    if(m->hp<=0 && before>0) obs_emit(EV_FAINT,m,0,0,0);
}
// These events are completely inert without a v2 attachment.
static void obs2_emit(int kind, const Mon *m, int x, int y, int z) {
#ifdef ENGINE_STATE_EVENT_SINK
    ENGINE_STATE_EVENT_SINK(kind,m,x,y,z);
#endif
    if(OBS && OBS->version>=2) obs_emit(kind,m,x,y,z);
}
static void obs2_cancel_charge(const Mon *m) {
    // A known charge ending is visible; no new submitted choice is disclosed.
    obs2_emit(EV_CHARGE_END,m,0,0,0);
}
static void obs_item(const Mon *m, int consumed) { obs_emit(EV_ITEM,m,m->item,consumed?2:1,0); }
static void obs_mask(const Mon *m, int terminal, ActionMask *mask) {
    memset(mask,0,sizeof(*mask)); if(terminal) return;
    if(held_item(m)==ITEM_SCARF && m->choice_lock>=0) {
        int i=find_move(m,m->choice_lock);
        if(i>=0 && m->pp[i]>0) { mask->legal[i]=1; return; }
    }
    int any=0;
    for(int i=0;i<m->nmoves;i++) if(m->pp[i]>0) any=mask->legal[i]=1;
    mask->legal[4]=!any;
}
static int mon_v1_supported(const Mon *m) {
    if(!s8_old_observation_scope(m)) return 0;
    if(m->mega_mode==2 || field_v1_unsupported(m) || m->state_features || m->status || m->freeze_turns || m->toxic_stage || m->confusion || m->weather || m->nmoves<1 || m->nmoves>4 ||
       m->choice_lock < -1 || m->choice_lock>=V1_MOVE_COUNT ||
       m->last_move < -1 || m->last_move>=V1_MOVE_COUNT) return 0;
    for(int i=0;i<m->nmoves;i++) if(m->moves[i]<0 || m->moves[i]>=V1_MOVE_COUNT) return 0;
    return 1;
}
static void obs_require_v1(const Mon *self, const Knowledge *k) {
    if(!mon_v1_supported(self)) goto unsupported;
    for(int a=0;a<2;a++) {
        const PublicMon *p=&k->side[a];
        if(!lock_v1(p->lock_set) || p->last < -1 || p->last>=V1_MOVE_COUNT) goto unsupported;
        for(int i=0;i<4;i++) if(p->moves[i].valid && (p->moves[i].id<0 || p->moves[i].id>=V1_MOVE_COUNT)) goto unsupported;
    }
    return;
unsupported:
    fprintf(stderr,"MGO1/g12 rejects post-v1 move vocabulary or state\n"); exit(2);
}
static void obs_build(const Mon *self, const Knowledge *k, Observation *o, ActionMask *mask) {
    obs_require_v1(self,k);
    memset(o,0,sizeof(*o)); int n=0;
#define PUT(x) do { if(n>=OBS_WORDS) abort(); o->v[n++]=(x); } while(0)
    PUT(k->turn); PUT(k->profile); PUT(k->terminal);
    for(int a=0;a<2;a++) { const PublicMon *p=&k->side[a];
        PUT(376); PUT(p->mega); PUT(p->type1); PUT(p->type2);
        for(int i=0;i<6;i++) PUT(OBS_BASE[p->mega][i]);
    }
    PUT(obs_clamp_hp(self->hp,self->maxhp)); PUT(self->maxhp);
    for(int i=0;i<6;i++) PUT(self->stats[i]);
    for(int i=0;i<5;i++) PUT(self->st[i]);
    PUT(self->acc);
    PUT(self->sleep); PUT(self->healblock); PUT(self->sub_hp); PUT(self->protect_streak); PUT(self->protected_now);
    PUT(self->item); PUT(self->item_used); PUT(self->mega?AB_TC:self->ability);
    PUT(self->choice_lock+1); PUT(self->last_move+1); PUT(self->metro_streak);
    for(int i=0;i<4;i++) { int yes=i<self->nmoves; PUT(yes); PUT(yes?self->moves[i]:0); PUT(yes?self->pp[i]:0); PUT(yes?MOVES[self->moves[i]].pp:0); }
    const PublicMon *p=&k->side[1];
    PUT(p->hp); PUT(p->faint); for(int i=0;i<6;i++) PUT(p->stage[i]);
    PUT(p->sleep); PUT(p->block); PUT(p->sub); PUT(p->protect); PUT(p->protected_now);
    PUT(p->sleep_set); PUT(p->block_set);
    for(int i=0;i<4;i++) { const Reveal *r=&p->moves[i];
        PUT(r->valid); PUT(r->id); PUT(r->uses); PUT(r->spent); PUT(r->called); PUT(r->pp_valid); PUT(r->lo); PUT(r->hi);
    }
    PUT(p->item_known); PUT(p->item); PUT(p->item_state);
    PUT(p->abilities); PUT(p->ability_known); PUT(p->ability); PUT((int32_t)p->lock_set.word[0]);
    PUT(p->last+1); PUT(p->streak);
    PUT(p->sub); PUT(!p->sub); PUT(0); PUT(0);
    for(int i=0;i<18;i++) PUT(0);
    const TurnSummary *t=&k->previous;
    PUT(t->order); for(int i=0;i<2;i++) { PUT(t->nominal_valid[i]); PUT(t->nominal[i]); }
    PUT(t->relation); PUT(t->claw[0]); PUT(t->claw[1]); PUT(t->qualified);
    for(int i=0;i<3;i++) PUT(k->counts[i]);
    for(int i=0;i<2;i++) { const Activity *a=&t->activity[i];
        PUT(a->move_valid); PUT(a->move); PUT(a->call_valid); PUT(a->call); PUT(a->outcome); PUT(a->crit);
    }
    for(int i=0;i<2;i++) { const HPSummary *h=&t->hp[i]; PUT(h->start); PUT(h->end); PUT(h->loss); PUT(h->gain); PUT(h->present); }
    obs_mask(self,k->terminal,mask); for(int i=0;i<5;i++) PUT(mask->legal[i]);
#undef PUT
}
static inline void obs_serialize(const Observation *o, unsigned char bytes[OBS_BYTES]) {
    // Transport: ASCII MGO1, u32 word count, 256 signed two's-complement LE32.
    memcpy(bytes,"MGO1",4);
    for(int j=0;j<4;j++) bytes[4+j]=(unsigned char)((uint32_t)OBS_WORDS>>(8*j));
    for(int i=0;i<OBS_WORDS;i++) for(int j=0;j<4;j++) bytes[8+4*i+j]=(unsigned char)((uint32_t)o->v[i]>>(8*j));
}
static int obsdemo(const Observation *o, const ActionMask *mask) {
    (void)o; // Deliberately minimal deterministic boundary pilot.
    for(int i=0;i<5;i++) if(mask->legal[i]) return i;
    return -1;
}
static int obsh2(const Observation *o, const ActionMask *mask);
static int learned(const Observation *o, const ActionMask *mask);
typedef int (*ObservablePolicy)(const Observation *, const ActionMask *);
static ObservablePolicy observable_policy = obsdemo;
// Offline collection may supply a sampler with an independent policy stream.
// Its interface is identical to the deterministic deployment policy.
static ObservablePolicy learned_policy = learned;

static int obs_checked_action(int action, const ActionMask *mask) {
    if(action>=0 && action<5 && mask->legal[action]) return action==4?-1:action;
    obs_fallbacks++;
    fprintf(stderr,"observable policy invalid action; deterministic fallback (%lu)\n",obs_fallbacks);
    for(int i=0;i<4;i++) if(mask->legal[i]) return i;
    return -1;
}
static int obs_valid_build(const Mon *m, int profile) {
    if(!mon_v1_supported(m)) return 0;
    if(m->mega<0 || m->mega>1 || m->maxhp<=0 || m->nmoves<1 || m->nmoves>4) return 0;
    if(profile==OBS_REGULAR && (m->nmoves!=4 || (!m->mega && m->ability!=AB_CB && m->ability!=AB_LM))) return 0;
    for(int i=0;i<m->nmoves;i++) {
        if(m->moves[i]<0 || m->moves[i]>=MV_COUNT || m->pp[i]!=MOVES[m->moves[i]].pp) return 0;
        for(int j=0;j<i;j++) if(m->moves[j]==m->moves[i]) return 0;
    }
    for(int i=0;i<6;i++) if(m->stats[i]<=0) return 0;
    if(profile==OBS_REGULAR) {
        // Existing project allocation model (run1/gen_stage1.py): 0..32
        // per stat, at most 66 total, 21 nature effects, level 50 core.
        int legal=0;
        for(int up=1;up<=5;up++) for(int down=1;down<=5;down++) {
            int total=0, possible=1;
            for(int i=0;i<6;i++) {
                int core=OBS_BASE[m->mega][i]+(i==0?75:20), least=33;
                for(int points=0;points<=32;points++) {
                    int value=core+points;
                    if(up!=down && i==up) value=(int)floor(value*1.1);
                    if(up!=down && i==down) value=(int)floor(value*0.9);
                    if(value==m->stats[i]) { least=points; break; }
                }
                if(least==33) possible=0;
                total+=least;
            }
            if(possible && total<=66) legal=1;
        }
        if(!legal) return 0;
    }
    return 1;
}
static void obs_attach(Mon *a, Mon *b, int profile) {
    if(!mon_v1_supported(a) || !mon_v1_supported(b)) {
        fprintf(stderr,"MGO1/g12 rejects post-v1 move vocabulary or state\n"); exit(2);
    }
    memset(&OBS_STORAGE,0,sizeof(OBS_STORAGE)); OBS=&OBS_STORAGE;
    OBS->seat[0]=a; OBS->seat[1]=b; OBS->profile=profile;
    OBS->turn=1;
    obs_emit(EV_RULES,a,profile,0,0);
    for(int i=0;i<2;i++) { Mon *m=OBS->seat[i];
        obs_emit(EV_START,m,m->mega,m->type1,m->type2); obs_hp(m,m->hp,0);
        if(m->item==ITEM_BALLOON && !m->item_used) obs_item(m,0);
    }
    OBS->turn=1;
    obs_emit(EV_BEGIN,a,0,0,0);
}
static int obs_finish(int result, int cap) {
    if(OBS) {
        ++OBS->seq;
        for(int v=0;v<2;v++) obs_deliver(v,EV_TERMINAL,v,result==2?2:result==v?0:1,cap,0);
    }
    OBS=NULL; return result;
}

// ---------- metagross_observation_v2 / MGO2 ----------
// 2056 bytes: "MGO2", LE32 word count 512, then signed LE32 words.
// 0..180: v1 field order and enums, now 68-move IDs + Struggle=68;
// word 117 is reserved zero (wide lock is below). This is a NEW schema.
// 181: vocabulary=68. 182..188 own exact status/freeze/toxic/confusion/
// attempt_prev/recharge/charging. 189..194 foe status/elapsed/toxic/
// freeze-elapsed/confused/confusion-elapsed. 195..201 weather/kind remaining,
// terrain/kind remaining-valid remaining, Gravity remaining active.
// 202..205 self Reflect/Light Screen/rocks/Magnet Rise; 206..209 foe.
// 210 foe grounded: 0 unknown,1 grounded,2 floating (public facts only).
// 211..214 Future Sight against-self flag/countdown, against-foe flag/countdown.
// 215..217 foe charge flag/move-valid/move-ID; 218..220 recharge likewise.
// 221 public previous attempt (PUBLIC_*). 222..225 lock: four LE32 limbs,
// bit0 none, bit(move+1) move, unused bits 69..127 zero.
// 226..321: self then foe, four revelation-order records of 12 words:
// valid,move,hits,min,max,crits,sub-hits,sub-broke,clean-valid,min,max,count.
// Loss unit=0.01 percent: foe bar endpoint difference *100; own exact loss
// floor(10000*(before-after)/ownMaxHP). hits=0 invalidates all-hit min/max.
// 322..324 terminal result-valid/result(0 win,1 loss,2 draw)/reason(0 faint,1 cap).
// 325..511 reserved zero. Unknown tagged payloads zero; active duration 0
// means known absent. Elapsed counts boundary turns since infliction, never
// action opportunities or a remaining random counter. PP origin3=forced
// release; origin4=R4 named pre-PP cancellation (neither spends PP or is a
// Sleep Talk call). Origin4 does not update repetition/lock. No native padding.
static int obs2_grounded(const Knowledge2 *k,int a) {
    const PublicMon *p=&k->base.side[a];
    if(k->gravity || (p->item_known && p->item==ITEM_IRONBALL)) return GROUND_YES;
    if(p->type1==T_FLY || p->type2==T_FLY || k->rise[a]) return GROUND_NO;
    if(p->item_known && p->item==ITEM_BALLOON) {
        if(p->item_state==1) return GROUND_NO;
        if(!p->item_state) return GROUND_UNKNOWN;
    }
    return GROUND_YES; // public Metagross default, not hidden grounding truth
}
static SeenDamage *obs2_damage_record(Knowledge2 *k,int a,int mv) {
    if(mv<0 || mv>=V2_MOVE_COUNT || MOVES[mv].cat==CAT_STATUS) return NULL;
    PublicMon *p=&k->base.side[a];
    for(int i=0;i<4;i++) if(p->moves[i].valid && p->moves[i].id==mv) return &k->seen[a][i];
    return NULL; // Struggle is not a revealed build move
}
static int obs2_clean(const Knowledge2 *k) {
    int a=k->attacker,b=1-a,mv=k->attack_move;
    const MoveDef *m=&MOVES[mv];
    int ai=mv==MV_BP?1:m->cat==CAT_PHYS?0:2;
    int di=m->cat==CAT_PHYS?1:3;
    if(MOVE_TRAITS[mv].defensive_stat) di=MOVE_TRAITS[mv].defensive_stat-1;
    if(k->attack_crit || k->base.side[a].stage[ai] || k->base.side[b].stage[di] ||
       (m->cat==CAT_PHYS?k->reflect[b]:k->light_screen[b]) || k->status[a]==STATUS_BRN) return 0;
    if((k->weather==WEATHER_SUN || k->weather==WEATHER_RAIN) && (m->type==T_FIRE || m->type==T_WATER)) return 0;
    if(k->weather==WEATHER_SAND && di==3 &&
       (k->base.side[b].type1==T_ROCK || k->base.side[b].type2==T_ROCK)) return 0;
    if(k->terrain==TERRAIN_PSYCHIC && m->type==T_PSY && obs2_grounded(k,a)!=GROUND_NO) return 0;
    return 1;
}
static void obs2_replay(Knowledge2 *k,const ObsEvent *e) {
    Knowledge *base=&k->base; PublicMon *p=&base->side[e->a]; TurnSummary *t=&base->current;
    int a=e->a;
    switch(e->kind) {
    case EV_RULES:
        memset(k,0,sizeof(*k)); obs_replay(base,e); return;
    case EV_BEGIN:
        for(int i=0;i<2;i++) { k->previous_attempt[i]=k->attempt[i]; k->attempt[i]=PUBLIC_NONE; }
        k->attack_active=0; obs_replay(base,e); return;
    case EV_OWN_MAXHP: if(!a) k->own_maxhp=e->x; return;
    case EV_OPPORTUNITY:
        if(!t->opportunities) t->first=a;
        t->opportunities++; t->activity[a].outcome=e->x;
        return; // actual counter ticks are separate branch-local v2 events
    case EV_BLOCK_TICK:
        p->block_set=obs_dec_set(p->block_set); p->block=p->block_set!=1; return;
    case EV_SLEEP_TICK:
        p->sleep_set=obs_dec_set(p->sleep_set); p->sleep=p->sleep_set!=1;
        t->activity[a].outcome=ACT_SLEEP; k->attempt[a]=PUBLIC_SKIP_FAIL; return;
    case EV_USE: {
        if(t->activity[a].outcome==ACT_NONE) t->activity[a].outcome=ACT_USED;
        t->activity[a].move_valid=1; t->activity[a].move=e->x;
        if(e->y==1) { t->activity[a].call_valid=1; t->activity[a].call=e->x; }
        if((e->y==0 || e->y==3 || e->y==4) && t->activity[a].outcome==ACT_USED) { t->nominal_valid[a]=1; t->nominal[a]=e->x; }
        if(e->x<V2_MOVE_COUNT) {
            Reveal *r=obs_reveal(p,e->x); r->uses++;
            if(e->y==1) r->called++;
            else if(e->y!=3 && e->y!=4) { r->spent++; r->lo--; r->hi--; }
        }
        k->attempt[a]=e->y==2?PUBLIC_UNKNOWN:PUBLIC_SUCCESS; return; }
    case EV_NOTE:
        if(e->x==V2_STRUGGLE_WIRE) { p->last=-1; p->streak=0; return; }
        p->streak=p->last==e->x?(p->streak<5?p->streak+1:5):1; p->last=e->x;
        if(p->item_known) { if(p->item==ITEM_SCARF) p->lock_set=lock_single(e->x); }
        else p->lock_set=lock_add(p->lock_set,e->x);
        return;
    case EV_EXEC:
        if(e->x!=MV_PRO && e->x!=MV_END) p->protect=0;
        if((e->x==MV_AGI && p->stage[4]==6) || (e->x==MV_ID && p->stage[1]==6) ||
           (e->x==MV_CP && p->stage[1]==6 && p->stage[3]==6)) k->attempt[a]=PUBLIC_FAIL;
        return;
    case EV_PROTECT:
        p->protect=e->x?p->protect+1:0;
        p->protected_now=e->x && t->activity[a].move!=MV_END;
        if(!e->x) k->attempt[a]=PUBLIC_FAIL;
        return;
    case EV_HIT:
        if(e->x==HIT_CRIT && k->attack_active && k->attacker==a) k->attack_crit=1;
        if(e->x==HIT_FAIL || e->x==HIT_MISS || e->x==HIT_IMMUNE) {
            k->attempt[a]=t->activity[a].move_valid && t->activity[a].move==MV_SUB?PUBLIC_NOT_FAIL:PUBLIC_FAIL;
        }
        if(e->x==HIT_PROTECT) k->attempt[a]=PUBLIC_NOT_FAIL;
        if(e->x==HIT_SUB && k->attack_active && k->attacker==a) {
            SeenDamage *d=obs2_damage_record(k,a,k->attack_move);
            if(d) { d->sub_hits++; d->sub_broke|=!e->y; }
        }
        obs_replay(base,e); return;
    case EV_ABILITY:
        if(e->x==AB_CB && t->activity[1-a].move_valid && t->activity[1-a].move==MV_SF) k->attempt[1-a]=PUBLIC_FAIL;
        obs_replay(base,e); return;
    case EV_STAGE:
        if(e->x==4 && e->y<0 && p->stage[4]==-6 &&
           t->activity[1-a].move_valid && t->activity[1-a].move==MV_SF) k->attempt[1-a]=PUBLIC_FAIL;
        obs_replay(base,e); return;
    case EV_HP:
        if(e->z==HP_HIT && k->attack_active && a!=k->attacker && e->x>=e->y) {
            SeenDamage *d=obs2_damage_record(k,k->attacker,k->attack_move);
            if(d) {
                int loss=a?100*(e->x-e->y):(k->own_maxhp?(int)(10000LL*(e->x-e->y)/k->own_maxhp):0);
                if(!d->hits || loss<d->lo) d->lo=loss;
                if(!d->hits || loss>d->hi) d->hi=loss;
                d->hits++; d->crits+=k->attack_crit;
                if(obs2_clean(k)) {
                    if(!d->clean_valid || loss<d->clean_lo) d->clean_lo=loss;
                    if(!d->clean_valid || loss>d->clean_hi) d->clean_hi=loss;
                    d->clean_valid=1; d->clean_hits++;
                }
            }
        }
        obs_replay(base,e); return;
    case EV_STATUS_START: k->status[a]=e->x; k->status_since[a]=e->turn; k->toxic[a]=0; return;
    case EV_STATUS_END: k->status[a]=0; k->status_since[a]=0; k->toxic[a]=0; return;
    case EV_CONFUSION_START: k->confused[a]=1; k->confusion_since[a]=e->turn; return;
    case EV_CONFUSION_END: k->confused[a]=0; k->confusion_since[a]=0; return;
    case EV_TOXIC_TICK: if(k->status[a]==STATUS_TOX && k->toxic[a]<15) k->toxic[a]++; return;
    case EV_WEATHER_START: k->weather=e->x; k->weather_turns=5; return;
    case EV_WEATHER_END: k->weather=k->weather_turns=0; return;
    case EV_TERRAIN_START: k->terrain=e->x; k->terrain_turns=5; return;
    case EV_TERRAIN_END: k->terrain=k->terrain_turns=0; return;
    case EV_SCREEN_START: if(e->x) k->light_screen[a]=5; else k->reflect[a]=5; return;
    case EV_SCREEN_END: if(e->x) k->light_screen[a]=0; else k->reflect[a]=0; return;
    case EV_ROCKS: k->rocks[a]=1; return;
    case EV_GRAVITY_START: k->gravity=5; return;
    case EV_GRAVITY_END: k->gravity=0; return;
    case EV_RISE_START: k->rise[a]=5; return;
    case EV_RISE_END: k->rise[a]=0; return;
    case EV_FUTURE_SET: k->future[a]=3; k->attempt[1-a]=PUBLIC_NOT_FAIL; return;
    case EV_FUTURE_IMPACT: k->future[a]=0; return;
    case EV_FUTURE_TICK: if(k->future[a]) k->future[a]--; return;
    case EV_CHARGE_START: k->charge[a]=1; k->attempt[a]=PUBLIC_NOT_FAIL; return;
    case EV_CHARGE_END: k->charge[a]=0; return;
    case EV_RECHARGE_START: k->recharge[a]=1; k->recharge_move[a]=e->x; return;
    case EV_RECHARGE: k->recharge[a]=k->recharge_move[a]=0; return;
    case EV_SKIP:
        k->attempt[a]=e->x==SKIP_RECHARGE?PUBLIC_SKIP_NOT_FAIL:e->x==SKIP_UNANNOUNCED?PUBLIC_UNKNOWN:
                      e->x==SKIP_FOCUS?PUBLIC_FAIL:PUBLIC_SKIP_FAIL;
        t->activity[a].outcome=e->x==SKIP_FLINCH?ACT_FLINCH:ACT_SKIPPED;
        return;
    case EV_ATTACK_START:
        k->attack_active=e->x>=0 && e->x<V2_MOVE_COUNT && MOVES[e->x].cat!=CAT_STATUS;
        k->attacker=a; k->attack_move=e->x; k->attack_crit=0; k->attack_future=e->y; k->saved_attempt=k->attempt[a]; return;
    case EV_ATTACK_END: if(k->attack_future) k->attempt[k->attacker]=k->saved_attempt; k->attack_active=0; k->attack_future=0; return;
    case EV_END:
        if(k->weather_turns) k->weather_turns--;
        if(k->terrain_turns) k->terrain_turns--;
        if(k->gravity) k->gravity--;
        for(int i=0;i<2;i++) {
            if(k->reflect[i]) k->reflect[i]--;
            if(k->light_screen[i]) k->light_screen[i]--;
            if(k->rise[i]) k->rise[i]--;
            base->side[i].protected_now=0;
        }
        t->order=t->opportunities==2?(t->first==0?1:2):3;
        if(t->nominal_valid[0] && t->nominal_valid[1]) {
            int pa=t->nominal[0]==V2_STRUGGLE_WIRE?-1:MOVES[t->nominal[0]].prio;
            int pb=t->nominal[1]==V2_STRUGGLE_WIRE?-1:MOVES[t->nominal[1]].prio;
            t->relation=pa==pb?1:pa>pb?2:3;
        }
        t->qualified=t->opportunities==2 && t->relation==1 && !t->claw[0] && !t->claw[1];
        base->counts[t->qualified?t->first:2]++; base->previous=*t;
        for(int i=0;i<2;i++) k->previous_attempt[i]=k->attempt[i];
        return;
    case EV_TERMINAL:
        k->result_valid=1; k->result=e->x; k->cap=e->y; obs_replay(base,e); return;
    default: obs_replay(base,e); return;
    }
}
static void obs2_build(const Mon *self, const Knowledge2 *k2, Observation2 *o, ActionMask *mask) {
    const Knowledge *k=&k2->base;
    memset(o,0,sizeof(*o)); int n=0;
#define PUT(x) do { if(n>=OBS2_WORDS) abort(); o->v[n++]=(x); } while(0)
    PUT(k->turn); PUT(k->profile); PUT(k->terminal);
    for(int a=0;a<2;a++) { const PublicMon *p=&k->side[a];
        PUT(376); PUT(p->mega); PUT(p->type1); PUT(p->type2);
        for(int i=0;i<6;i++) PUT(OBS_BASE[p->mega][i]);
    }
    PUT(obs_clamp_hp(self->hp,self->maxhp)); PUT(self->maxhp);
    for(int i=0;i<6;i++) PUT(self->stats[i]);
    for(int i=0;i<5;i++) PUT(self->st[i]);
    PUT(self->acc);
    PUT(self->sleep); PUT(self->healblock); PUT(self->sub_hp); PUT(self->protect_streak); PUT(self->protected_now);
    PUT(self->item); PUT(self->item_used); PUT(self->mega?AB_TC:self->ability);
    PUT(self->choice_lock+1); PUT(self->last_move+1); PUT(self->metro_streak);
    for(int i=0;i<4;i++) { int yes=i<self->nmoves; PUT(yes); PUT(yes?self->moves[i]:0); PUT(yes?self->pp[i]:0); PUT(yes?MOVES[self->moves[i]].pp:0); }
    const PublicMon *p=&k->side[1];
    PUT(p->hp); PUT(p->faint); for(int i=0;i<6;i++) PUT(p->stage[i]);
    PUT(p->sleep); PUT(p->block); PUT(p->sub); PUT(p->protect); PUT(p->protected_now);
    PUT(p->sleep_set); PUT(p->block_set);
    for(int i=0;i<4;i++) { const Reveal *r=&p->moves[i];
        PUT(r->valid); PUT(r->id); PUT(r->uses); PUT(r->spent); PUT(r->called); PUT(r->pp_valid); PUT(r->lo); PUT(r->hi);
    }
    PUT(p->item_known); PUT(p->item); PUT(p->item_state);
    PUT(p->abilities); PUT(p->ability_known); PUT(p->ability); PUT(0); // v2 lock lives at 222..225
    PUT(p->last+1); PUT(p->streak);
    PUT(p->sub); PUT(!p->sub); PUT(0); PUT(0);
    for(int i=0;i<18;i++) PUT(0);
    const TurnSummary *t=&k->previous;
    PUT(t->order); for(int i=0;i<2;i++) { PUT(t->nominal_valid[i]); PUT(t->nominal[i]); }
    PUT(t->relation); PUT(t->claw[0]); PUT(t->claw[1]); PUT(t->qualified);
    for(int i=0;i<3;i++) PUT(k->counts[i]);
    for(int i=0;i<2;i++) { const Activity *a=&t->activity[i];
        PUT(a->move_valid); PUT(a->move); PUT(a->call_valid); PUT(a->call); PUT(a->outcome); PUT(a->crit);
    }
    for(int i=0;i<2;i++) { const HPSummary *h=&t->hp[i]; PUT(h->start); PUT(h->end); PUT(h->loss); PUT(h->gain); PUT(h->present); }
    obs_mask(self,k->terminal,mask); for(int i=0;i<5;i++) PUT(mask->legal[i]);
    if(n!=181) abort();
    PUT(V2_MOVE_COUNT);
    PUT(self->status); PUT(self->freeze_turns); PUT(self->toxic_stage); PUT(self->confusion);
    PUT(self->attempt_prev); PUT(self->recharge); PUT(self->charging);
    PUT(k2->status[1]); PUT(k2->status[1]?k->turn-k2->status_since[1]:0); PUT(k2->toxic[1]);
    PUT(k2->status[1]==STATUS_FRZ?k->turn-k2->status_since[1]:0);
    PUT(k2->confused[1]); PUT(k2->confused[1]?k->turn-k2->confusion_since[1]:0);
    PUT(k2->weather); PUT(k2->weather_turns); PUT(k2->terrain); PUT(1); PUT(k2->terrain_turns);
    PUT(k2->gravity); PUT(k2->gravity>0);
    for(int a=0;a<2;a++) { PUT(k2->reflect[a]); PUT(k2->light_screen[a]); PUT(k2->rocks[a]); PUT(k2->rise[a]); }
    PUT(obs2_grounded(k2,1));
    PUT(k2->future[0]>0); PUT(k2->future[0]); PUT(k2->future[1]>0); PUT(k2->future[1]);
    PUT(k2->charge[1]); PUT(k2->charge[1]); PUT(k2->charge[1]?MV_MB:0);
    PUT(k2->recharge[1]); PUT(k2->recharge[1]); PUT(k2->recharge[1]?k2->recharge_move[1]:0);
    PUT(k2->previous_attempt[1]);
    for(int w=0;w<2;w++) for(int b=0;b<2;b++) PUT((int32_t)(uint32_t)(p->lock_set.word[w]>>(32*b)));
    for(int a=0;a<2;a++) for(int i=0;i<4;i++) {
        const Reveal *r=&k->side[a].moves[i]; const SeenDamage *d=&k2->seen[a][i];
        int valid=r->valid && MOVES[r->id].cat!=CAT_STATUS;
        PUT(valid); PUT(valid?r->id:0);
        PUT(d->hits); PUT(d->lo); PUT(d->hi); PUT(d->crits); PUT(d->sub_hits); PUT(d->sub_broke);
        PUT(d->clean_valid); PUT(d->clean_lo); PUT(d->clean_hi); PUT(d->clean_hits);
    }
    PUT(k2->result_valid); PUT(k2->result); PUT(k2->cap);
    if(n!=325) abort();
#undef PUT
}
static int obs2_valid_build(const Mon *m, int profile) {
    if(!s8_old_observation_scope(m)) return 0;
    if(m->mega_mode==2 && (m->item!=ITEM_STONE || !m->mega_stats_valid)) return 0;
    if(m->state_features || m->status || m->freeze_turns || m->toxic_stage || m->confusion || m->weather || field_v1_unsupported(m)) return 0;
    if(m->mega<0 || m->mega>1 || m->maxhp<=0 || m->nmoves<1 || m->nmoves>4) return 0;
    if(profile==OBS_REGULAR && (m->nmoves!=4 || (!m->mega && m->ability!=AB_CB && m->ability!=AB_LM))) return 0;
    for(int i=0;i<m->nmoves;i++) {
        if(m->moves[i]<0 || m->moves[i]>=MV_COUNT || m->pp[i]!=MOVES[m->moves[i]].pp) return 0;
        for(int j=0;j<i;j++) if(m->moves[j]==m->moves[i]) return 0;
    }
    for(int i=0;i<6;i++) if(m->stats[i]<=0) return 0;
    if(profile==OBS_REGULAR) {
        // Existing project allocation model (run1/gen_stage1.py): 0..32
        // per stat, at most 66 total, 21 nature effects, level 50 core.
        int legal=0;
        for(int up=1;up<=5;up++) for(int down=1;down<=5;down++) {
            int total=0, possible=1;
            for(int i=0;i<6;i++) {
                int core=OBS_BASE[m->mega][i]+(i==0?75:20), least=33;
                for(int points=0;points<=32;points++) {
                    int value=core+points;
                    if(up!=down && i==up) value=(int)floor(value*1.1);
                    if(up!=down && i==down) value=(int)floor(value*0.9);
                    if(value==m->stats[i]) { least=points; break; }
                }
                if(least==33) possible=0;
                total+=least;
            }
            if(possible && total<=66) legal=1;
        }
        if(!legal) return 0;
    }
    return 1;
}

static inline void obs2_serialize(const Observation2 *o,unsigned char bytes[OBS2_BYTES]) {
    memcpy(bytes,"MGO2",4);
    for(int j=0;j<4;j++) bytes[4+j]=(unsigned char)((uint32_t)OBS2_WORDS>>(8*j));
    for(int i=0;i<OBS2_WORDS;i++) for(int j=0;j<4;j++) bytes[8+4*i+j]=(unsigned char)((uint32_t)o->v[i]>>(8*j));
}
static int obsdemo2(const Observation2 *o,const ActionMask *mask) {
    (void)o; for(int i=0;i<5;i++) if(mask->legal[i]) return i; return -1;
}
static int obsh3(const Observation2 *o,const ActionMask *mask);
typedef int (*ObservablePolicy2)(const Observation2 *,const ActionMask *);
static ObservablePolicy2 observable_policy2=obsdemo2;
static int learned2(const Observation2 *o,const ActionMask *mask);
static ObservablePolicy2 learned2_policy=learned2;
static void obs2_attach(Mon *a,Mon *b,int profile) {
    memset(&OBS_STORAGE,0,sizeof(OBS_STORAGE)); OBS=&OBS_STORAGE;
    OBS->seat[0]=a; OBS->seat[1]=b; OBS->profile=profile; OBS->version=2; OBS->turn=1;
    obs_emit(EV_RULES,a,profile,0,0);
    for(int i=0;i<2;i++) {
        Mon *m=OBS->seat[i]; obs_emit(EV_START,m,m->mega,m->type1,m->type2); obs_hp(m,m->hp,0);
        ++OBS->seq; obs_deliver(i,EV_OWN_MAXHP,i,m->maxhp,0,0);
        if(m->item==ITEM_BALLOON && !m->item_used) obs_item(m,0);
    }
    obs_emit(EV_BEGIN,a,0,0,0);
}
static int obs2_vocabulary(const Mon *a,const Mon *b) {
    const Mon *ms[]={a,b};
    for(int j=0;j<2;j++) for(int i=0;i<ms[j]->nmoves;i++) if(ms[j]->moves[i]>=V1_MOVE_COUNT) return 1;
    return 0;
}


// MGO3 records are revelation ordered. PP identity is independent of use.
static Obs3Record *obs3_record(Obs3Mon *p,int mv) {
    if(mv<0 || mv>=S8_MOVES_COUNT || mv==S8_STRUGGLE) return NULL;
    for(int i=0;i<OBS3_RECORDS;i++) if(p->records[i].valid && p->records[i].id==mv) return &p->records[i];
    for(int i=0;i<OBS3_RECORDS;i++) if(!p->records[i].valid) {
        Obs3Record *r=&p->records[i]; r->valid=1;r->id=mv;
        r->pp_valid=1;r->lo=r->hi=p->transformed?5:S8_MOVES[mv].pp;
        r->generation=p->generation;return r;
    }
    fprintf(stderr,"MGO3: move history exceeded admitted two-build closure\n");abort();
}
static void obs3_duration_set(Knowledge3 *k,int id,int lo,int hi) {
    Obs3Duration *d=&k->duration[id];
    *d=(Obs3Duration){lo>0,lo==hi?1:2,k->turn,lo,hi,0};
}
static int obs3_clean(const Knowledge3 *k,int actor,int mv,int crit) {
    if(mv<0 || mv>=S8_MOVES_COUNT || crit) return 0;
    const S8Move *m=&S8_MOVES[mv];int target=1-actor;
    int ai=mv==MV_BP?1:m->category==CAT_PHYS?0:2;
    int di=m->category==CAT_PHYS?1:3;
    if(mv==MV_PSH || !strcmp(m->id,"secretsword")) di=1;
    if(k->side[actor].rank[ai] || k->side[target].rank[di] ||
       k->side[actor].status==STATUS_BRN) return 0;
    if(k->duration[O3_SIDE+target*7+(m->category==CAT_PHYS?0:1)].active ||
       k->duration[O3_SIDE+target*7+2].active) return 0;
    // Conservative clean partition: field damage modifiers, including all
    // terrains, weather, rooms and announced power/defence volatiles.
    if(k->duration[O3_WEATHER].active || k->duration[O3_TERRAIN].active ||
       k->duration[O3_WONDER].active || k->side[actor].vol[O3_FLASH] ||
       k->side[actor].vol[S8C_CHARGE] || k->side[target].vol[S8C_GLAIVERUSH]) return 0;
    return 1;
}
// A previously announced item can make a public duration unique. Suppression
// and Klutz are considered only when publicly known or ruled out by species.
static int obs3_public_duration(const Knowledge3 *k,int actor,int id,int lo,int hi) {
    const Obs3Mon *p=&k->side[actor];
    if(lo==hi || !p->item_known) return 0;
    int kind=id==O3_WEATHER?S8D_WEATHER:id==O3_TERRAIN?S8D_TERRAIN:S8D_SCREEN,extended=0;
    for(size_t i=0;i<sizeof(S8_DURATION_ITEMS)/sizeof(*S8_DURATION_ITEMS);i++) {
        const S8DurationItem *row=&S8_DURATION_ITEMS[i];
        if(row->kind==kind && S8_ITEM_OPERATIONS[row->item].table==p->item &&
           (kind!=S8D_WEATHER || row->weather==k->weather || (row->weather==WEATHER_SNOW && k->weather==WEATHER_HAIL))) extended=1;
    }
    if(!extended || p->item_state==2 || k->duration[O3_MAGIC].active) return 5;
    int klutz=-1;for(int i=0;i<S8_ABILITIES_COUNT;i++) if(!strcmp(S8_ABILITIES[i].id,"klutz")) klutz=i;
    if(p->ability_known) return p->ability==klutz && !p->suppressed?5:8;
    const S8Species *species=&S8_SPECIES[p->form];
    for(int i=0;i<species->ability_count;i++) if(S8_SPECIES_ABILITIES[species->ability_offset+i]==klutz) return 0;
    return 8;
}
// ---------- Stage 8e-2b: deterministic belief replay ----------
static int obs3_bit(const uint32_t *bits,int id) {
    return id>=0 && !!(bits[id/32]&(UINT32_C(1)<<(id%32)));
}
static void obs3_bit_set(uint32_t *bits,int id) { bits[id/32]|=UINT32_C(1)<<(id%32); }
static void obs3_bit_clear(uint32_t *bits,int id) { if(id>=0) bits[id/32]&=~(UINT32_C(1)<<(id%32)); }
static int obs3_ability_id(const char *key) {
    for(int i=0;i<S8_ABILITIES_COUNT;i++) if(!strcmp(S8_ABILITIES[i].id,key)) return i;
    return -1;
}
static int obs3_item_bit(const Obs3Belief *b,int item) {
    for(int i=0;i<b->population;i++) if(b->item_id[i]==item) return i;
    return -1;
}
static int obs3_item_possible(const Obs3Belief *b,int item) {
    int bit=obs3_item_bit(b,item);return bit>=0 && obs3_bit(b->items,bit);
}
static void obs3_item_remove(Obs3Belief *b,int item) {
    int bit=obs3_item_bit(b,item);if(bit>=0) obs3_bit_clear(b->items,bit);
}
static void obs3_item_exact(Obs3Belief *b,int item) {
    int bit=obs3_item_bit(b,item);
    // A revealed exchanged foreign stone is represented by confirmed identity;
    // it has no bit in this species' frozen initial-stone vocabulary.
    memset(b->items,0,sizeof(b->items));
    if(bit>=0) obs3_bit_set(b->items,bit);
}
static int obs3_param_value(const Obs3Belief *b,int stat,int nature,int points) {
    int source=b->source[stat];
    int v=(2*S8_SPECIES[b->form].base_stats[source]+31)/2+(source?5:60)+points;
    if(source && nature!=1) v=v*(nature?110:90)/100;
    return v;
}
static void obs3_bounds(const Obs3Belief *b,int stat,int *lo,int *hi) {
    if(b->replace[stat]) { *lo=b->lo[stat];*hi=b->hi[stat];return; }
    *lo=100000;*hi=0;
    int source=b->source[stat];
    // Enforce the public build contract: one up/down nature (or neutral),
    // individual points <=32 and total points 66. The DP is truncated at 66.
    for(int up=0;up<=5;up++) for(int down=0;down<=5;down++) {
        if((!up && down) || (up && (!down || up==down))) continue;
        __uint128_t sums=1,limit=(((__uint128_t)1)<<67)-1;
        for(int i=0;i<6;i++) if(i!=source) {
            int n=i && i==up?2:i && i==down?0:1;
            __uint128_t next=0;
            for(int q=0;q<=32;q++) if(b->params[i][n]&(UINT64_C(1)<<q)) next|=sums<<q;
            sums=next&limit;
        }
        int n=source && source==up?2:source && source==down?0:1;
        for(int q=0;q<=32;q++) if((b->params[source][n]&(UINT64_C(1)<<q)) && (sums&(((__uint128_t)1)<<(66-q)))) {
            int v=obs3_param_value(b,stat,n,q);if(v<*lo) *lo=v;if(v>*hi) *hi=v;
        }
    }
}
static void obs3_params_intersect(Obs3Belief *b,int stat,const uint64_t allowed[3]) {
    if(b->replace[stat]) return;
    int source=b->source[stat];uint64_t old[3];memcpy(old,b->params[source],sizeof(old));
    for(int i=0;i<3;i++) b->params[source][i]&=allowed[i];
    int point=0;
    for(int i=0;i<6;i++) { int lo,hi;obs3_bounds(b,i,&lo,&hi);point|=lo>=hi; }
    // A point estimate is forbidden. Keep the preceding conservative domain
    // when this evidence would identify a point (or has no legal witness).
    if(point) memcpy(b->params[source],old,sizeof(old));
}
static void obs3_belief_identity(Obs3Belief *b,int species,int form) {
    int initial=!b->valid,changed=b->valid && b->form!=form;
    if(initial) {
        b->valid=1;
        for(int i=0;i<S8_NON_STONE_ITEMS_COUNT;i++) b->item_id[b->population++]=i;
        b->item_id[b->population++]=-1;
        for(int i=0;i<S8_MEGA_LINKS_COUNT;i++) if(S8_MEGA_LINKS[i].source==species)
            b->item_id[b->population++]=S8_MEGA_LINKS[i].stone;
        for(int i=0;i<b->population;i++) obs3_bit_set(b->items,i);
        for(int s=0;s<6;s++) {
            b->source[s]=s;
            for(int n=0;n<3;n++) b->params[s][n]=(s || n==1)?((UINT64_C(1)<<33)-1):0;
        }
    }
    if(changed) for(int i=1;i<6;i++) { b->source[i]=i;b->replace[i]=0; }
    if(initial || changed) {
        memset(b->abilities,0,sizeof(b->abilities));
        const S8Species *row=&S8_SPECIES[form];
        for(int i=0;i<row->ability_count;i++) obs3_bit_set(b->abilities,S8_SPECIES_ABILITIES[row->ability_offset+i]);
    }
    b->form=form;
}
static int obs3_has_hypothesis(const Knowledge3 *k,int a,const char *key) {
    int ab=obs3_ability_id(key);return ab>=0 && obs3_bit(k->belief[a].abilities,ab) && !k->side[a].suppressed;
}
static int obs3_items_active(const Knowledge3 *k,int a) {
    return !k->duration[O3_MAGIC].active && !obs3_has_hypothesis(k,a,"klutz");
}
static int obs3_can_eat(const Knowledge3 *k,int a) {
    if(!obs3_items_active(k,a) || k->side[a].vol[O3_BLOCK]) return 0;
    if(a) return k->own.valid && (k->own.suppressed ||
        (strcmp(S8_ABILITIES[k->own.ability].id,"unnerve") && strcmp(S8_ABILITIES[k->own.ability].id,"asoneglastrier") && strcmp(S8_ABILITIES[k->own.ability].id,"asonespectrier")));
    return !obs3_has_hypothesis(k,1,"unnerve") && !obs3_has_hypothesis(k,1,"asoneglastrier") && !obs3_has_hypothesis(k,1,"asonespectrier");
}
// Certified partitions: other abilities retain their hypotheses. This is a
// sufficient-evidence rule, never an assumption that an unknown hook is absent.
static int obs3_neutral_ability(int ab,int speed_only) {
    if(ab<0 || ab>=S8_ABILITIES_COUNT) return 0;
    const char *id=S8_ABILITIES[ab].id;
    const char *damage[]={"clearbody","lightmetal","runaway","illuminate","keeneye","innerfocus","owntempo","limber","insomnia","vitalspirit","pickup","frisk","forewarn","anticipation","pressure","unnerve","levitate"};
    for(size_t i=0;i<sizeof(damage)/sizeof(*damage);i++) if(!strcmp(id,damage[i])) return 1;
    if(speed_only) {
        const char *extra[]={"toughclaws","blaze","torrent","overgrow","swarm","magicguard","sheerforce","sturdy","rockhead","ironfist","strongjaw","megalauncher","sniper","superluck"};
        for(size_t i=0;i<sizeof(extra)/sizeof(*extra);i++) if(!strcmp(id,extra[i])) return 1;
    }
    return 0;
}
static int obs3_neutral_side(const Knowledge3 *k,int a,int speed_only) {
    if(k->side[a].suppressed) return 1;
    if(!a) return k->own.valid && (k->own.suppressed || obs3_neutral_ability(k->own.ability,speed_only));
    int found=0;
    for(int i=0;i<S8_ABILITIES_COUNT;i++) if(obs3_bit(k->belief[a].abilities,i)) {
        found=1;if(!obs3_neutral_ability(i,speed_only)) return 0;
    }
    return found;
}
static int obs3_scale(int v,int modifier) { return (int)(((int64_t)v*modifier+2047)/4096); }
static int obs3_chain(int a,int b) { return (int)(((int64_t)a*b+2048)/4096); }
static int obs3_rank_value(int v,int rank) { return rank>=0?v*(2+rank)/2:v*2/(2-rank); }
static void obs3_speed_prepare(Knowledge3 *k,int first) {
    k->speed_valid=0;
    Obs3Belief *b=&k->belief[1];const Obs3Mon *p=&k->side[1];
    if(k->terrain || !k->own.valid || !b->valid || b->replace[5] || !obs3_neutral_side(k,0,1) || !obs3_neutral_side(k,1,1)) return;
    k->speed_valid=1;k->speed_form=b->form;
    memset(k->speed_possible,0,sizeof(k->speed_possible));
    for(int bit=0;bit<b->population;bit++) if(obs3_bit(b->items,bit)) {
        int item=b->item_id[bit];
        int active=obs3_items_active(k,1) && (!p->item_known || p->item_state!=2);
        for(int n=0;n<3;n++) for(int q=0;q<=32;q++) if(b->params[b->source[5]][n]&(UINT64_C(1)<<q)) {
            int raw=obs3_param_value(b,5,n,q),rank=p->rank[4];
            double value=raw*(rank>=0?(2.0+rank)/2:2.0/(2-rank));
            int integral=p->status==STATUS_PAR || k->duration[O3_TRICK].active || k->duration[O3_SIDE+7+4].active || k->duration[O3_MAGIC].active;
            int mod=4096;
            if(active && item==S8_ITEM_OPERATIONS[ITEM_SCARF].table) mod=6144; // generated ordinary IDs
            if(active && item==S8_ITEM_OPERATIONS[ITEM_IRONBALL].table) mod=2048;
            if(k->duration[O3_SIDE+7+4].active) mod=obs3_chain(mod,8192);
            double fractional=value*mod/4096.0,rounded=obs3_scale((int)value,mod);
            double low=integral?rounded:fmin(fractional,rounded),high=integral?rounded:fmax(fractional,rounded);
            // A past ability replacement can leave an integer getStat path.
            // Keep both rounding paths when that history does not disprove it.
            if(p->status==STATUS_PAR) low=high=(int)rounded/2;
            if(k->duration[O3_TRICK].active) { double tmp=low;low=-high;high=-tmp; }
            // Ties permit either order. The own-speed transport is an outward
            // quantization of owner-exact information, never the foe's speed.
            double own=k->own.speed/4096.0;
            if(first?(high>=own-1.0/4096):(low<=own+1.0/4096)) k->speed_possible[bit][n]|=UINT64_C(1)<<q;
        }
    }
}
static void obs3_speed_finish(Knowledge3 *k) {
    Obs3Belief *b=&k->belief[1];TurnSummary *t=&k->current;
    if(!k->speed_valid || b->form!=k->speed_form || !t->qualified || t->nominal[0]>=S8_MOVES_COUNT || t->nominal[1]>=S8_MOVES_COUNT) return;
    // Move-priority modifiers are excluded by the neutral ability partition.
    uint64_t allowed[3]={0,0,0};uint32_t items[3]={0,0,0};
    for(int i=0;i<b->population;i++) if(obs3_bit(b->items,i)) {
        uint64_t any=0;for(int n=0;n<3;n++) { allowed[n]|=k->speed_possible[i][n];any|=k->speed_possible[i][n]; }
        if(any) obs3_bit_set(items,i);
    }
    if(items[0] || items[1] || items[2]) {
        memcpy(b->items,items,sizeof(items));obs3_params_intersect(b,5,allowed);
    }
}
static int obs3_public_type(const Obs3Mon *p,int i) { return i==2?p->types[i]-1:p->types[i]; }
static double obs3_effectiveness(const Knowledge3 *k,int target,int type) {
    double eff=1;
    for(int i=0;i<3;i++) eff*=eff_vs(type,obs3_public_type(&k->side[target],i));
    return eff;
}
static int obs3_resist_item(int item,int type,double eff) {
    for(size_t i=0;i<sizeof(S8_RESIST_BERRIES)/sizeof(*S8_RESIST_BERRIES);i++) {
        const S8ResistBerry *row=&S8_RESIST_BERRIES[i];
        if(row->operation.table==item && row->type==type && (eff>1 || (!row->super_effective && eff>0))) return 1;
    }
    return eff>1 && ((item==S8_ITEM_OPERATIONS[ITEM_SHUCA].table && type==T_GROUND) || (item==S8_ITEM_OPERATIONS[ITEM_CHOPLE].table && type==T_FIGHT) || (item==S8_ITEM_OPERATIONS[ITEM_KASIB].table && type==T_GHOST) || (item==S8_ITEM_OPERATIONS[ITEM_PAYAPA].table && type==T_PSY));
}
static int obs3_fixed_damage_move(int mv) {
    const char *allowed[]={"bodypress","earthquake","tackle","flamethrower","surf","icebeam","shadowball","psychic","flashcannon","ironhead","bulletpunch","aerialace","airslash","darkpulse","dragonpulse","thunderbolt"};
    int found=0;
    for(size_t i=0;i<sizeof(allowed)/sizeof(*allowed);i++) if(!strcmp(S8_MOVES[mv].id,allowed[i])) found=1;
    return found;
}
static int obs3_damage_certified(const Knowledge3 *k,int mv) {
    if(k->type_uncertain[0] || k->type_uncertain[1] || !obs3_fixed_damage_move(mv) || !k->own.valid || !obs3_neutral_side(k,0,0) || !obs3_neutral_side(k,1,0)) return 0;
    if(k->terrain || k->duration[O3_WONDER].active) return 0;
    for(int a=0;a<2;a++) {
        const Obs3Mon *p=&k->side[a];
        if(p->transformed || p->vol[O3_POWERTRICK] || p->vol[O3_CHARGE] || p->vol[S8C_CHARGE] || p->vol[O3_FLASH] ||
           p->vol[S8C_GLAIVERUSH] || p->vol[S8C_MINIMIZE] || p->vol[O3_STOCK] || p->vol[O3_ROOST]) return 0;
    }
    return 1;
}
static int obs3_damage_predict(const Knowledge3 *k,int actor,int mv,int attack,int defense,int item,int crit,int roll) {
    const S8Move *m=&S8_MOVES[mv];int target=1-actor;
    int ai=mv==MV_BP?2:m->category==CAT_PHYS?1:3,di=m->category==CAT_PHYS?2:4;
    int ar=k->side[actor].rank[ai-1],dr=k->side[target].rank[di-1];
    if(crit && ar<0) ar=0;
    if(crit && dr>0) dr=0;
    attack=obs3_rank_value(attack,ar);defense=obs3_rank_value(defense,dr);
    int atitem=actor?item:k->own.item,deitem=actor?k->own.item:item;
    if(!obs3_items_active(k,actor) || (actor?k->side[1].item_state==2:k->own.used)) atitem=-1;
    if(!obs3_items_active(k,target) || (target?k->side[1].item_state==2:k->own.used)) deitem=-1;
    int power_mod=4096;
    if((m->category==CAT_PHYS && atitem==S8_ITEM_OPERATIONS[ITEM_MUSCLE].table) || (m->category==CAT_SPEC && atitem==S8_ITEM_OPERATIONS[ITEM_WISE].table)) power_mod=4505;
    for(size_t i=0;i<sizeof(S8_TYPE_BOOST_ITEMS)/sizeof(*S8_TYPE_BOOST_ITEMS);i++) {
        const S8TypeBoostItem *row=&S8_TYPE_BOOST_ITEMS[i];
        if(row->operation.table==atitem && row->type==m->type) power_mod=obs3_chain(power_mod,4915);
    }
    if(atitem==S8_ITEM_OPERATIONS[ITEM_NORMALGEM].table && m->type==T_NORMAL) power_mod=obs3_chain(power_mod,5325);
    if(atitem==S8_ITEM_OPERATIONS[ITEM_LIGHTBALL].table && !strcmp(S8_SPECIES[k->side[actor].species].id,"pikachu") ) attack*=2;
    int weather=k->weather;
    int icy=0,rock=0,stab=0;
    for(int i=0;i<3;i++) {
        icy|=obs3_public_type(&k->side[target],i)==T_ICE;rock|=obs3_public_type(&k->side[target],i)==T_ROCK;
        stab|=obs3_public_type(&k->side[actor],i)==m->type;
    }
    if((di==2 && weather==WEATHER_SNOW && icy) || (di==4 && weather==WEATHER_SAND && rock)) defense=obs3_scale(defense,6144);
    if(attack<1) attack=1;
    if(defense<1) defense=1;
    int base=(int)((int64_t)22*obs3_scale(m->power,power_mod)*attack/defense/50)+2;
    if((weather==WEATHER_SUN && m->type==T_FIRE) || (weather==WEATHER_RAIN && m->type==T_WATER)) base=obs3_scale(base,6144);
    if((weather==WEATHER_SUN && m->type==T_WATER) || (weather==WEATHER_RAIN && m->type==T_FIRE)) base=obs3_scale(base,2048);
    if(crit) base=base*3/2;
    base=base*roll/100;
    if(stab) base=obs3_scale(base,6144);
    double eff=obs3_effectiveness(k,target,m->type);base=(int)(base*eff);
    if(k->side[actor].status==STATUS_BRN && m->category==CAT_PHYS) base=obs3_scale(base,2048);
    int final=4096;
    if(atitem==S8_ITEM_OPERATIONS[ITEM_LIFEORB].table) final=obs3_chain(final,5324);
    if(atitem==S8_ITEM_OPERATIONS[ITEM_BELT].table && eff>1) final=obs3_chain(final,4915);
    if(atitem==S8_ITEM_OPERATIONS[ITEM_METRO].table) { static const int mods[]={4096,4915,5734,6553,7372,8192};int streak=k->side[actor].last==mv+1?k->side[actor].streak:0;final=obs3_chain(final,mods[streak>5?5:streak]); }
    if(obs3_resist_item(deitem,m->type,eff) && obs3_can_eat(k,target)) final=obs3_chain(final,2048);
    if(!crit && (k->duration[O3_SIDE+target*7+(m->category==CAT_PHYS?0:1)].active || k->duration[O3_SIDE+target*7+2].active)) final=obs3_chain(final,2048);
    base=obs3_scale(base,final);return base>0?base:1;
}
static int obs3_damage_witness(int before,int after,int maxhp,int low,int high) {
    // Existence of actual HP endpoints under the public rounded bar. Keeping
    // the whole roll envelope is conservative even when integer rolls skip.
    int minhp=(maxhp*(2*before-1)+199)/200,maxbefore=(maxhp*(2*before+1)-1)/200;
    if(minhp<1) minhp=1;
    if(maxbefore>maxhp) maxbefore=maxhp;
    int minafter=(maxhp*(2*after-1)+199)/200,maxafter=(maxhp*(2*after+1)-1)/200;
    if(minafter<0) minafter=0;
    if(maxafter>maxhp) maxafter=maxhp;
    if(!after) return maxbefore>=low; // overkill is publicly censored
    return maxbefore-minafter>=low && minhp-maxafter<=high;
}
static void obs3_damage_evidence(Knowledge3 *k,int target,int before,int after) {
    int ix=k->depth-1,actor=k->attack[ix].actor,mv=k->attack[ix].move,crit=k->attack[ix].crit;
    if(actor==target || mv<0 || mv>=S8_MOVES_COUNT || before<=after || k->attack[ix].context_changed || !obs3_damage_certified(k,mv)) return;
    if(obs3_effectiveness(k,target,S8_MOVES[mv].type)<=0) return;
    Obs3Belief *b=&k->belief[1];
    int stat=actor?(mv==MV_BP?2:S8_MOVES[mv].category==CAT_PHYS?1:3):(S8_MOVES[mv].category==CAT_PHYS?2:4);
    if(b->replace[stat] || b->replace[0]) return;
    uint64_t allowed[3]={0,0,0},hp_allowed[3]={0,0,0};uint32_t items[3]={0,0,0};
    int own_stat=actor?(S8_MOVES[mv].category==CAT_PHYS?2:4):(mv==MV_BP?2:S8_MOVES[mv].category==CAT_PHYS?1:3);
    for(int bit=0;bit<b->population;bit++) if(obs3_bit(b->items,bit)) {
        int item=b->item_id[bit],any=0;
        // Focus Band can replace any lethal hit with survival at 1 HP. Its
        // latent random activation is an alternative, never negative evidence.
        if(!actor && (item==S8_ITEM_OPERATIONS[ITEM_FBAND].table || item==S8_ITEM_OPERATIONS[ITEM_SASH].table) && after<=1) {
            obs3_bit_set(items,bit);
            for(int n=0;n<3;n++) allowed[n]|=b->params[b->source[stat]][n];
            hp_allowed[1]|=b->params[0][1];continue;
        }
        for(int n=0;n<3;n++) for(int q=0;q<=32;q++) if(b->params[b->source[stat]][n]&(UINT64_C(1)<<q)) {
            int value=obs3_param_value(b,stat,n,q);
            int low=obs3_damage_predict(k,actor,mv,actor?value:k->own.stats[own_stat],actor?k->own.stats[own_stat]:value,item,crit,85);
            int high=obs3_damage_predict(k,actor,mv,actor?value:k->own.stats[own_stat],actor?k->own.stats[own_stat]:value,item,crit,100);
            int fits=actor?((!after && before<=high) || (after>0 && before-after>=low && before-after<=high)):0;
            if(!actor) for(int h=0;h<=32;h++) if(b->params[0][1]&(UINT64_C(1)<<h)) {
                int maxhp=obs3_param_value(b,0,1,h);
                if(obs3_damage_witness(before,after,maxhp,low,high)) { fits=1;hp_allowed[1]|=UINT64_C(1)<<h; }
            }
            if(fits) { any=1;allowed[n]|=UINT64_C(1)<<q; }
        }
        if(any) obs3_bit_set(items,bit);
    }
    if(items[0] || items[1] || items[2]) {
        memcpy(b->items,items,sizeof(items));obs3_params_intersect(b,stat,allowed);
        if(!actor) obs3_params_intersect(b,0,hp_allowed);
    }
}
static void obs3_absence_hit(Knowledge3 *k,int actor,int mv) {
    if(actor || !k->own.valid || k->side[1].suppressed || k->type_uncertain[actor] || !obs3_fixed_damage_move(mv)) return;
    // Neutral owner abilities cannot bypass the defender's immunity hook.
    if(!k->own.suppressed && !obs3_neutral_ability(k->own.ability,0)) return;
    int type=S8_MOVES[mv].type;
    const char *absorb=type==T_ELEC?"voltabsorb":type==T_WATER?"waterabsorb":type==T_FIRE?"flashfire":NULL;
    if(absorb) obs3_bit_clear(k->belief[1].abilities,obs3_ability_id(absorb));
    if(type==T_GROUND && !k->duration[O3_GRAVITY].active && !k->side[1].vol[O3_INGRAIN] &&
       !k->side[1].vol[S8C_SMACKDOWN] && !obs3_item_possible(&k->belief[1],S8_ITEM_OPERATIONS[ITEM_IRONBALL].table))
        obs3_bit_clear(k->belief[1].abilities,obs3_ability_id("levitate"));
}
static void obs3_absence_finish(Knowledge3 *k,int ix) {
    int actor=k->attack[ix].actor,mv=k->attack[ix].move;
    if(ix || k->attack[ix].context_changed || mv<0 || mv>=S8_MOVES_COUNT) return;
    Obs3Belief *b=&k->belief[1];const Obs3Mon *p=&k->side[1];
    if(!p->item_known && obs3_items_active(k,1)) {
        if(actor==1 && k->attack[ix].landed && !k->attack[ix].recoil && p->hp>0 &&
           !obs3_has_hypothesis(k,1,"magicguard") && !obs3_has_hypothesis(k,1,"sheerforce")) obs3_item_remove(b,S8_ITEM_OPERATIONS[ITEM_LIFEORB].table);
        if(k->attack[ix].body && !actor && obs3_can_eat(k,1) && obs3_damage_certified(k,mv)) {
            double eff=obs3_effectiveness(k,1,S8_MOVES[mv].type);
            for(int i=0;i<S8_NON_STONE_ITEMS_COUNT;i++) if(obs3_resist_item(i,S8_MOVES[mv].type,eff)) obs3_item_remove(b,i);
        }
        if(!actor && k->attack[ix].body && p->hp>0 && p->hp<50 && obs3_can_eat(k,1)) { obs3_item_remove(b,S8_ITEM_OPERATIONS[ITEM_SITRUS].table);obs3_item_remove(b,S8_ITEM_OPERATIONS[ITEM_ORAN].table); }
    }
}
static void obs3_replace_bounds(Obs3Belief *b,int stat,int lo,int hi) {
    if(lo==hi) { if(lo>1) lo--;hi++; }
    b->replace[stat]=1;b->lo[stat]=lo;b->hi[stat]=hi;
}
static void obs3_stat_change(Knowledge3 *k,int ix) {
    int actor=k->attack[ix].actor,mv=k->attack[ix].move;
    if(k->side[actor].attempt==PUBLIC_FAIL || k->side[actor].attempt==PUBLIC_SKIP_FAIL) return;
    Obs3Belief *b=&k->belief[1];
    if(mv==S8B_POWERTRICK && actor==1) {
        int source=b->source[1];b->source[1]=b->source[2];b->source[2]=source;
        int tmp=b->replace[1];b->replace[1]=b->replace[2];b->replace[2]=tmp;
        tmp=b->lo[1];b->lo[1]=b->lo[2];b->lo[2]=tmp;
        tmp=b->hi[1];b->hi[1]=b->hi[2];b->hi[2]=tmp;
    }
    if(!k->own.valid) return;
    if(mv==S8B_SPEEDSWAP) obs3_replace_bounds(b,5,k->own.stats[5],k->own.stats[5]);
    if(mv==S8B_POWERSPLIT || mv==S8B_GUARDSPLIT) for(int i=mv==S8B_POWERSPLIT?1:2;i<=4;i+=2) {
        int lo,hi;obs3_bounds(b,i,&lo,&hi);
        obs3_replace_bounds(b,i,(lo+k->own.stats[i])/2,(hi+k->own.stats[i])/2);
    }
}
static void obs3_belief_write(const Knowledge3 *k,Observation3 *o) {
    const Obs3Belief *b=&k->belief[1];if(!b->valid) return;
    o->v[912]=1;for(int i=0;i<7;i++) o->v[913+i]=(int32_t)b->abilities[i];
    o->v[920]=!!(b->items[0] || b->items[1] || b->items[2]);
    for(int i=0;i<3;i++) o->v[921+i]=(int32_t)b->items[i];
    uint32_t moves[16]={0};
    const Obs3Mon *p=&k->side[1];const S8Species *species=&S8_SPECIES[p->transformed?p->form:p->species];
    for(int i=0;i<species->move_count;i++) obs3_bit_set(moves,S8_LEARNSETS[species->move_offset+i]);
    int slots=0;
    for(int i=0;i<OBS3_RECORDS;i++) {
        const Obs3Record *r=&p->records[i];
        if(r->valid && r->pp_valid && r->generation==p->generation) { obs3_bit_clear(moves,r->id);slots++; }
    }
    if(slots>=4) memset(moves,0,sizeof(moves));
    o->v[924]=1;for(int i=0;i<16;i++) o->v[925+i]=(int32_t)moves[i];
    o->v[944]=1;
    for(int i=0;i<6;i++) {
        int lo,hi;obs3_bounds(b,i,&lo,&hi);
        // An epoch mapping can coalesce different parameter witnesses. Keep
        // an outward nonpoint envelope; never serialize a point estimate.
        if(lo==hi) { if(lo>1) lo--;hi++; }
        o->v[945+2*i]=lo;o->v[946+2*i]=hi;
    }
}
static void obs3_replay(Knowledge3 *k,const ObsEvent *e) {
    int a=e->a;Obs3Mon *p=&k->side[a];TurnSummary *t=&k->current;
    switch(e->kind) {
    case EV_RULES: memset(k,0,sizeof(*k));k->profile=3;k->turn=e->turn;return;
    case EV3_IDENTITY: {
        k->speed_valid=0;
        obs3_belief_identity(&k->belief[a],e->x,e->y);
        if(e->z) for(int i=0;i<S8_MEGA_LINKS_COUNT;i++) if(S8_MEGA_LINKS[i].source==e->x && S8_MEGA_LINKS[i].target==e->y) obs3_item_exact(&k->belief[a],S8_MEGA_LINKS[i].stone);
        p->species=e->x;p->form=e->y;p->mega=e->z;
        const S8Species *form=&S8_SPECIES[e->y];
        // The public Mega transition replaces the ability even when the new
        // passive ability has no Start message (e.g. Clear Body -> Tough Claws).
        if(e->z && form->form_kind==S8_FORM_MEGA && form->ability_count==1) {
            p->ability_known=1;p->ability=S8_SPECIES_ABILITIES[form->ability_offset];
            p->suppressed=0;p->changed=1;
        }
        return; }
    case EV3_TYPES: p->types[0]=e->x;p->types[1]=e->y;p->types[2]=e->z;return;
    case EV3_ABILITY:
        if(k->depth) k->attack[k->depth-1].context_changed=1;
        k->speed_valid=0;
        if(!a) {
            if(e->x>=0) k->own.ability=e->x;
            if(e->y==S8AE_SUPPRESS) k->own.suppressed=1;
            if(e->y==S8AE_RESTORE || e->z) k->own.suppressed=0;
        }
        if(e->x>=0) { p->ability_known=1;p->ability=e->x;
            memset(k->belief[a].abilities,0,sizeof(k->belief[a].abilities));obs3_bit_set(k->belief[a].abilities,e->x); }
        if(e->y==S8AE_SUPPRESS) p->suppressed=1;
        else if(e->y==S8AE_RESTORE) p->suppressed=0;
        if(e->z) { p->changed=1;p->suppressed=0; }
        return;
    case EV3_ITEM:
        k->speed_valid=0;
        if(k->depth) k->attack[k->depth-1].context_changed=1;
        obs3_item_exact(&k->belief[a],e->x);
        p->item_known=1;p->item=e->x;p->item_state=e->y;
        if(e->z) p->item_use++;else p->item_reveal++;
        if(e->x==S8_ITEM_OPERATIONS[ITEM_CLAW].table && e->z) t->claw[a]=1;
        return;
    case EV3_REVEAL: {
        Obs3Record *r=obs3_record(p,e->x);
        if(r) {
            if(r->generation!=p->generation) { r->generation=p->generation;r->lo=r->hi=p->transformed?5:S8_MOVES[e->x].pp; }
            r->pp_valid=1;
        }
        return; }
    case EV3_TRANSFORM:
        k->speed_valid=0;
        if(a==1 && k->own.valid) for(int i=1;i<6;i++) obs3_replace_bounds(&k->belief[a],i,k->own.stats[i],k->own.stats[i]);
        p->transformed=1;p->transform_from=e->x;p->form=e->x;p->generation++;
        p->ability_known=k->side[1-a].ability_known;p->ability=k->side[1-a].ability;
        p->changed=1;p->suppressed=0;
        memcpy(k->belief[a].abilities,k->belief[1-a].abilities,sizeof(k->belief[a].abilities));
        if(a && k->own.valid) { memset(k->belief[a].abilities,0,sizeof(k->belief[a].abilities));obs3_bit_set(k->belief[a].abilities,k->own.ability); }
        // Historical use/damage survives. Old PP evidence does not describe
        // replacement slots; first subsequent reveal starts the new PP range.
        for(int i=0;i<OBS3_RECORDS;i++) p->records[i].pp_valid=0;
        return;
    case EV3_VOLATILE:
        if(p->vol[e->x]!=e->y) p->since[e->x]=e->y?e->turn:0;
        p->vol[e->x]=e->y;
        if(e->x==S8C_DISABLE) p->disabled_move=e->z;
        if(e->x==S8C_ENCORE) p->encored_move=e->z;
        return;
    case EV3_RANK: p->rank[e->x]=e->y;return;
    case EV3_HAZARD: p->hazard[e->x]=e->y;return;
    case EV3_DURATION: {
        int exact=obs3_public_duration(k,a,e->x,e->y,e->z);
        obs3_duration_set(k,e->x,exact?exact:e->y,exact?exact:e->z);return; }
    case EV3_COURT:
        for(int i=0;i<5;i++) {
            Obs3Duration tmp=k->duration[O3_SIDE+i];
            k->duration[O3_SIDE+i]=k->duration[O3_SIDE+7+i];k->duration[O3_SIDE+7+i]=tmp;
        }
        for(int i=0;i<4;i++) { int tmp=k->side[0].hazard[i];k->side[0].hazard[i]=k->side[1].hazard[i];k->side[1].hazard[i]=tmp; }
        return;
    case EV3_HIT_BEGIN:
        if(k->depth) k->attack[k->depth-1].crit=0;
        return;
    case EV3_PP:
        for(int i=0;i<OBS3_RECORDS;i++) if(p->records[i].valid && p->records[i].id==e->x) {
            Obs3Record *r=&p->records[i];
            if(r->generation!=p->generation) { r->generation=p->generation;r->lo=r->hi=p->transformed?5:S8_MOVES[e->x].pp; }
            r->pp_valid=1;r->lo-=e->y;r->hi-=e->y;
            if(r->lo<0) r->lo=0;
            if(r->hi<0) r->hi=0;
        }
        return;
    case EV3_OWN_STAT: if(!a) k->own.stats[e->x]=e->y;return;
    case EV3_OWN_CONTEXT:
        if(!a) { k->own.ability=e->x;k->own.suppressed=e->y&1;k->own.used=e->y&2;k->own.item=e->z;k->own.valid=1; }return;
    case EV3_OWN_SPEED: if(!a) k->own.speed=e->x;return;
    case EV_OWN_MAXHP: if(!a) k->own_maxhp=e->x;return;
    case EV_BEGIN:
        k->turn=e->turn;memset(t,0,sizeof(*t));
        k->speed_valid=0;memset(k->end_healed,0,sizeof(k->end_healed));
        memset(k->type_uncertain,0,sizeof(k->type_uncertain));
        for(int i=0;i<2;i++) {
            k->side[i].previous_attempt=k->side[i].attempt;k->side[i].attempt=PUBLIC_NONE;
            t->hp[i].start=t->hp[i].end=k->side[i].hp;
        }
        return;
    case EV_OPPORTUNITY:
        if(!t->opportunities) { t->first=a;obs3_speed_prepare(k,a); }
        t->opportunities++;t->activity[a].outcome=e->x;return;
    case EV_USE: {
        // A public attempt suffices to retain the alternate type hypothesis;
        // failure or later consumption need not be guessed from private flags.
        if(e->x>=0 && e->x<S8_MOVES_COUNT) {
            if(!strcmp(S8_MOVES[e->x].id,"electrify")) k->type_uncertain[1-a]=1;
            if(!strcmp(S8_MOVES[e->x].id,"plasmafists")) k->type_uncertain[0]=k->type_uncertain[1]=1;
        }
        Activity *activity=&t->activity[a];
        if(activity->outcome==ACT_NONE) activity->outcome=ACT_USED;
        activity->move_valid=1;activity->move=e->x;
        if(e->y==1) { activity->call_valid=1;activity->call=e->x; }
        if(e->y==0 || e->y==3 || e->y==4) { t->nominal_valid[a]=1;t->nominal[a]=e->x; }
        int known_slot=0;
        for(int i=0;i<OBS3_RECORDS;i++) if(p->records[i].valid && p->records[i].id==e->x && p->records[i].pp_valid && p->records[i].generation==p->generation) known_slot=1;
        Obs3Record *r=obs3_record(p,e->x);
        if(r) {
            // A Copycat call does not prove that the called move occupies a
            // build slot. Preserve the public use, but do not invent its PP.
            if(r->generation!=p->generation) { r->generation=p->generation;r->lo=r->hi=p->transformed?5:S8_MOVES[e->x].pp;r->pp_valid=0; }
            if(e->y==1 && !known_slot) r->pp_valid=0;
            else if(e->y!=1) r->pp_valid=1;
            r->uses++;
            if(e->y==1) r->called++;
            else if(e->y==3) r->forced++;
            else if(e->y==4) r->cancelled++;
            else { r->spent++;if(r->lo>0) r->lo--;if(r->hi>0) r->hi--; }
        }
        p->attempt=e->y==2?PUBLIC_UNKNOWN:PUBLIC_SUCCESS;return; }
    case EV_NOTE:
        if(e->x==S8_MOVES_COUNT) { p->last=0;p->streak=0; }
        else { p->streak=p->last==e->x+1?p->streak+1:1;p->last=e->x+1; }
        return;
    case EV_HP: {
        if(e->z==HP_HEAL && e->y>e->x) k->end_healed[a]=1;
        if(k->depth && e->z==HP_RECOIL && k->attack[k->depth-1].actor==a) k->attack[k->depth-1].recoil=1;
        if(e->z==HP_HIT && k->depth && a!=k->attack[k->depth-1].actor) {
            int ix=k->depth-1; k->attack[ix].body=k->attack[ix].landed=1;
            int mv=k->attack[ix].move;
            if(mv>=0 && mv<S8_MOVES_COUNT) { obs3_absence_hit(k,k->attack[ix].actor,mv);obs3_damage_evidence(k,a,e->x,e->y); }
        }
        p->hp=e->y;
        if(e->z) { HPSummary *h=&t->hp[a];h->present=1;h->end=e->y;
            if(e->x>e->y) h->loss+=e->x-e->y;else h->gain+=e->y-e->x; }
        if(e->z!=HP_HIT || !k->depth) return;
        int ix=k->depth-1,actor=k->attack[ix].actor,mv=k->attack[ix].move;
        if(actor==a || e->x<e->y || mv<0 || mv>=S8_MOVES_COUNT || S8_MOVES[mv].category==CAT_STATUS) return;
        Obs3Record *r=obs3_record(&k->side[actor],mv);if(!r) return;
        SeenDamage *d=&r->damage;
        int before=a?100*e->x:(k->own_maxhp?(int)(10000LL*e->x/k->own_maxhp):0);
        int after=a?100*e->y:(k->own_maxhp?(int)(10000LL*e->y/k->own_maxhp):0);
        int loss=a?100*(e->x-e->y):(k->own_maxhp?(int)(10000LL*(e->x-e->y)/k->own_maxhp):0);
        if(!d->hits || loss<d->lo) d->lo=loss;
        if(!d->hits || loss>d->hi) d->hi=loss;
        d->hits++;d->crits+=k->attack[ix].crit;
        if(!r->endpoints || before<r->before_lo) r->before_lo=before;
        if(!r->endpoints || before>r->before_hi) r->before_hi=before;
        if(!r->endpoints || after<r->after_lo) r->after_lo=after;
        if(!r->endpoints || after>r->after_hi) r->after_hi=after;
        r->endpoints++;
        if(obs3_clean(k,actor,mv,k->attack[ix].crit)) {
            if(!d->clean_valid || loss<d->clean_lo) d->clean_lo=loss;
            if(!d->clean_valid || loss>d->clean_hi) d->clean_hi=loss;
            d->clean_valid=1;d->clean_hits++;
        }
        return; }
    case EV_HIT:
        if(e->x==HIT_CRIT) { t->activity[a].crit=1;if(k->depth) k->attack[k->depth-1].crit=1; }
        if(e->x==HIT_SUB && k->depth && k->attack[k->depth-1].actor==a) {
            k->attack[k->depth-1].landed=1;
            int mv=k->attack[k->depth-1].move;
            if(mv>=0 && mv<S8_MOVES_COUNT && S8_MOVES[mv].category!=CAT_STATUS) {
                Obs3Record *r=obs3_record(p,mv);if(r) { r->damage.sub_hits++;r->damage.sub_broke|=!e->y; }
            }
        }
        if(e->x==HIT_FAIL || e->x==HIT_MISS || e->x==HIT_IMMUNE) p->attempt=t->activity[a].move_valid && t->activity[a].move==MV_SUB?PUBLIC_NOT_FAIL:PUBLIC_FAIL;
        if(e->x==HIT_PROTECT) p->attempt=PUBLIC_NOT_FAIL;
        return;
    case EV_ATTACK_START:
        if(k->depth>=16) abort();
        k->attack[k->depth].actor=a;k->attack[k->depth].move=e->x;k->attack[k->depth].crit=0;
        k->attack[k->depth].future=e->y;k->attack[k->depth].saved_attempt=p->attempt;
        k->attack[k->depth].body=k->attack[k->depth].landed=k->attack[k->depth].recoil=k->attack[k->depth].context_changed=0;k->depth++;return;
    case EV_ATTACK_END:
        if(k->depth) { obs3_absence_finish(k,k->depth-1);obs3_stat_change(k,k->depth-1);k->depth--;if(k->attack[k->depth].future) k->side[k->attack[k->depth].actor].attempt=k->attack[k->depth].saved_attempt; }
        return;
    case EV_STAGE: if(e->x<7) p->rank[e->x]=clamp_stage(p->rank[e->x]+e->y);return;
    case EV_COPY: memcpy(p->rank,k->side[1-a].rank,sizeof(p->rank));return;
    case EV_HERB: for(int i=0;i<7;i++) if(p->rank[i]<0) p->rank[i]=0;return;
    case EV_SLEEP: p->status=e->x?7:0;p->status_since=e->x?e->turn:0;return;
    case EV_STATUS_START: p->status=e->x;p->status_since=e->turn;p->toxic=0;return;
    case EV_STATUS_END: p->status=p->status_since=p->toxic=0;return;
    case EV_CONFUSION_START: p->confused=1;p->confusion_since=e->turn;return;
    case EV_CONFUSION_END: p->confused=p->confusion_since=0;return;
    case EV_TOXIC_TICK: if(p->toxic<15) p->toxic++;return;
    case EV_SLEEP_TICK: t->activity[a].outcome=ACT_SLEEP;p->attempt=PUBLIC_SKIP_FAIL;return;
    case EV_SKIP:
        p->attempt=e->x==SKIP_RECHARGE?PUBLIC_SKIP_NOT_FAIL:e->x==SKIP_UNANNOUNCED?PUBLIC_UNKNOWN:e->x==SKIP_FOCUS?PUBLIC_FAIL:PUBLIC_SKIP_FAIL;
        t->activity[a].outcome=e->x==SKIP_FLINCH?ACT_FLINCH:ACT_SKIPPED;return;
    case EV_SUB:
        if(p->vol[O3_SUB]!=e->x) p->since[O3_SUB]=e->x?e->turn:0;
        p->vol[O3_SUB]=e->x;return;
    case EV_PROTECT: p->protect_streak=e->x?p->protect_streak+1:0;return;
    case EV_EXEC:
        if(e->x!=MV_PRO && e->x!=MV_END && !s8_protect_move(e->x)) p->protect_streak=0;
        return;
    case EV_FAINT: p->faint=1;return;
    case EV_MEGA: p->mega=1;return;
    case EV_WEATHER_START: k->weather=e->x;return;
    case EV_TERRAIN_START: k->terrain=e->x;return;
    case EV_WEATHER_END: k->weather=0;memset(&k->duration[O3_WEATHER],0,sizeof(Obs3Duration));return;
    case EV_TERRAIN_END: k->terrain=0;memset(&k->duration[O3_TERRAIN],0,sizeof(Obs3Duration));return;
    case EV_LEPPA: {
        Obs3Record *r=obs3_record(p,e->x);if(r && r->pp_valid) {
            r->lo+=10;r->hi+=10;int cap=p->transformed?5:S8_MOVES[e->x].pp;
            if(r->lo>cap) r->lo=cap;
            if(r->hi>cap) r->hi=cap;
        }
        return; }
    case EV_END:
        for(int i=0;i<O3_DUR_COUNT;i++) if(k->duration[i].active) k->duration[i].elapsed++;
        t->order=t->opportunities==2?(t->first?2:1):3;
        if(t->nominal_valid[0] && t->nominal_valid[1]) {
            int ma=t->nominal[0],mb=t->nominal[1];
            int pa=ma==S8_MOVES_COUNT?-1:S8_MOVES[ma].priority,pb=mb==S8_MOVES_COUNT?-1:S8_MOVES[mb].priority;
            t->relation=pa==pb?1:pa>pb?2:3;
        }
        t->qualified=t->opportunities==2 && t->relation==1 && !t->claw[0] && !t->claw[1];
        obs3_speed_finish(k);
        if(k->belief[1].valid && k->side[0].hp>0 && !k->side[0].faint && !k->side[1].faint && !k->side[1].item_known && k->side[1].hp>0 && k->side[1].hp<100 && !k->end_healed[1] &&
           !k->side[1].vol[O3_BLOCK] && obs3_items_active(k,1)) obs3_item_remove(&k->belief[1],S8_ITEM_OPERATIONS[ITEM_LEFTOVERS].table);
        k->counts[t->qualified?t->first:2]++;k->previous=*t;
        for(int i=0;i<2;i++) k->side[i].previous_attempt=k->side[i].attempt;
        return;
    case EV_TERMINAL: k->terminal=1;k->result_valid=1;k->result=e->x;k->cap=e->y;return;
    default: return; // legacy events superseded by the public v3 announcements
    }
}
// Announcements at public state boundaries. This whitelist deliberately
// excludes choice locks, ability effectState, unburden and random counters.
// Reading a timer here only produces an active/ended message, never its value.
static void obs3_public_values(const Mon *m,int v[O3_VOL_COUNT]) {
    for(int i=0;i<S8C_COUNT;i++) v[i]=!!m->control[i];
    v[O3_STOCK]=m->stockpile;v[O3_POWERTRICK]=m->power_trick;v[O3_NORETREAT]=m->no_retreat;
    v[O3_RING]=m->aquaring;v[O3_INGRAIN]=m->ingrain;v[O3_ROOST]=m->roost;
    v[O3_PARTIAL]=m->partial_trap?m->partial_move+1:0;v[O3_TRAPPED]=m->trapped;
    v[O3_SEED]=m->leech_seed;v[O3_OCTO]=m->octolock;v[O3_CURSE]=m->cursed;
    v[O3_WISH]=m->wish_pending;v[O3_RAMPAGE]=m->rampage_duration?m->rampage_move+1:0;
    v[O3_UPROAR]=!!m->uproar;v[O3_THROAT]=!!m->throatchop;v[O3_BLOCK]=!!m->healblock;
    v[O3_SUB]=!!m->sub_hp;v[O3_PROTECT]=m->protected_now;v[O3_SHIELD]=m->shield_now;
    v[O3_QUICK]=m->quick_guard;v[O3_WIDE]=m->wide_guard;v[O3_RISE]=!!m->magnet_rise;
    v[O3_CHARGE]=m->charging?m->charge_move:0;v[O3_RECHARGE]=m->recharge;
    v[O3_FOCUS]=m->focusing;v[O3_FLASH]=m->flash_fire;
}
static void obs3_duration_announce(const Mon *source,int id,int timer,int extended) {
    if(!OBS || OBS->version!=3) return;
    int seat=source==OBS->seat[1];++OBS->seq;
    for(int viewer=0;viewer<2;viewer++) {
        int relative=id;
        if(id>=O3_WISH_DUR) relative=O3_WISH_DUR+(id-O3_WISH_DUR==viewer?0:1);
        else if(id>=O3_SIDE) relative=O3_SIDE+((id-O3_SIDE)/7==viewer?0:1)*7+(id-O3_SIDE)%7;
        // Exact own knowledge is a private event; the other viewer receives
        // only public 5/8 alternatives, independently of the actual timer.
        int lo=timer?(extended && viewer!=seat?5:timer):0;
        int hi=timer?(extended && viewer!=seat?8:timer):0;
        obs_deliver(viewer,EV3_DURATION,seat,relative,lo,hi);
    }
}
static void obs3_publish(void) {
    if(!OBS || OBS->version!=3) return;
    for(int a=0;a<2;a++) {
        const Mon *m=OBS->seat[a];int v[O3_VOL_COUNT];obs3_public_values(m,v);
        int types[3]={s8_effective_type_core(m,0),s8_effective_type_core(m,1),s8_effective_type_core(m,2)};
        if(memcmp(types,OBS->published_types[a],sizeof(types))) {
            memcpy(OBS->published_types[a],types,sizeof(types));obs_emit(EV3_TYPES,m,types[0],types[1],types[2]+1);
        }
        for(int i=0;i<O3_VOL_COUNT;i++) if(OBS->published[a][i]!=v[i]) {
            OBS->published[a][i]=v[i];
            int named=v[i]?(i==S8C_DISABLE?m->disabled_move+1:i==S8C_ENCORE?m->encored_move+1:0):0;
            obs_emit(EV3_VOLATILE,m,i,v[i],named);
        }
        for(int i=0;i<7;i++) {
            int rank=i<5?m->st[i]:i==5?m->acc:m->evasion;
            if(OBS->published_rank[a][i]!=rank) { OBS->published_rank[a][i]=rank;obs_emit(EV3_RANK,m,i,rank,0); }
        }
        const int hazards[]={m->stealth_rock,m->spikes,m->toxic_spikes,m->sticky_web,m->gmax_steelsurge};
        for(int i=0;i<5;i++) if(OBS->published_hazard[a][i]!=hazards[i]) {
            OBS->published_hazard[a][i]=hazards[i];obs_emit(EV3_HAZARD,m,i,hazards[i],0);
        }
        int wish_id=O3_WISH_DUR+a;
        if(OBS->published_field[wish_id]!=!!m->wish_pending) {
            OBS->published_field[wish_id]=!!m->wish_pending;obs3_duration_announce(m,wish_id,m->wish_pending?2:0,0);
        }
        const int side[]={m->reflect,m->light_screen,m->aurora_veil,m->safeguard,m->tailwind,m->magnet_rise,m->future_turns};
        for(int i=0;i<7;i++) {
            int id=O3_SIDE+a*7+i;
            if(OBS->published_field[id]!=!!side[i]) {
                OBS->published_field[id]=!!side[i];obs3_duration_announce(m,id,side[i],i<3);
            }
        }
    }
    const int fields[]={FIELD.weather_turns,FIELD.terrain_turns,FIELD.gravity,FIELD.trick_room,FIELD.magic_room,FIELD.wonder_room,FIELD.fairy_lock};
    for(int i=0;i<7;i++) if(OBS->published_field[i]!=!!fields[i]) {
        OBS->published_field[i]=!!fields[i];
        // Weather/terrain starts are emitted in their setters with source
        // ownership. End announcements carry only the known absent value.
        if(i>1 || !fields[i]) obs3_duration_announce(OBS->seat[0],i,fields[i],0);
    }
}
static void obs3_ability(const Mon *m,int id,int event,int visibility) {
    if(!OBS || OBS->version!=3 || visibility!=S8A_PUBLIC) return;
    obs_emit(EV3_ABILITY,m,id,event,event==S8AE_REVEAL);
}
static void obs3_reveal_message(int kind,const Mon *holder,const Mon *target,int value,int detail,int silent) {
    (void)detail;(void)silent;
    if(!OBS || OBS->version!=3) return;
    if(kind==0) obs_emit(EV3_REVEAL,target,value,0,0);
    else if(kind==1) obs_emit(EV3_ITEM,target,value,1,0);
    else if(kind==2) {
        obs_emit(EV3_TRANSFORM,holder,value,0,0);
        obs_emit(EV3_TYPES,holder,holder->type1,holder->type2,holder->added_type);
    }
}
// Owner-exact context plus public field announcements. This adapter never
// calls a battle hook or queries FIELD to manufacture inference evidence.
static int obs3_owner_speed(const Mon *m,const Knowledge3 *k) {
    int ab=s8_current_ability(m);
    if(!m->ability_suppressed && !obs3_neutral_ability(ab,1)) return 0;
    int rank=m->st[4],mod=4096,item=k->duration[O3_MAGIC].active?ITEM_NONE:m->item;
    double value=m->stats[5]*(rank>=0?(2.0+rank)/2:2.0/(2-rank));
    if(item==ITEM_SCARF) mod=6144;
    if(item==ITEM_IRONBALL) mod=2048;
    if(m->tailwind) mod=obs3_chain(mod,8192);
    int integral=m->unburden || m->boost_stats_active || m->tailwind || m->status==STATUS_PAR ||
        k->duration[O3_TRICK].active || k->duration[O3_MAGIC].active || s8_weather_implemented(ab);
    if(integral) value=obs3_scale((int)value,mod);else value*=mod/4096.0;
    if(m->status==STATUS_PAR) value=(int)value/2;
    if(integral && value>10000) value=10000;
    if(k->duration[O3_TRICK].active) value=-value;
    return (int)floor(4096*value);
}
static void obs3_private(void) {
    if(!OBS || OBS->version!=3) return;
    for(int viewer=0;viewer<2;viewer++) {
        const Mon *m=OBS->seat[viewer];
        for(int i=0;i<6;i++) { ++OBS->seq;obs_deliver(viewer,EV3_OWN_STAT,viewer,i,m->stats[i],0); }
        ++OBS->seq;obs_deliver(viewer,EV3_OWN_CONTEXT,viewer,s8_current_ability(m),!!m->ability_suppressed+2*!!m->item_used,s8_item_table(m));
        ++OBS->seq;obs_deliver(viewer,EV3_OWN_SPEED,viewer,obs3_owner_speed(m,&OBS->v3[viewer]),0,0);
    }
}
static void obs3_attach(Mon *a,Mon *b) {
    memset(&OBS_STORAGE,0,sizeof(OBS_STORAGE));OBS=&OBS_STORAGE;
    OBS->seat[0]=a;OBS->seat[1]=b;OBS->version=3;OBS->turn=1;
    obs_emit(EV_RULES,a,3,0,0);
    for(int i=0;i<2;i++) {
        Mon *m=OBS->seat[i];
        obs_emit(EV3_IDENTITY,m,m->table_base_id,m->table_species_id,m->mega);
        obs_emit(EV3_TYPES,m,m->type1,m->type2,m->added_type);obs_hp(m,m->hp,0);
        ++OBS->seq;obs_deliver(i,EV_OWN_MAXHP,i,m->maxhp,0,0);
        if(m->item==ITEM_BALLOON && !m->item_used) obs_item(m,0);
    }
    obs3_private();obs_emit(EV_BEGIN,a,0,0,0);
}
static void obs3_mask(const Mon *m,const Knowledge3 *k,ActionMask *mask) {
    memset(mask,0,sizeof(*mask));if(k->terminal) return;
    int klutz=s8_current_ability(m)==S8I_KLUTZ && !m->ability_suppressed;
    int scarf=m->item==ITEM_SCARF && !m->item_used && !klutz && !k->duration[O3_MAGIC].active;
    if(scarf && m->choice_lock>=0) {
        int slot=find_move(m,m->choice_lock);
        if(slot>=0 && m->pp[slot]>0) mask->legal[slot]=1;
    }
    int locked=0;for(int i=0;i<4;i++) locked|=mask->legal[i];
    if(!locked) for(int i=0;i<m->nmoves;i++) mask->legal[i]=m->pp[i]>0;
    // Restrict only from own state and publicly revealed opposing Imprison
    // moves. No opponent Mon or hidden slot set is consulted.
    int any=0;
    for(int i=0;i<4;i++) if(mask->legal[i]) {
        int mv=m->moves[i],blocked=0;
        if(m->control[S8C_DISABLE] && mv==m->disabled_move) blocked=1;
        if(m->control[S8C_ENCORE] && mv!=m->encored_move) blocked=1;
        if(m->control[S8C_TAUNT] && S8_MOVES[mv].category==CAT_STATUS) blocked=1;
        if(m->control[S8C_TORMENT] && mv==m->last_move) blocked=1;
        if(m->throatchop && (S8_MOVES[mv].flags&S8_FLAG_SOUND)) blocked=1;
        if(mv==S8U_M_GIGATONHAMMER && m->action_move==mv+1) blocked=1;
        if((mv==S8U_M_FAKEOUT || mv==S8U_M_FIRSTIMPRESSION) && k->turn>1) blocked=1;
        if(k->side[1].vol[S8C_IMPRISON]) for(int j=0;j<OBS3_RECORDS;j++) {
            const Obs3Record *r=&k->side[1].records[j];
            if(r->valid && r->generation==k->side[1].generation && r->id==mv) blocked=1;
        }
        if(blocked) mask->legal[i]=0;
        else any=1;
    }
    mask->legal[4]=!any && !k->terminal;
}
static void obs3_build(const Mon *self,const Knowledge3 *k,Observation3 *o,ActionMask *mask) {
    memset(o,0,sizeof(*o));int n=0;
#define PUT(x) do { if(n>=OBS3_WORDS) abort();o->v[n++]=(x); } while(0)
    PUT(k->turn);PUT(3);PUT(k->terminal);PUT(S8_MOVES_COUNT);PUT(k->result_valid);PUT(k->result);PUT(k->cap);
    for(int i=0;i<3;i++) PUT(k->counts[i]);
    PUT(1);PUT(OBS3_RECORDS);PUT(O3_VOL_COUNT);PUT(O3_DUR_COUNT);
    n=O3_SELF_ID;
    PUT(1);PUT(self->table_base_id);PUT(self->table_species_id);
    for(int i=0;i<3;i++) PUT(s8_effective_type_core(self,i));
    PUT(self->sex);PUT(self->nature_up);PUT(self->nature_down);
    PUT(1);PUT(s8_current_ability(self));PUT(self->ability_suppressed);PUT(self->ability_changed);
    PUT(1);PUT(s8_item_table(self));PUT(self->item_used?2:1);
    PUT(self->mega);PUT(self->mega_mode==2 && !self->mega);PUT(self->table_mega_target);
    PUT(self->transformed);PUT(self->transformed?k->side[0].transform_from:0);
    n=O3_SELF_NUM;
    PUT(self->hp);PUT(self->maxhp);for(int i=0;i<6;i++) PUT(self->stats[i]);
    for(int i=0;i<5;i++) PUT(self->st[i]);
    PUT(self->acc);PUT(self->evasion);
    PUT(self->status);PUT(self->sleep);PUT(self->freeze_turns);PUT(self->toxic_stage);PUT(self->confusion);
    PUT(self->healblock);PUT(self->sub_hp);PUT(self->protect_streak);PUT(self->protected_now);
    PUT(self->choice_lock+1);PUT(self->last_move+1);PUT(self->metro_streak);PUT(self->attempt_prev);
    for(int i=0;i<4;i++) { int valid=i<self->nmoves;PUT(valid);PUT(valid?self->moves[i]:0);PUT(valid?self->pp[i]:0);PUT(valid?(self->transformed?(S8_MOVES[self->moves[i]].base_pp<5?S8_MOVES[self->moves[i]].base_pp:5):S8_MOVES[self->moves[i]].pp):0); }
    int vol[O3_VOL_COUNT];obs3_public_values(self,vol);for(int i=0;i<O3_VOL_COUNT;i++) PUT(vol[i]);
    for(int i=0;i<S8C_COUNT;i++) PUT(self->control[i]);
    PUT(self->stock_def);PUT(self->stock_spd);PUT(self->partial_trap);PUT(self->rampage_duration);PUT(self->rampage_true);
    PUT(self->uproar);PUT(self->throatchop);PUT(self->wish_start);PUT(self->wish_hp);PUT(self->disabled_move);PUT(self->encored_move);
    PUT(self->unburden);PUT(self->ate_berry);PUT(self->used_mask);PUT(self->boost_stats_active);PUT(self->ally_timer);PUT(self->ally_streak);
    PUT(self->dragon_cheer);PUT(self->laser_focus);PUT(self->gmax_chi_layers);PUT(self->gem);PUT(self->guard_stall);
    if(n>O3_FOE_ID) abort();
    n=O3_FOE_ID;const Obs3Mon *p=&k->side[1];
    PUT(1);PUT(p->species);PUT(p->form);
    for(int i=0;i<3;i++) PUT(i==2?p->types[i]-1:p->types[i]);
    PUT(p->mega);int candidates[2]={0,0},count=0;
    if(!p->mega && !p->transformed) for(int i=0;i<S8_MEGA_LINKS_COUNT;i++) if(S8_MEGA_LINKS[i].source==p->species) {
        if(count<2) candidates[count]=S8_MEGA_LINKS[i].target;
        count++;
    }
    PUT(count>0);PUT(count);PUT(candidates[0]);PUT(candidates[1]);PUT(p->transformed);PUT(p->transformed?p->transform_from:0);
    PUT(p->vol[S8C_DISABLE]>0);PUT(p->vol[S8C_DISABLE]?p->disabled_move-1:0);
    PUT(p->vol[S8C_ENCORE]>0);PUT(p->vol[S8C_ENCORE]?p->encored_move-1:0);
    n=O3_FOE_VISIBLE;
    PUT(p->hp);PUT(p->faint);for(int i=0;i<7;i++) PUT(p->rank[i]);
    PUT(p->status);PUT(p->status?k->turn-p->status_since:0);PUT(p->toxic);
    PUT(p->confused);PUT(p->confused?k->turn-p->confusion_since:0);
    PUT(p->protect_streak);PUT(p->last);PUT(p->streak);PUT(p->previous_attempt);
    for(int i=0;i<O3_VOL_COUNT;i++) { PUT(p->vol[i]);PUT(p->vol[i]?k->turn-p->since[i]:0); }
    // Public fixed control durations, separately from the elapsed-only
    // random timers. Never copy the target's actual control counters.
    for(int i=0;i<S8C_COUNT;i++) {
        int total=S8_CONTROL_MOVES[i].duration;
        int remaining=p->vol[i] && total?total-(k->turn-p->since[i]):0;
        PUT(remaining>0?remaining:0);
    }
    int uproar=p->vol[O3_UPROAR]?3-(k->turn-p->since[O3_UPROAR]):0;PUT(uproar>0?uproar:0);
    if(n!=O3_FIELD) abort();
    n=O3_FIELD;
    // Weather/terrain identity is stored as the public START event payload.
    PUT(k->weather);PUT(k->terrain);
    for(int i=0;i<O3_DUR_COUNT;i++) {
        const Obs3Duration *d=&k->duration[i];int elapsed=d->active?d->elapsed:0;
        int lo=d->active?d->lo-elapsed:0,hi=d->active?d->hi-elapsed:0;
        PUT(d->active);PUT(d->active?d->mode:0);PUT(elapsed);PUT(lo>0?lo:0);PUT(hi>0?hi:0);
    }
    for(int a=0;a<2;a++) for(int i=0;i<5;i++) PUT(k->side[a].hazard[i]);
    if(n>O3_RECORDS) abort();
    n=O3_RECORDS;
    for(int a=0;a<2;a++) for(int i=0;i<OBS3_RECORDS;i++) {
        const Obs3Record *r=&k->side[a].records[i];const SeenDamage *d=&r->damage;
        PUT(r->valid);PUT(r->id);PUT(r->uses);PUT(r->spent);PUT(r->called);PUT(r->forced);PUT(r->cancelled);
        PUT(r->pp_valid);PUT(r->pp_valid?r->lo:0);PUT(r->pp_valid?r->hi:0);PUT(d->hits);PUT(d->lo);PUT(d->hi);PUT(d->crits);
        PUT(d->sub_hits);PUT(d->sub_broke);PUT(d->clean_valid);PUT(d->clean_lo);PUT(d->clean_hi);PUT(d->clean_hits);
        PUT(r->before_lo);PUT(r->before_hi);PUT(r->after_lo);PUT(r->after_hi);PUT(r->endpoints);PUT(r->generation);
    }
    if(n!=O3_CONFIRMED) abort();
    PUT(p->ability_known);PUT(p->ability_known?p->ability:0);PUT(p->suppressed);PUT(p->changed);
    PUT(p->item_known);PUT(p->item_known?p->item:0);PUT(p->item_state);PUT(p->item_reveal);PUT(p->item_use);
    obs3_belief_write(k,o); // frozen 912..959, reserved seams remain zero
    obs3_mask(self,k,mask);n=O3_CONFIRMED+10;for(int i=0;i<5;i++) PUT(mask->legal[i]);
    n=O3_SUMMARY;
    for(int previous=0;previous<2;previous++) {
        const TurnSummary *t=previous?&k->previous:&k->current;
        PUT(t->order);PUT(t->relation);PUT(t->qualified);PUT(t->opportunities);
        for(int a=0;a<2;a++) { PUT(t->nominal_valid[a]);PUT(t->nominal[a]);PUT(t->claw[a]); }
        for(int a=0;a<2;a++) {
            const Activity *ac=&t->activity[a];const HPSummary *h=&t->hp[a];
            PUT(ac->move_valid);PUT(ac->move);PUT(ac->call_valid);PUT(ac->call);PUT(ac->outcome);PUT(ac->crit);
            PUT(h->present);PUT(h->start);PUT(h->end);PUT(h->loss);PUT(h->gain);
        }
    }
    if(n!=OBS3_WORDS) abort();
#undef PUT
}
static inline void obs3_serialize(const Observation3 *o,unsigned char bytes[OBS3_BYTES]) {
    memcpy(bytes,"MGO3",4);for(int j=0;j<4;j++) bytes[4+j]=(unsigned char)((uint32_t)OBS3_WORDS>>(8*j));
    for(int i=0;i<OBS3_WORDS;i++) for(int j=0;j<4;j++) bytes[8+4*i+j]=(unsigned char)((uint32_t)o->v[i]>>(8*j));
}
static inline int obs3_deserialize(const unsigned char *bytes,size_t size,Observation3 *o) {
    if(size!=OBS3_BYTES || memcmp(bytes,"MGO3",4)) return 0;
    uint32_t count=0;for(int j=0;j<4;j++) count|=(uint32_t)bytes[4+j]<<(8*j);
    if(count!=OBS3_WORDS) return 0;
    for(int i=0;i<OBS3_WORDS;i++) {
        uint32_t value=0;for(int j=0;j<4;j++) value|=(uint32_t)bytes[8+4*i+j]<<(8*j);
        o->v[i]=value<=INT32_MAX?(int32_t)value:(int32_t)(-1-(int64_t)(UINT32_MAX-value));
    }
    return 1;
}
static int obsdemo3(const Observation3 *o,const ActionMask *mask) {
    (void)o;for(int i=0;i<5;i++) if(mask->legal[i]) return i;return -1;
}
static int obsdemo3_mega(const Observation3 *o) { return !o->v[32] && o->v[33]; }

// ---------- Item helpers ----------
// Mainline-style fixed-point item modifier, rounding exact halves down.
static int item_mod(int value, int numerator) {
    return (int)(((long)value * numerator + 2047) / 4096);
}
// Raw identity, including a foreign stone; Magic Room suppresses effects only.
static int s8_item_table(const Mon *m) {
    if(m->item==ITEM_NONE) return -1;
    if(m->item==ITEM_STONE && m->spec_v3) return m->table_item_id;
    return S8_ITEM_OPERATIONS[m->item].table;
}
static int s8_stone_matches(const Mon *m,int table) {
    if(table<0 || !S8_ITEMS[table].is_stone) return 0;
    int base=m->spec_v3?m->table_base_id:S8_METAGROSS;
    for(int i=0;i<S8_MEGA_LINKS_COUNT;i++)
        if(S8_MEGA_LINKS[i].source==base && S8_MEGA_LINKS[i].stone==table) return 1;
    return 0;
}
static int s8_item_takeable(const Mon *m) { return m->item!=ITEM_NONE && !s8_stone_matches(m,s8_item_table(m)); }
static void s8_item_clear(Mon *m) {
    m->item=ITEM_NONE;m->table_item_id=-1;m->item_used=0;m->choice_lock=-1;m->crit_bonus=0;
}
static void s8_item_set(Mon *m,int item,int table) {
    m->item=item;m->table_item_id=table;m->item_used=0;m->choice_lock=-1;m->crit_bonus=item==ITEM_LENS;
    obs_emit(EV_TRANSFER,m,item,1,0);
    s8_seed_check(m); // setItem invokes the new item's Start on an active holder
}
static void s8_items_after_use(Mon *,int);
static void s8_item_spend_boost(Mon *m,int recyclable,const int delta[7]) {
    int item=m->item,eaten=recyclable && !m->fling_item && S8_ITEM_OPERATIONS[item].berry;
    if(eaten) s8_ability_event(m,S8AE_EAT_ITEM,S8_EFFECT_SOURCE,m,S8_STATUS_EFFECT,1,item);
    if(recyclable && !m->fling_item && S8_ITEM_OPERATIONS[m->item].berry) m->ate_berry=1;
    if(recyclable) { m->last_item=m->item;m->last_item_table=s8_item_table(m); }
    m->item_used=1;obs_item(m,1);
    if(delta) {
        int saved_item=S8_ITEM_EFFECT,saved_boost=S8_BOOST_EFFECT;
        const MoveDef *saved_move=S8_STATUS_EFFECT;Mon *saved_source=S8_EFFECT_SOURCE;
        S8_ITEM_EFFECT=item;S8_BOOST_EFFECT=-1;S8_STATUS_EFFECT=NULL;S8_EFFECT_SOURCE=m;
        s8_change_boost(m,m,delta);
        S8_ITEM_EFFECT=saved_item;S8_BOOST_EFFECT=saved_boost;
        S8_STATUS_EFFECT=saved_move;S8_EFFECT_SOURCE=saved_source;
    }
    if(field_member(m) && FIELD.item_context) s8_item_clear(m);
    s8_items_after_use(m,item);
}
static void s8_item_spend(Mon *m,int recyclable) {
    s8_item_spend_boost(m,recyclable,NULL);
}
static void s8_seed_check(Mon *m) {
    if(!field_member(m) || m->hp<=0 || m->item_used) return;
    int item=held_item(m);
    for(size_t i=0;i<sizeof(S8_SEED_ITEMS)/sizeof(*S8_SEED_ITEMS);i++) {
        const S8SeedItem *row=&S8_SEED_ITEMS[i];
        if(row->item!=item || row->terrain!=FIELD.terrain) continue;
        s8_item_enable(FIELD.seat[0],FIELD.seat[1]);
        int delta[7]={0};delta[row->stat]=1;
        s8_item_spend_boost(m,1,delta);return;
    }
}
static void s8_item_enable(Mon *a,Mon *b) {
    if(FIELD.item_context) return;
    FIELD.item_context=1;
    Mon *ms[]={a,b};
    for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if(m->item_used) {
            if(!m->last_item && m->item!=ITEM_BALLOON) { m->last_item=m->item;m->last_item_table=s8_item_table(m); }
            s8_item_clear(m);
        }
    }
}
static int s8_item_take(Mon *m,Mon *source,const MoveDef *move) {
    if(!s8_item_takeable(m)) return 0;
    int item=m->item;
    if(!s8_ability_event(m,S8AE_TAKE_ITEM,source?source:m,m,move,1,item)) return 0;
    s8_item_clear(m);return item;
}
static void s8_items_after_use(Mon *m,int item) {
    m->used_item_this_turn=1;
    s8_ability_event(m,S8AE_AFTER_USE_ITEM,m,m,NULL,1,item);
    int side=s8_resist_side(m);
    if(side<0) return;
    S8AbilityCall calls[5];int n=0;
    for(int i=0;i<FIELD.power_ally_count[side];i++) {
        Mon *ally=FIELD.power_allies[side][i];
        if(!ally || ally==m || ally->hp<=0 || !s8_has_ability(ally,S8I_SYMBIOSIS)) continue;
        for(unsigned j=0;j<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);j++) {
            const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[j];
            if(row->ability==S8I_SYMBIOSIS && row->event==S8AE_AFTER_USE_ITEM)
                calls[n++]=(S8AbilityCall){ally,row,eff_speed(ally),0};
        }
    }
    S8AbilityEvent e={S8AE_AFTER_USE_ITEM,m,m,NULL,1,item};
    s8_ability_run_queue(calls,n,&e,BATTLE_RNG);
}
static int s8_item_exchange(Mon *a,Mon *b) {
    int ai=a->item,bi=b->item,at=s8_item_table(a),bt=s8_item_table(b);
    if((!ai && !bi) || (ai && !s8_item_takeable(a)) || (bi && !s8_item_takeable(b)) ||
       s8_stone_matches(a,bt) || s8_stone_matches(b,at)) return 0;
    // Trick/Switcheroo's TryImmunity precedes both TakeItem callbacks.
    if(s8_has_ability(b,S8I_STICKYHOLD) && !s8_property_ignored(b) && !FIELD.ignore_breakable) return 0;
    const MoveDef *move=S8_STATUS_EFFECT;
    int own=ai?s8_item_take(a,a,move):0,other=bi?s8_item_take(b,a,move):0;
    if((ai && !own) || (bi && !other)) {
        if(own) { a->item=ai;a->table_item_id=at; }
        if(other) { b->item=bi;b->table_item_id=bt; }
        return 0;
    }
    s8_item_set(a,bi,bt);s8_item_set(b,ai,at);return 1;
}
static int trick_stone_blocked(const Mon *a, const Mon *b) {
    if(a->spec_v3 || b->spec_v3 || (field_member(a) && FIELD.item_context))
        return s8_stone_matches(a,s8_item_table(a)) || s8_stone_matches(b,s8_item_table(b)) ||
               s8_stone_matches(a,s8_item_table(b)) || s8_stone_matches(b,s8_item_table(a));
    return (a->mega && a->mega_mode!=2) || (b->mega && b->mega_mode!=2) ||
           a->item == ITEM_STONE || b->item == ITEM_STONE;
}
static double eff_speed(const Mon *m) {
    // New stat mechanics use integral getStat ordering. Frozen battles never
    // activate this context, preserving legacy fractional Speed and RNG.
    if(m->unburden || s8_has_ability(m,S8I_KLUTZ) || m->boost_stats_active || s8_weather_implemented(s8_current_ability(m)) || m->tailwind || (field_member(m) && FIELD.magic_room)) return s8_speed_stat(m);
    double s = m->stats[5] * smult(m->st[4]);
    // Once paralysis is active, use the pinned getStat/finalModify integer
    // rounding before its final Speed halving. The legacy no-status path stays exact.
    if(m->status==STATUS_PAR) {
        int speed=(int)floor(s);
        if(held_item(m)==ITEM_SCARF) speed=item_mod(speed,6144);
        if(held_item(m)==ITEM_IRONBALL) speed=item_mod(speed,2048);
        return speed/2;
    }
    if (held_item(m) == ITEM_SCARF) s *= 1.5;        // Choice Scarf
    if (held_item(m) == ITEM_IRONBALL) s *= 0.5;     // Iron Ball
    return s;
}
// Sitrus / Oran Berry: heal once when HP is at or below half (blocked by Heal Block).
static void berry_hp_check(Mon *m) {
    if (m->hp <= 0 || m->item_used || m->healblock > 0) return;
    if((held_item(m)!=ITEM_SITRUS && held_item(m)!=ITEM_ORAN) || m->hp>m->maxhp/2 || !s8_items_try_eat(m)) return;
    if (held_item(m) == ITEM_SITRUS && m->hp <= m->maxhp / 2) {
        int before=m->hp; m->hp += s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,(m->maxhp+(field_member(m) && FIELD.item_context && !FIELD.item_ability_context?2:0))/4,ITEM_SITRUS); if (m->hp > m->maxhp) m->hp = m->maxhp; s8_item_spend(m,1); obs_hp(m,before,HP_HEAL);
    } else if (held_item(m) == ITEM_ORAN && m->hp <= m->maxhp / 2) {
        int before=m->hp; m->hp += s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,10,ITEM_ORAN); if (m->hp > m->maxhp) m->hp = m->maxhp; s8_item_spend(m,1); obs_hp(m,before,HP_HEAL);
    }
}
// White Herb: once, restore all negative stages (incl. accuracy) to 0.
static void white_herb_check(Mon *m) {
    if (held_item(m) != ITEM_HERB || m->item_used) return;
    int neg = (m->acc < 0 || m->evasion < 0);
    for (int i = 0; i < 5; i++) if (m->st[i] < 0) neg = 1;
    if (!neg) return;
    for (int i = 0; i < 5; i++) if (m->st[i] < 0) m->st[i] = 0;
    if (m->acc < 0) m->acc = 0;
    if (m->evasion < 0) m->evasion = 0;
    s8_item_spend(m,1); obs_emit(EV_HERB,m,0,0,0);
}
// Leppa Berry: once, when a move's PP reaches 0, restore 10 PP to it.
static int s8_move_max_pp(const Mon *m,int slot) {
    int pp=m->transformed?S8_MOVES[m->moves[slot]].base_pp:move_def(m->moves[slot])->pp;
    return m->transformed && pp>5?5:pp;
}
static void leppa_check(Mon *m, int slot) {
    if (slot < 0 || held_item(m) != ITEM_LEPPA || m->item_used) return;
    if ((!FIELD.item_ability_context || m->hp>0) && m->pp[slot] == 0 && s8_items_try_eat(m)) {
        m->pp[slot] += s8_has_ability(m,S8I_RIPEN)?20:10;
        int mx = s8_move_max_pp(m,slot);
        if (m->pp[slot] > mx) m->pp[slot] = mx;
        s8_item_spend(m,1); obs_emit(EV_LEPPA,m,m->moves[slot],0,0);
    }
}
// Attacker-side effects of landing a damaging move (apply even vs a substitute).
static void post_hit_attacker(Mon *me, int dmg) {
    if (dmg <= 0 || me->hp <= 0) return;
    if (held_item(me) == ITEM_SHELLBELL && me->healblock == 0) {
        int heal = dmg / 8;                              // Shell Bell: 512/4096 of damage, minimum 1 HP
        if (heal < 1) heal = 1;
        int before=me->hp; me->hp += heal; if (me->hp > me->maxhp) me->hp = me->maxhp;
        if(me->hp!=before) { obs_item(me,0); obs_hp(me,before,HP_HEAL); }
    }
    if (held_item(me) == ITEM_LIFEORB && s8_resist_loss(me,1,S8R_OTHER)) { int before=me->hp; me->hp -= s8_resist_loss(me,me->maxhp / 10,S8R_OTHER); obs_item(me,0); obs_hp(me,before,HP_RECOIL); }   // Life Orb recoil
}
// Bookkeeping after a mon actually uses a move: Metronome streak + Choice lock.
// metro_streak counts consecutive uses of last_move INCLUDING the latest one,
// so the next use of the same move boosts by 20% x min(streak, 5).
static void note_move_used(Mon *m, int mv) {
    obs_emit_action(EV_NOTE,m,(MoveAction){ACTION_MOVE,mv},0);
    if (m->last_move == mv) m->metro_streak++;
    else { m->last_move = mv; m->metro_streak = 1; }
    if (held_item(m) == ITEM_SCARF) m->choice_lock = mv;
}
// G3 state never crosses the frozen MGO1 boundary. These helpers are also the
// harness seam for statuses with no source in Metagross's learnset.
static void clear_major_status(Mon *m) {
    if(m->status) obs2_emit(EV_STATUS_END,m,m->status,0,0);
    m->status=STATUS_NONE; m->freeze_turns=0; m->toxic_stage=0;
    if(m->sleep) { m->sleep=0; obs_emit(EV_SLEEP,m,0,0,0); }
}
static int s8_selective_status_matches(const Mon *m,const S8StatusItem *row) {
    return (m->status>STATUS_NONE && m->status<=STATUS_BRN && (row->status_mask&(1u<<m->status))) ||
        (row->confusion && m->confusion);
}
static void s8_selective_status_cure(Mon *m,const S8StatusItem *row) {
    if(m->status>STATUS_NONE && m->status<=STATUS_BRN && (row->status_mask&(1u<<m->status)))
        clear_major_status(m);
    if(row->confusion) {
        if(m->confusion) obs2_emit(EV_CONFUSION_END,m,0,0,0);
        m->confusion=0;
    }
}
static const int S8_MENTAL_CONTROLS[]={S8C_ATTRACT,S8C_TAUNT,S8C_ENCORE,S8C_TORMENT,S8C_DISABLE};
static int s8_mental_present(const Mon *m) {
    for(size_t i=0;i<sizeof(S8_MENTAL_CONTROLS)/sizeof(*S8_MENTAL_CONTROLS);i++)
        if(m->control[S8_MENTAL_CONTROLS[i]]) return 1;
    return m->healblock>0;
}
static void s8_mental_cure(Mon *m) {
    for(size_t i=0;i<sizeof(S8_MENTAL_CONTROLS)/sizeof(*S8_MENTAL_CONTROLS);i++)
        m->control[S8_MENTAL_CONTROLS[i]]=0;
    m->disabled_move=m->encored_move=-1;
    // CONTROL volatiles (including Attract's -end) have no event in MGO1/2.
    // Keep their existing representation; Heal Block has an existing end event.
    if(m->healblock) { m->healblock=0;obs_emit(EV_BLOCK,m,0,0,0); }
}
static void status_berry_check(Mon *m) {
    if(m->hp<=0 || m->item_used) return;
    const S8StatusItem *row=s8_status_item(held_item(m));
    if(row) {
        if(row->operation.berry) {
            if(s8_selective_status_matches(m,row) && s8_items_try_eat(m)) {
                s8_item_spend(m,1);s8_selective_status_cure(m,row);
            }
        } else if(s8_mental_present(m)) {
            s8_mental_cure(m);s8_item_spend(m,1);
        }
        return;
    }
    if(held_item(m)==ITEM_LUM && (m->status || m->sleep || m->confusion) && s8_items_try_eat(m)) {
        s8_item_spend(m,1); clear_major_status(m); if(m->confusion) obs2_emit(EV_CONFUSION_END,m,0,0,0); m->confusion=0;
    } else if(held_item(m)==ITEM_CHESTO && m->sleep && s8_items_try_eat(m)) {
        s8_item_spend(m,1); clear_major_status(m);
    }
}
static int has_type(const Mon *m, int type) { return s8_effective_type(m,0)==type || s8_effective_type(m,1)==type || s8_effective_type(m,2)==type; }
// The generated masks describe callback predicates; source/effect/weather remain
// event inputs. onAlly guards include the holder in this one-Pokemon format.
static const S8Guard *s8_guard_row(const Mon *m) {
    int id=s8_current_ability(m);
    for(unsigned i=0;i<sizeof(S8_GUARDS)/sizeof(*S8_GUARDS);i++)
        if(S8_GUARDS[i].ability==id) return &S8_GUARDS[i];
    return NULL;
}
static const unsigned char *s8_guard_move(const MoveDef *move) {
    if(move) for(int i=0;i<S8_MOVES_COUNT;i++)
        if(!strcmp(move->code,S8_MOVES[i].code)) return S8_GUARD_MOVES[i];
    static const unsigned char empty[3]={0};return empty;
}
static void s8_guard_callback(const Mon *holder,S8AbilityEvent *e) {
    const S8Guard *g=s8_guard_row(holder);if(!g) return;
    const unsigned char *move=s8_guard_move(e->move);
    int id=g->ability;Mon *m=(Mon *)holder;
    int condition=g->condition==1?has_type(holder,T_GRASS):g->condition==2?current_weather(holder)==WEATHER_SUN:1;
    if(e->event==S8AE_SET_STATUS) {
        if(!condition || !(g->status_mask&(1<<e->detail))) return;
        if(g->condition==1 && (!e->source || e->source==holder || S8_YAWN_END ||
           (!e->move && !S8_STATUS_CAUSE))) return;
        e->value=0;
        if(id==S8G_SWEETVEIL || (g->condition==1?
           (S8_STATUS_CAUSE && !strcmp(S8_STATUS_CAUSE,"synchronize")) || (move[0] && !move[2]):move[0]))
            s8_ability_record(holder,e->event,S8A_PUBLIC);
    } else if(e->event==S8AE_TRY_VOLATILE) {
        if(condition && (g->volatile_mask&(1u<<e->detail))) {
            e->value=0;
            if(id!=S8G_INNERFOCUS && id!=S8G_OWNTEMPO && (id!=S8G_AROMAVEIL || e->move))
                s8_ability_record(holder,e->event,S8A_PUBLIC);
        }
    } else if(e->event==S8AE_STATUS_IMMUNITY) {
        if((id==S8G_MAGMAARMOR && e->detail==STATUS_FRZ) ||
           (id==S8G_OBLIVIOUS && e->detail==64+S8C_ATTRACT)) e->value=0;
    } else if(e->event==S8AE_UPDATE) {
        if(holder->hp<=0) return;
        if((holder->sleep && (g->cure_mask&1)) || (holder->status && (g->cure_mask&(1<<holder->status)))) {
            s8_ability_record(holder,e->event,S8A_PUBLIC);clear_major_status(m);
        }
        if(holder->confusion && (g->cure_volatile&(1u<<S8G_CONFUSION))) {
            s8_ability_record(holder,e->event,S8A_PUBLIC);m->confusion=0;obs2_emit(EV_CONFUSION_END,m,0,0,0);
        }
        for(int k=0;k<S8C_COUNT;k++) if(m->control[k] && (g->cure_volatile&(1u<<k))) {
            s8_ability_record(holder,e->event,S8A_PUBLIC);m->control[k]=0;
        }
    } else if(e->event==S8AE_TRY_BOOST) {
        if(g->boost==1 && has_type(holder,T_GRASS) && holder!=e->source && e->value<0) e->value=0;
        if(g->boost==2 && S8_BOOST_EFFECT==S8_HIT_INTIMIDATE && e->detail==0 && e->value) {
            e->value=0;s8_ability_record(holder,e->event,S8A_PUBLIC);
        }
    } else if(e->event==S8AE_CRITICAL_HIT) e->value=0;
    else if(e->event==S8AE_MODIFY_SECONDARIES) { if(!e->detail) e->value=0; }
    else if(e->event==S8AE_TRY_HIT) {
        if(!e->move) return;
        const char *id=s8_power_move(e->move)->id;
        if(!id) id=e->move->code; // synthetic event fixtures / moves outside the admitted vocabulary
        for(unsigned i=0;i<sizeof(S8_GUARD_TRY_HIT_IDS)/sizeof(*S8_GUARD_TRY_HIT_IDS);i++)
            if(!strcmp(id,S8_GUARD_TRY_HIT_IDS[i])) {
                e->value=0;s8_ability_record(holder,e->event,S8A_PUBLIC);break;
            }
    } else if(e->event==S8AE_HIT) {
        if(move[1]) s8_ability_record(holder,e->event,S8A_PUBLIC);
    } else if((e->event==S8AE_ATK || e->event==S8AE_SPA) && e->move && e->move->type==T_GHOST)
        e->value=damage_chain(e->value,2048);
}
static int s8_guard_volatile(Mon *m,int kind) {
    if(!s8_ability_event(m,S8AE_STATUS_IMMUNITY,S8_EFFECT_SOURCE,m,S8_STATUS_EFFECT,1,64+kind)) return 0;
    return s8_ability_event(m,S8AE_TRY_VOLATILE,S8_EFFECT_SOURCE,m,S8_STATUS_EFFECT,1,kind);
}
static void s8_guard_flinch(Mon *m) {
    if(m->hp>0 && !m->flinched && s8_guard_volatile(m,S8G_FLINCH)) m->flinched=1;
}
static int s8_guard_secondary(const Mon *target,int self) {
    return s8_ability_event(target,S8AE_MODIFY_SECONDARIES,S8_EFFECT_SOURCE,target,S8_STATUS_EFFECT,1,self);
}
static int inflict_status(Mon *m, int status, Rng *r) {
    if(m->hp<=0 || m->status || m->sleep || s8_status_prevented(m,0,0)) return 0;
    if((status==STATUS_PAR && has_type(m,T_ELEC)) ||
       (status==STATUS_FRZ && (has_type(m,T_ICE) || current_weather(m)==WEATHER_SUN)) ||
       ((status==STATUS_PSN || status==STATUS_TOX) &&
        !(S8_EFFECT_SOURCE && s8_has_ability(S8_EFFECT_SOURCE,S8G_CORROSION)) && (has_type(m,T_STEEL) || has_type(m,T_POISON))) ||
       (status==STATUS_BRN && has_type(m,T_FIRE))) return 0;
    if(status<=STATUS_NONE || status>STATUS_BRN) return 0;
    if(!s8_ability_event(m,S8AE_STATUS_IMMUNITY,S8_EFFECT_SOURCE,m,S8_STATUS_EFFECT,1,status) ||
       !s8_ability_event(m,S8AE_SET_STATUS,S8_EFFECT_SOURCE,m,S8_STATUS_EFFECT,1,status)) return 0;
    (void)r; // non-sleep major statuses do not draw on Start
    m->status=status; m->freeze_turns=status==STATUS_FRZ?3:0; m->toxic_stage=0;
    m->state_features|=MON_POST_V1_STATE;
    obs2_emit(EV_STATUS_START,m,status,0,0);
    s8_ability_event(m,S8AE_AFTER_STATUS,S8_EFFECT_SOURCE,m,S8_STATUS_EFFECT,1,status);
    status_berry_check(m); // Lum's AfterSetStatus runs before the next action
    return 1;
}
static int inflict_sleep(Mon *m, Rng *r) {
    if(m->hp<=0 || m->status || m->sleep || s8_status_prevented(m,1,0)) return 0;
    if(!s8_ability_event(m,S8AE_SET_STATUS,S8_EFFECT_SOURCE,m,S8_STATUS_EFFECT,1,0)) return 0;
    static const int turns[]={2,3,3}; // champions slp.onStart sample
    m->sleep=turns[rng_int(r,3)];
    m->state_features|=MON_POST_V1_STATE;
    obs_emit(EV_SLEEP,m,1,12,0); // public support {2,3}, never the private draw
    status_berry_check(m);
    return 1;
}
static int inflict_confusion_min(Mon *m, int minimum, Rng *r) {
    if(m->hp<=0 || m->confusion || s8_status_prevented(m,0,1)) return 0;
    if(!s8_guard_volatile(m,S8G_CONFUSION)) return 0;
    m->confusion=minimum+rng_int(r,6-minimum); // Axe Kick random(3,6), others random(2,6)
    m->state_features|=MON_POST_V1_STATE;
    obs2_emit(EV_CONFUSION_START,m,0,0,0);
    status_berry_check(m); // Lum's Update also cures confusion
    return 1;
}
static int inflict_confusion(Mon *m, Rng *r) { return inflict_confusion_min(m,2,r); }
static void status_damaging_hit(Mon *m, int type, int category, int polar_flare) {
    if(m->hp>0 && m->status==STATUS_FRZ && type==T_FIRE && category!=CAT_STATUS && !polar_flare)
        clear_major_status(m);
}
// No move in the 68 has defrost/thawsTarget. G4/G5 need no additions here;
// future species pass the resolved defrost flag (Burn Up requires Fire typing).
static int freeze_before_move(Mon *m, int defrost, int burn_up, Rng *r) {
    if(m->status!=STATUS_FRZ) return 1;
    if(defrost && (!burn_up || has_type(m,T_FIRE))) return 1;
    if(--m->freeze_turns<=0 || rng_dbl(r)<0.25) { clear_major_status(m); return 1; }
    return 0;
}
static void status_modify_move(Mon *m, int defrost) {
    if(defrost && m->status==STATUS_FRZ) clear_major_status(m);
}
static int s8_selected_defrost(const Mon *m, int slot) {
    if(slot<0 || slot>=m->nmoves) return 0;
    if(m->moves[slot]==S8TA_M_BURNUP) return has_type(m,T_FIRE);
    const S8StatusMove *row=s8_resolved_status_move(m->moves[slot]);
    return row && row->defrost;
}
static void status_residual(Mon *m) {
    if(m->hp<=0) return;
    int loss=0;
    if(m->status==STATUS_PSN) loss=m->maxhp/8;
    if(m->status==STATUS_BRN) loss=m->maxhp/16;
    if(m->status==STATUS_TOX) {
        obs2_emit(EV_TOXIC_TICK,m,0,0,0);
        if(m->toxic_stage<15) m->toxic_stage++;
        loss=(m->maxhp/16>0?m->maxhp/16:1)*m->toxic_stage;
    }
    if(!loss && (m->status==STATUS_PSN || m->status==STATUS_BRN)) loss=1;
    loss=s8_resist_loss(m,loss,m->status==STATUS_BRN?S8R_BURN:S8R_POISON);
    if(loss) { int before=m->hp; m->hp-=loss; obs_hp(m,before,HP_RECOIL); berry_hp_check(m); }
}
static void s8_stateful_residual(Mon *a,Mon *b,int order);
static void s8_field_residual(Mon *a,Mon *b,int order);
static void s8_trap_residual(Mon *a,Mon *b,int order);
static void s8_control_residual(Mon *a,Mon *b,int order);
static void s8_field_change(S8AbilityEventId event,const Mon *source);
static void s8_weather_residual(Mon *,Mon *,int);
static int s8_weather_context(const Mon *,const Mon *);
static void end_of_turn_core(Mon *A, Mon *B) {
    Mon *ms[2] = {A, B};
    if(s8_trap_battle_ended(A,B)) return;
    if(field_member(A) && FIELD.weather_turns && !--FIELD.weather_turns) {
        obs2_emit(EV_WEATHER_END,A,0,0,0);
        FIELD.weather=WEATHER_NONE; A->weather=B->weather=WEATHER_NONE;
        FIELD.weather_source=NULL;s8_field_change(S8AE_WEATHER_CHANGE,A);
    }
    int weather_first=0;
    if(current_weather(A)!=WEATHER_NONE && (s8_has_ability(A,S8R_DRYSKIN) || s8_has_ability(B,S8R_DRYSKIN) || s8_has_ability(A,S8V3_SOLARPOWER) || s8_has_ability(B,S8V3_SOLARPOWER) || s8_has_ability(A,S8V3_RAINDISH) || s8_has_ability(B,S8V3_RAINDISH) || s8_has_ability(A,S8V3_ICEBODY) || s8_has_ability(B,S8V3_ICEBODY))) {
        double sa=eff_speed(A),sb=eff_speed(B);
        if(sb>sa || (sb==sa && BATTLE_RNG && rng_int(BATTLE_RNG,2))) weather_first=1;
    }
    for(int i=0;i<2;i++) {
        Mon *m=ms[i^weather_first];int before=m->hp;
        s8_ability_event(m,S8AE_WEATHER_TICK,m,m,NULL,0,s8_field_weather(m));
        if(m->hp<before) { obs_hp(m,before,HP_RECOIL);queue_faint(m,FAINT_DAMAGE);berry_hp_check(m); }
        if(s8_trap_battle_ended(A,B)) return;
    }
    if(current_weather(A)==WEATHER_SAND) for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if(m->hp>0 && s8_semi(m)!=S8U_M_DIG && s8_semi(m)!=S8U_M_DIVE && !has_type(m,T_ROCK) && !has_type(m,T_GROUND) && !has_type(m,T_STEEL)) {
            int loss=m->maxhp/16; if(loss<1) loss=1;
            loss=s8_resist_loss(m,loss,S8R_SAND);if(!loss) continue;
            int before=m->hp; m->hp-=loss; obs_hp(m,before,HP_RECOIL); berry_hp_check(m);
            if(s8_trap_battle_ended(A,B)) return;
        }
    }
    if(current_weather(A)==WEATHER_HAIL) for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if(m->hp>0 && s8_semi(m)!=S8U_M_DIG && s8_semi(m)!=S8U_M_DIVE && !has_type(m,T_ICE)) {
            int loss=s8_resist_loss(m,m->maxhp/16>0?m->maxhp/16:1,S8R_HAIL);
            if(loss) { int before=m->hp;m->hp-=loss;obs_hp(m,before,HP_RECOIL);berry_hp_check(m); }
            if(s8_trap_battle_ended(A,B)) return;
        }
    }
    if(s8_trap_battle_ended(A,B)) return;
    if(A->hp>0 && B->hp>0 && (A->future_turns || B->future_turns)) future_residual(A,B,BATTLE_RNG);
    if(s8_trap_battle_ended(A,B)) return;
    s8_stateful_residual(A,B,4); // Wish, after Future Sight, before Leftovers (5)
    if(s8_weather_context(A,B)) s8_weather_residual(A,B,5);
    for (int i = 0; i < 2; i++) {
        Mon *m = ms[i];
        m->flinched = 0;
        if (!s8_weather_context(A,B) && m->hp > 0 && held_item(m) == ITEM_LEFTOVERS && m->healblock == 0) {
            int before=m->hp; m->hp += s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,m->maxhp/16,ITEM_LEFTOVERS);
            if (m->hp > m->maxhp) m->hp = m->maxhp;
            if(m->hp!=before) { obs_item(m,0); obs_hp(m,before,HP_HEAL); }
        }
    }
    if(!s8_weather_context(A,B)) s8_field_residual(A,B,5); // legacy order
    s8_stateful_residual(A,B,6); // Aqua Ring
    s8_stateful_residual(A,B,7); // Ingrain
    s8_trap_residual(A,B,8); // Leech Seed, before poison/burn
    if(s8_trap_battle_ended(A,B)) return;
    for(int order=9;order<=10;order++) for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if((order==9 && (m->status==STATUS_PSN || m->status==STATUS_TOX)) ||
           (order==10 && m->status==STATUS_BRN)) status_residual(m);
        if(s8_trap_battle_ended(A,B)) return;
    }
    s8_trap_residual(A,B,12); // Curse
    if(s8_trap_battle_ended(A,B)) return;
    if(FIELD.control_context) s8_control_residual(A,B,13);
    else s8_trap_residual(A,B,13); // partial trapping
    if(s8_trap_battle_ended(A,B)) return;
    if(FIELD.control_context) s8_control_residual(A,B,14);
    else s8_trap_residual(A,B,14); // Octolock
    if(s8_trap_battle_ended(A,B)) return;
    for(int order=15;order<=17;order++) s8_control_residual(A,B,order);
    for(int i=0;i<2;i++) if(ms[i]->magnet_rise && !--ms[i]->magnet_rise) obs2_emit(EV_RISE_END,ms[i],0,0,0); // 18
    s8_control_residual(A,B,22); // Throat Chop, before Yawn
    s8_control_residual(A,B,23); // Yawn
    if(s8_trap_battle_ended(A,B)) return;
    s8_control_residual(A,B,24); // Perish Song
    if(s8_trap_battle_ended(A,B)) return;
    for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if(m->laser_focus) m->laser_focus--; // copied Laser Focus restarts at duration 2
        m->roost=0; // residual 25, after status/weather damage
        if(m->reflect && !--m->reflect) obs2_emit(EV_SCREEN_END,m,0,0,0); // side residual 26.1
        if(m->light_screen && !--m->light_screen) obs2_emit(EV_SCREEN_END,m,1,0,0); // 26.2
        if(m->safeguard) m->safeguard--; // side residual 26.3
        if(m->tailwind) m->tailwind--; // 26.5
        if(m->aurora_veil) m->aurora_veil--; // 26.10
        m->endured_now=0;
    }
    s8_field_residual(A,B,27); // Trick Room (27.1), Gravity (27.2), rooms, terrain
    if(FIELD.item_ability_context) s8_items_residual(A,B,28);
    else if(s8_weather_context(A,B)) s8_weather_residual(A,B,28);
    else s8_control_residual(A,B,28); // Uproar 28.1
    s8_boost_phase(A,B,S8AE_RESIDUAL,-1,0);
    s8_control_residual(A,B,0); // default order: Lock-On, Electrify, lockedmove
    A->gem=B->gem=0; // gem duration 1, default residual order
}

static void s8_protect_residual(Mon *a,Mon *b) {
    Mon *guards[]={a,b};
    for(int i=0;i<2;i++) {
        Mon *m=guards[i];
        m->shield_now=m->quick_guard=m->wide_guard=0;
        if(FIELD.guard_context && m->guard_stall && !--m->guard_stall) m->protect_streak=0;
    }
}
static void end_of_turn(Mon *a,Mon *b) {
    FIELD.ability_residual_speed[0]=eff_speed(a);FIELD.ability_residual_speed[1]=eff_speed(b);
    // Residual handlers capture holder Speed before any residual Speed drops.
    if(FIELD.control_context) {
        FIELD.control_residual_active=1;
        FIELD.control_residual_speed[0]=s8_speed_stat(a);
        FIELD.control_residual_speed[1]=s8_speed_stat(b);
    }
    s8_ability_phase(a,b,S8AE_RESIDUAL,-1,0);
    s8_ability_phase(a,b,S8AE_UPDATE,-1,0);
    if(s8_status_item(a->item)) status_berry_check(a);
    if(s8_status_item(b->item)) status_berry_check(b);
    FIELD.item_residual=1;
    end_of_turn_core(a,b);
    FIELD.item_residual=0;
    s8_protect_residual(a,b);
    Mon *turn_mons[]={a,b};
    for(int i=0;i<2;i++) {
        turn_mons[i]->beak_hot=0;
        if(turn_mons[i]->ally_timer && !--turn_mons[i]->ally_timer) turn_mons[i]->ally_streak=0;
    }
    FIELD.control_residual_active=0;
}

// ---------- Damage ----------
// Champions damage hooks. Each hook chains 4096-based modifiers first, then
// applies one rounding operation (ties down). Chaining itself rounds ties up.
static int damage_chain(int modifier, int next) {
    return (int)(((int64_t)modifier*next+2048)/4096);
}
static int damage_stat(int stat, int stage) {
    stage=clamp_stage(stage);
    int value=stage>=0 ? (int)((int64_t)stat*(2+stage)/2) : (int)((int64_t)stat*2/(2-stage));
    return value<1?1:value;
}
static int s8_terrain_power_modifier(const Mon *atk,const Mon *def,int mvi) {
    if(!field_member(atk)) return 4096;
    int type=move_def(mvi)->type;
    if(FIELD.terrain==TERRAIN_MISTY && type==T_DRAGON && is_grounded(def)) return 2048;
    if(FIELD.terrain==TERRAIN_GRASSY && is_grounded(def) && s8_semi(def)<0 && (mvi==S8F_EARTHQUAKE || mvi==S8F_BULLDOZE)) return 2048;
    if(is_grounded(atk) && (FIELD.terrain==TERRAIN_GRASSY || s8_semi(atk)<0) && ((FIELD.terrain==TERRAIN_ELECTRIC && type==T_ELEC) ||
       (FIELD.terrain==TERRAIN_PSYCHIC && type==T_PSY) || (FIELD.terrain==TERRAIN_GRASSY && type==T_GRASS))) return 5325;
    return 4096;
}
static int damage_power_with_item(const Mon *atk, const MoveDef *mv,int terrain,int expanding,int knock,const Mon *def) {
    int modifier=4096;
    // BasePower event priorities: Tough Claws 21, band/glasses 16, type item 15.
    modifier=s8_power_damage_event_range(atk,def,S8AE_BASE_POWER,mv,modifier,mv->power,17,INT_MAX);
    if((mv->cat==CAT_PHYS && held_item(atk)==ITEM_MUSCLE) ||
       (mv->cat==CAT_SPEC && held_item(atk)==ITEM_WISE)) modifier=damage_chain(modifier,4505);
    if(s8_type_boost_matches(held_item(atk),mv->type))
        modifier=damage_chain(modifier,4915);
    if(atk->gem) modifier=damage_chain(modifier,5325); // BasePower priority 14
    if(atk->control[S8C_CHARGE] && mv->type==T_ELEC) modifier=damage_chain(modifier,8192); // priority 9
    modifier=s8_power_damage_event_range(atk,def,S8AE_BASE_POWER,mv,modifier,mv->power,7,8);
    modifier=damage_chain(modifier,terrain); // terrain priority 6, after items
    if(expanding) modifier=damage_chain(modifier,6144); // Expanding Force priority 0
    if(knock) modifier=damage_chain(modifier,6144); // Knock Off priority 0, after terrain
    // Facade onBasePower priority -2, after item/ability hooks; includes freeze.
    if((mv==&MOVES[MV_FAC] || (mv==&S8_CONTROL_OVERRIDE && S8_CONTROL_OVERRIDE_ID==MV_FAC)) && atk->status && !atk->sleep) modifier=damage_chain(modifier,8192);
    int power=item_mod(mv->power,modifier);
    return power<1?1:power;
}
static int damage_power_with_terrain(const Mon *atk,const MoveDef *mv,int terrain,int expanding) {
    return damage_power_with_item(atk,mv,terrain,expanding,0,NULL);
}
static inline int damage_power(const Mon *atk,const MoveDef *mv) { return damage_power_with_terrain(atk,mv,4096,0); }
static double damage_effectiveness(const Mon *def, int mvi) {
    // Frozen legacy behavior: awake ST falls through to a power-0 damage
    // attempt. It historically used the partial chart's neutral Normal row.
    // Preserve that compatibility quirk without altering any real attack row.
    if(mvi==MV_ST) return 1.0;
    int attack=move_def(mvi)->type;
    if(attack<0) return 1; // move-local typeless Struggle
    if(attack==T_GROUND && !is_grounded(def)) return 0;
    double total=1;
    for(int i=0;i<3;i++) {
        int type=s8_effective_type(def,i);
        double eff=attack==T_GROUND && type==T_FLY?1:eff_vs(attack,type);
        if(type==T_GHOST && (attack==T_NORMAL || attack==T_FIGHT) && S8_PROPERTY && S8_PROPERTY->scrappy) eff=1;
        if(mvi==S8TA_M_FREEZEDRY && type==T_WATER) eff=2;
        if(mvi==S8TA_M_FLYINGPRESS) eff*=eff_vs(T_FLY,type);
        total*=eff;
    }
    if(total>0 && move_def(mvi)->cat!=CAT_STATUS) {
        const Mon *source=S8_PROPERTY?S8_PROPERTY->user:NULL;
        total=s8_ability_event(def,S8AE_EFFECTIVENESS,source,def,move_def(mvi),(int)(total*4096),0)/4096.0;
    }
    return total;
}
static int damage_berry(const Mon *def, const MoveDef *mv, double eff) {
    // New berries protect the body. Sound moves carry bypasssub in the mod;
    // Infiltrator is an action-local property, including called moves.
    for(size_t i=0;i<sizeof(S8_RESIST_BERRIES)/sizeof(*S8_RESIST_BERRIES);i++) {
        const S8ResistBerry *row=&S8_RESIST_BERRIES[i];
        if(held_item(def)!=row->item || mv->type!=row->type) continue;
        if(def->item_used || eff<=0 || (row->super_effective && eff<=1) || s8_items_blocked(def)) return 0;
        if(def->sub_hp>0 && !(s8_power_move(mv)->flags&S8_FLAG_BYPASSSUB) &&
           !(S8_PROPERTY && S8_PROPERTY->infiltrates)) return 0;
        return 1;
    }
    // Preserve the legacy eligibility/consumption path, including Substitute.
    return eff>1 && !def->item_used && !s8_items_blocked(def) &&
        ((held_item(def)==ITEM_SHUCA && mv->type==T_GROUND) || (held_item(def)==ITEM_CHOPLE && mv->type==T_FIGHT) ||
         (held_item(def)==ITEM_KASIB && mv->type==T_GHOST) || (held_item(def)==ITEM_PAYAPA && mv->type==T_PSY));
}
static int damage_final_modifier(const Mon *atk, int mvi, double eff, int berry, int screen) {
    int modifier=screen;
    if(held_item(atk)==ITEM_LIFEORB) modifier=damage_chain(modifier,5324);
    if(eff>1 && held_item(atk)==ITEM_BELT) modifier=damage_chain(modifier,4915);
    if(held_item(atk)==ITEM_METRO && atk->last_move==mvi) {
        static const int MOD[6]={4096,4915,5734,6553,7372,8192};
        int k=atk->metro_streak>5?5:atk->metro_streak;
        modifier=damage_chain(modifier,MOD[k]);
    }
    if(berry) modifier=damage_chain(modifier,2048);
    return modifier;
}
static int s8_power_damage_item_active(const Mon *holder) {
    int item=held_item(holder);
    return item==ITEM_LIFEORB || item==ITEM_BELT || item==ITEM_METRO;
}
static void s8_power_item_damage(const Mon *holder,S8AbilityEvent *e) {
    if(!e->move) return;
    for(int i=0;i<S8_MOVES_COUNT;i++) if(!strcmp(e->move->code,S8_MOVES[i].code)) {
        double eff=e->target?damage_effectiveness(e->target,i):1;
        e->value=damage_final_modifier(holder,i,eff,0,e->value);return;
    }
}
// Action-local hook inputs retained from G0. G3 supplies burn from Mon status;
// future groups supply weather/screens after resolving field state.
typedef struct { int weather, screen, burned; } DamageHooks;
static const DamageHooks DAMAGE_NEUTRAL={4096,4096,0};
static int damage_modifiers(int base, int crit, int roll, int stab, double eff,
                            const DamageHooks *hooks, int final_modifier) {
    base=item_mod(base,hooks->weather);                 // WeatherModifyDamage
    if(crit) base=base*3/2;                             // truncation, not modify()
    base=(int)((int64_t)base*roll/100);                  // truncation, not modify()
    if(stab) base=item_mod(base,stab==1?6144:stab);       // old Boolean API or 4096-based STAB
    base=(int)(base*eff);                               // type: double / truncate halves
    if(hooks->burned) base=item_mod(base,2048);          // burn is its own rounding point
    base=item_mod(base,final_modifier);                 // ModifyDamage hook chain
    return base<1?1:base;
}
// Dataset weights in hectograms: Metagross 550.0 kg; Mega 942.9 kg.
static int mon_weight(const Mon *m) {
    int weight;
    if(m->spec_v3) {
        weight=m->weight_hg;
        // 8b-2 stored the original Metagross Light Metal result. Form data is
        // exact, including odd weights; current ability changes never cache it.
        if(!m->ability_changed && m->table_species_id==S8_METAGROSS && m->ability==AB_LM)
            weight=S8_SPECIES[m->table_species_id].weight_hg;
    } else weight=S8_SPECIES[m->mega?S8_METAGROSS_MEGA:S8_METAGROSS].weight_hg;
    weight=s8_ability_event(m,S8AE_WEIGHT,m,m,NULL,weight,0);
    return weight>0?weight:1;
}
static int grass_knot_power(int weight) {
    return weight>=2000?120:weight>=1000?100:weight>=500?80:weight>=250?60:weight>=100?40:20;
}
static int move_base_power(const Mon *atk,const Mon *def,int mv) {
    if(mv==MV_TAN) return tantrum_failed(atk)?150:75;
    if(mv==MV_RND && atk->round_chained) return 120;
    if(mv==MV_GYRO) {
        double speed=eff_speed(atk), foe_speed=eff_speed(def);
        int power=speed>0?(int)floor(25*foe_speed/speed)+1:1;
        return power>150?150:power;
    }
    if(mv==MV_HP) {
        int64_t ratio=(int64_t)def->hp*4096/def->maxhp;
        int power=(int)((100*(100*ratio)+2047)/4096/100);
        return power>0?power:1;
    }
    if(mv==MV_GK) return grass_knot_power(mon_weight(def));
    return move_def(mv)->power;
}
// getStat('spe') uses integral stages and chained item/status modifiers;
// eff_speed's historical fractional no-status ordering remains frozen.
static int s8_speed_stat(const Mon *m) {
    int speed=damage_stat(m->stats[5],m->st[4]), modifier=4096;
    if(s8_weather_implemented(s8_current_ability(m))) modifier=s8_ability_event(m,S8AE_SPEED,m,m,NULL,modifier,0);
    if(held_item(m)==ITEM_SCARF) modifier=damage_chain(modifier,6144);
    if(held_item(m)==ITEM_IRONBALL) modifier=damage_chain(modifier,2048);
    if(m->tailwind) modifier=damage_chain(modifier,8192);
    if(!s8_weather_implemented(s8_current_ability(m))) modifier=s8_ability_event(m,S8AE_SPEED,m,m,NULL,modifier,0);
    speed=item_mod(speed,modifier);
    if(m->status==STATUS_PAR && !s8_has_ability(m,S8V3_QUICKFEET)) speed/=2; // finalModify, then floor(50%)
    return speed>10000?10000:speed;
}
static int S8_HIT_INDEX=1, S8_POWER_PROC;
static int s8_base_power(const Mon *a,const Mon *b,int mv) {
    int power=move_def(mv)->power;
    switch(mv) {
    case S8V_ACROBATICS: return a->item==ITEM_NONE || a->item_used?power*2:power;
    case S8V_ASSURANCE: return b->hurt_this_turn?power*2:power;
    case S8V_AVALANCHE: return a->attacked_positive?power*2:power;
    case S8V_BEATUP: return 5+S8_SPECIES[a->spec_v3?a->table_base_id:S8_METAGROSS].base_stats[1]/10;
    case S8V_ELECTROBALL: {
        int denominator=s8_speed_stat(b), ratio=denominator?s8_speed_stat(a)/denominator:0;
        static const int bp[]={40,60,80,120,150}; return bp[ratio>4?4:ratio];
    }
    case S8V_ERUPTION: case S8V_WATERSPOUT: {
        int bp=(int)((int64_t)power*a->hp/a->maxhp); return bp>0?bp:1;
    }
    case S8V_FLAIL: case S8V_REVERSAL: {
        int ratio=(int)((int64_t)a->hp*48/a->maxhp);
        return ratio<2?200:ratio<5?150:ratio<10?100:ratio<17?80:ratio<33?40:20;
    }
    case S8V_HEATCRASH: case S8V_HEAVYSLAM: {
        int64_t aw=mon_weight(a), bw=mon_weight(b);
        return aw>=bw*5?120:aw>=bw*4?100:aw>=bw*3?80:aw>=bw*2?60:40;
    }
    case S8V_HEX: case S8V_INFERNALPARADE: return b->status || b->sleep?power*2:power;
    case S8V_LASTRESPECTS: return 50; // living single member: totalFainted=0
    case S8V_LOWKICK: return grass_knot_power(mon_weight(b));
    case S8V_PAYBACK: return field_member(b) && !FIELD.consumed[b==FIELD.seat[1]] && FIELD.committed[b==FIELD.seat[1]]>=0?power:power*2;
    case S8V_POWERTRIP: case S8V_STOREDPOWER:
        for(int i=0;i<5;i++) if(a->st[i]>0) power+=20*a->st[i];
        if(a->acc>0) power+=20*a->acc;
        return power;
    case S8V_RAGEFIST: return a->times_attacked>=6?350:50+50*a->times_attacked;
    case S8V_RISINGVOLTAGE: return field_member(b) && FIELD.terrain==TERRAIN_ELECTRIC && is_grounded(b)?power*2:power;
    case S8V_TEMPERFLARE: return tantrum_failed(a)?power*2:power;
    case S8V_TRIPLEAXEL: return power*S8_HIT_INDEX;
    default: return power;
    }
}
static int s8_resolved_power(const Mon *a,const Mon *b,int mv) {
    const MoveDef *m=move_def(mv);
    int power=s8_base_power(a,b,mv), modifier=4096;
    modifier=s8_power_damage_event_range(a,b,S8AE_BASE_POWER,m,modifier,power,17,INT_MAX);
    if((m->cat==CAT_PHYS && held_item(a)==ITEM_MUSCLE) || (m->cat==CAT_SPEC && held_item(a)==ITEM_WISE)) modifier=damage_chain(modifier,4505);
    if(s8_type_boost_matches(held_item(a),m->type)) modifier=damage_chain(modifier,4915);
    if(a->gem) modifier=damage_chain(modifier,5325); // BasePower priority 14
    if(a->control[S8C_CHARGE] && m->type==T_ELEC) modifier=damage_chain(modifier,8192); // priority 9
    modifier=s8_power_damage_event_range(a,b,S8AE_BASE_POWER,m,modifier,power,7,8);
    modifier=damage_chain(modifier,s8_terrain_power_modifier(a,b,mv));
    int poison=b->status==STATUS_PSN || b->status==STATUS_TOX;
    if(((mv==S8V_BARBBARRAGE || mv==S8V_VENOSHOCK) && poison) || (mv==S8V_LASHOUT && a->stats_lowered_turn) ||
       (mv==S8V_FICKLEBEAM && S8_POWER_PROC)) modifier=damage_chain(modifier,8192);
    if((mv==S8V_GRAVAPPLE && current_gravity(a)) || (mv==S8V_MISTYEXPLOSION && field_member(a) &&
        FIELD.terrain==TERRAIN_MISTY && is_grounded(a))) modifier=damage_chain(modifier,6144);
    power=item_mod(power,modifier); return power>0?power:1;
}
static void s8_damaging_hit(Mon *target,int category,int damage) {
    if(!field_member(target)) return;
    int planned=FIELD.committed[target==FIELD.seat[1]];
    if(planned==S8V_COUNTER && category==CAT_PHYS) { target->counter_valid=1; target->counter_damage=damage*2; }
    if(planned==S8V_MIRRORCOAT && category==CAT_SPEC) { target->coat_valid=1; target->coat_damage=damage*2; }
}
static void s8_got_attacked(Mon *target,int damage,int hits) {
    target->last_hit_damage=damage; target->attacked_turn=1;
    target->attacked_positive|=damage>0; target->times_attacked+=hits;
}
static int s8_ohko(int mv) { return mv==S8V_FISSURE || mv==S8V_GUILLOTINE || mv==S8V_HORNDRILL || mv==S8V_SHEERCOLD; }
// Admitted builds are level 50. Keep the level condition explicit and testable.
static int s8_ohko_chance(const Mon *a,const Mon *b,int mv,int al,int bl) {
    if(al<bl || (mv==S8V_SHEERCOLD && has_type(b,T_ICE))) return -1;
    return (mv==S8V_SHEERCOLD && !has_type(a,T_ICE)?20:30)+al-bl;
}
static int s8_fixed_damage(Mon *a,Mon *b,int mv) {
    if(s8_ohko(mv)) return b->maxhp;
    switch(mv) {
    case S8V_NIGHTSHADE: case S8V_SEISMICTOSS: return 50;
    case S8V_SUPERFANG: return b->hp/2>0?b->hp/2:1;
    case S8V_ENDEAVOR: return b->hp-a->hp;
    case S8V_COUNTER: return a->counter_damage>0?a->counter_damage:1;
    case S8V_MIRRORCOAT: return a->coat_damage>0?a->coat_damage:1;
    case S8V_COMEUPPANCE: case S8V_METALBURST: return a->last_hit_damage>0?a->last_hit_damage*3/2:1;
    case S8V_FINALGAMBIT: {
        int hp=a->hp; a->hp=0; queue_faint(a,FAINT_ATTEMPT_COST); obs_hp(a,hp,HP_COST); return hp;
    }
    default: return -1;
    }
}
static int LIVE_DAMAGE;
static int resolved_damage_power(const Mon *atk,const Mon *def,int mv) {
    if(mv==S8B_SPITUP) {
        MoveDef move=*move_def(mv); move.power=100*atk->stockpile;
        return damage_power_with_item(atk,&move,4096,0,0,def);
    }
    if(!LIVE_DAMAGE) return damage_power_with_item(atk,move_def(mv),4096,0,0,def);
    if(s8_variable_supported(mv)) return s8_resolved_power(atk,def,mv);
    int terrain=s8_terrain_power_modifier(atk,def,mv);
    int expanding=mv==MV_EF && field_member(atk) && FIELD.terrain==TERRAIN_PSYCHIC && is_grounded(atk);
    int dynamic=mv==MV_TAN || mv==MV_GYRO || mv==MV_HP || mv==MV_GK || (mv==MV_RND && atk->round_chained);
    const S8TurnMove *turn=s8_turn_move(mv);
    int weather=current_weather(atk);
    if(turn && turn->weak_weather && (weather==WEATHER_RAIN || weather==WEATHER_SAND || weather==WEATHER_SNOW)) terrain=damage_chain(terrain,2048);
    terrain=damage_chain(terrain,s8_semi_multiplier(def,mv,1));
    if(mv==S8IS_M_KNOCKOFF) return damage_power_with_item(atk,move_def(mv),terrain,expanding,s8_item_takeable(def),def);
    if(mv==S8IS_M_FLING) {
        MoveDef move=*move_def(mv);
        move.power=S8_ITEM_OPERATIONS[atk->fling_item?atk->fling_item:atk->item].power;
        return damage_power_with_item(atk,&move,terrain,expanding,0,def);
    }
    if(!dynamic) return damage_power_with_item(atk,move_def(mv),terrain,expanding,0,def);
    MoveDef move=*move_def(mv); move.power=move_base_power(atk,def,mv);
    return damage_power_with_item(atk,&move,terrain,expanding,0,def);
}

static int weather_damage_modifier(const Mon *atk,const Mon *def, const MoveDef *mv) {
    // WeatherModifyDamage priorityEvent: Mega Sol (1) exits before the field
    // weather (0), including in rain or with Cloud Nine present.
    if(s8_has_ability(atk,S8W_MEGASOL))
        return s8_ability_event(atk,S8AE_WEATHER_DAMAGE,atk,def,mv,4096,0);
    int weather=s8_field_weather(def);
    if((weather==WEATHER_SUN && mv->type==T_FIRE) || (weather==WEATHER_RAIN && mv->type==T_WATER)) return 6144;
    if((weather==WEATHER_SUN && mv->type==T_WATER) || (weather==WEATHER_RAIN && mv->type==T_FIRE)) return 2048;
    return 4096;
}
static int damage_value(const Mon *atk, const Mon *def, int mvi, int crit, int roll,
                        const DamageHooks *hooks, int berry) {
    const MoveDef *mv=move_def(mvi);
    int ai=mv->cat==CAT_PHYS?(mvi==MV_BP?2:1):3, di=mv->cat==CAT_PHYS?2:4;
    if(move_traits(mvi)->defensive_stat) di=move_traits(mvi)->defensive_stat;
    if(mvi==MV_BP && field_member(atk) && FIELD.wonder_room) ai=4; // onModifyMove
    const Mon *offensive=mvi==S8B_FOULPLAY?def:atk;
    int as=s8_property_rank(offensive,ai-1,offensive->st[ai-1]), ds=s8_property_rank(def,di-1,def->st[di-1]);
    if(mvi==S8B_DARKESTLARIAT || mvi==S8B_SACREDSWORD) ds=0;
    if(crit && as<0) as=0;
    if(crit && ds>0) ds=0;
    int raw_ai=ai,raw_di=di;
    if(field_member(atk) && FIELD.wonder_room) {
        if(ai==2 || ai==4) raw_ai=6-ai;
        if(di==2 || di==4) raw_di=6-di;
    }
    int attack=damage_stat(offensive->stats[raw_ai],as), defense=damage_stat(def->stats[raw_di],ds);
    int defmod=4096;
    int defense_weather=s8_has_ability(atk,S8W_MEGASOL)?WEATHER_SUN:current_weather(def);
    if((di==2 && defense_weather==WEATHER_SNOW && has_type(def,T_ICE)) ||
       (di==4 && defense_weather==WEATHER_SAND && has_type(def,T_ROCK))) defmod=6144;
    if(mv->cat==CAT_PHYS && s8_has_ability(atk,S8Q_HUSTLE)) attack=s8_ability_event(atk,S8AE_ATK,atk,def,mv,attack,1);
    attack=item_mod(attack,s8_power_damage_event(atk,def,mv->cat==CAT_PHYS?S8AE_ATK:S8AE_SPA,mv,4096,0));
    defmod=s8_ability_event(def,di==2?S8AE_DEF:S8AE_SPD,atk,def,mv,defmod,0);
    defense=item_mod(defense,defmod);
    if(defense<1) defense=1;
    if(attack<1) attack=1;
    int power=resolved_damage_power(atk,def,mvi);
    int base=(int)((int64_t)22*power*attack/defense/50)+2;
    if(S8_PROPERTY && S8_PROPERTY->parental && S8_PROPERTY->hit>1) base=item_mod(base,1024);
    double eff=damage_effectiveness(def,mvi);
    if(eff<=0) return 0;
    DamageHooks actual=*hooks;
    if(def->aurora_veil || (mv->cat==CAT_PHYS && def->reflect) || (mv->cat==CAT_SPEC && def->light_screen)) actual.screen=2048;
    int weather_mod=weather_damage_modifier(atk,def,mv);
    if(weather_mod!=4096) actual.weather=weather_mod;
    int modifier=s8_power_damage_event(atk,def,S8AE_DAMAGE,mv,4096,crit|(berry<<1)|(eff>1?4:0));
    if(!crit && !(S8_PROPERTY && S8_PROPERTY->infiltrates)) modifier=damage_chain(modifier,actual.screen);
    modifier=damage_chain(modifier,s8_semi_multiplier(def,mvi,2));
    if(def->control[S8C_GLAIVERUSH]) modifier=damage_chain(modifier,8192);
    if(def->control[S8C_MINIMIZE] && (S8_MOVES[mvi].flags&S8_FLAG_MINIMIZE)) modifier=damage_chain(modifier,8192);
    if(atk->status==STATUS_BRN && mv->cat==CAT_PHYS && mvi!=MV_FAC)
        actual.burned=s8_ability_event(atk,S8AE_BURN,atk,def,mv,1,0);
    int stab=mv->type>=0 && has_type(atk,mv->type)?6144:4096;
    stab=s8_ability_event(atk,S8AE_STAB,atk,def,mv,stab,0);
    int result=damage_modifiers(base,crit,roll,stab,eff,&actual,modifier);
    if(S8_PROPERTY && S8_PROPERTY->bypass) {
        result=item_mod(result,1024);if(result<1) result=1;
    }
    return result;
}
static double damage_crit_probability(const Mon *atk,const Mon *def, int mvi) {
    static const double CRITP[4]={1.0/24.0,1.0/8.0,1.0/2.0,1.0};
    if(!s8_ability_event(def,S8AE_CRITICAL_HIT,atk,def,move_def(mvi),1,0)) return 0;
    if(atk->laser_focus || (mvi>=MV_COUNT && S8_MOVES[mvi].will_crit)) return 1.0;
    int stage=move_traits(mvi)->crit_bonus+(held_item(atk)==ITEM_LENS)+2*!!atk->control[S8C_FOCUSENERGY]+(atk->dragon_cheer==2?2:!!atk->dragon_cheer)+atk->gmax_chi_layers;
    if(held_item(atk)==ITEM_LEEK && (s8_item_base_species(atk,"farfetchd") || s8_item_base_species(atk,"sirfetchd"))) stage+=2;
    stage=s8_ability_event(atk,S8AE_CRIT,atk,def,move_def(mvi),stage,0);
    return CRITP[stage>3?3:stage<0?0:stage];
}
static int calc_move_damage(Mon *atk, Mon *def, int mvi, Rng *r) {
    const MoveDef *mv=move_def(mvi);
    // Same two draws, crit then roll, as the historical engine. Retain the
    // splitmix roll mapping 85+draw%16; only arithmetic order changes.
    int crit=rng_dbl(r)<damage_crit_probability(atk,def,mvi);
    if(S8_HIT_CONTEXT) S8_HIT_CONTEXT->crit=crit;
    if(crit) obs_emit(EV_HIT,atk,HIT_CRIT,0,0);
    double eff=damage_effectiveness(def,mvi);
    obs_emit(EV_HIT,atk,HIT_EFFECT,eff<1?0:eff==1?1:2,0);
    int berry=damage_berry(def,mv,eff);
    if(berry) { berry=s8_items_try_eat(def);if(berry) s8_item_spend(def,1); }
    int roll=85+rng_int(r,16);
    LIVE_DAMAGE=1;
    S8_ITEM_DAMAGE_EXEC=1;
    int damage=damage_value(atk,def,mvi,crit,roll,&DAMAGE_NEUTRAL,berry);
    S8_ITEM_DAMAGE_EXEC=0;
    LIVE_DAMAGE=0; return damage;
}

// Frozen legacy pointer API; new execution uses table IDs without pointer arithmetic.
static int calc_damage(Mon *atk, Mon *def, const MoveDef *mv, Rng *r) {
    return calc_move_damage(atk,def,(int)(mv-MOVES),r);
}

// BeforeMove priorities: freeze/sleep 10, flinch 8 (in caller), confusion 3,
// paralysis 1. Called sleep-usable moves reach this only after the sleep tick.
static int status_before_move(Mon *m, Rng *r) {
    int was_confused=m->confusion>0;
    if(m->confusion && --m->confusion>0 && rng_dbl(r)<0.33) {
        int attack=damage_stat(m->stats[1],m->st[0]), defense=damage_stat(m->stats[2],m->st[1]);
        int base=(int)((int64_t)22*40*attack/defense/50)+2;
        // Separate typeless calculator: no crit, burn, STAB, ability or item mods.
        int damage=(int)((int64_t)(base&65535)*(100-rng_int(r,16))/100);
        int before=m->hp; m->hp-=damage>0?damage:1;
        obs2_emit(EV_SKIP,m,SKIP_CONFUSION,0,0);
        obs_hp(m,before,HP_HIT); berry_hp_check(m); attempt_result(m,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(m); m->charging=0; return 0;
    }
    if(was_confused && !m->confusion) obs2_emit(EV_CONFUSION_END,m,0,0,0);
    if(m->control[S8C_ATTRACT] && rng_dbl(r)<0.5) { attempt_result(m,ATTEMPT_SKIP_FAIL); return 0; }
    if(m->status==STATUS_PAR && rng_dbl(r)<0.125) { obs2_emit(EV_SKIP,m,SKIP_PARALYSIS,0,0); attempt_result(m,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(m); m->charging=0; return 0; }
    return 1;
}

static int S8_CALL_DEPTH;
static int CUR_SECOND = 0;   // set by the battle loops: 1 when the acting mon moves second this turn
static int s8_control_accuracy(const Mon *a,const Mon *b,const MoveDef *m) {
    if(s8_boost_any((Mon *)a,(Mon *)b,S8AE_ALWAYS_ACCURACY,m,0,0)) return 1;
    if(a->control[S8C_LOCKON] || b->control[S8C_GLAIVERUSH]) return 1;
    if(b->control[S8C_MINIMIZE]) for(int i=0;i<S8_MOVES_COUNT;i++)
        if(!strcmp(m->code,S8_MOVES[i].code)) return !!(S8_MOVES[i].flags&S8_FLAG_MINIMIZE);
    return 0;
}
static int s8_boost_accuracy_check(Mon *a,Mon *b,const MoveDef *m,Rng *r,int ignore_evasion,int first) {
    MoveDef adjusted;int weather=current_weather(b);
    if((!strcmp(m->code,S8_MOVES[S8X_M_BLIZZARD].code) && weather==WEATHER_SNOW) ||
       ((!strcmp(m->code,S8_MOVES[S8X_M_HURRICANE].code) || !strcmp(m->code,S8_MOVES[S8X_M_THUNDER].code)) && weather==WEATHER_RAIN)) return 1;
    if(weather==WEATHER_SUN && (!strcmp(m->code,S8_MOVES[S8X_M_HURRICANE].code) || !strcmp(m->code,S8_MOVES[S8X_M_THUNDER].code))) {
        adjusted=*m;adjusted.acc=50;m=&adjusted;
    }
    if(s8_control_accuracy(a,b,m) || m->acc>100) return 1;
    int ar=s8_property_rank(a,5,a->acc),er=s8_property_rank(b,6,b->evasion);
    // Illuminate/Keen Eye modify the active move; suppression is checked at use.
    int eye=S8_PROPERTY && S8_PROPERTY->user==a?S8_PROPERTY->ignore_evasion:
        s8_has_ability(a,S8Q_ILLUMINATE) || s8_has_ability(a,S8Q_KEENEYE);
    if(ignore_evasion || eye) er=0;
    double accuracy=m->acc;
    if(!first) {
        static const double boosts[]={1,4.0/3,5.0/3,2,7.0/3,8.0/3,3};
        accuracy=ar>=0?accuracy*boosts[ar]:accuracy/boosts[-ar];
        accuracy=er>=0?accuracy/boosts[er]:accuracy*boosts[-er];
    }
    S8AbilityEvent e={S8AE_MODIFY_ACCURACY,a,b,m,current_gravity(a)?6840:4096,0};
    S8AbilityCall calls[sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS)];int n=0;
    for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
        const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
        const Mon *holder=row->event==S8AE_SOURCE_MODIFY_ACCURACY?a:row->event==S8AE_MODIFY_ACCURACY?b:NULL;
        if(!holder || !s8_has_ability(holder,row->ability)) continue;
        if(holder!=a && (FIELD.ignore_breakable || (S8_PROPERTY && S8_PROPERTY->ignore)) && s8_resist_is_breakable(row->ability)) continue;
        calls[n++]=(S8AbilityCall){holder,row,eff_speed(holder),0};
    }
    int modifier=s8_ability_run_queue(calls,n,&e,NULL);
    if(held_item(a)==ITEM_WIDE) modifier=damage_chain(modifier,4505);
    if(held_item(a)==ITEM_ZOOM && CUR_SECOND) modifier=damage_chain(modifier,4915);
    if(held_item(b)==ITEM_POWDER) modifier=damage_chain(modifier,3686);
    if(accuracy==floor(accuracy)) accuracy=item_mod((int)accuracy,modifier);
    if(first) {
        int stage=clamp_stage(ar-er);
        accuracy=stage>=0?(int)accuracy*(3+stage)/3:(int)accuracy*3/(3-stage);
    }
    return (int)(rng_dbl(r)*100)<accuracy;
}

static int s8_boost_accuracy(Mon *a, Mon *b, const MoveDef *m, Rng *r, int ignore_evasion) {
    if(s8_boost_modern(a,b)) return s8_boost_accuracy_check(a,b,m,r,ignore_evasion,1);
    if(s8_control_accuracy(a,b,m)) return 1;
    if(m->acc>100) return 1;
    int modifier=current_gravity(a)?6840:4096;
    if(held_item(a)==ITEM_WIDE) modifier=damage_chain(modifier,4505);
    if(held_item(a)==ITEM_ZOOM && CUR_SECOND) modifier=damage_chain(modifier,4915);
    if(held_item(b)==ITEM_POWDER) modifier=damage_chain(modifier,3686);
    int accuracy=item_mod(m->acc,modifier);
    int boost=clamp_stage(s8_property_rank(a,5,a->acc)-(ignore_evasion?0:s8_property_rank(b,6,b->evasion)));
    accuracy=boost>=0?accuracy*(3+boost)/3:accuracy*3/(3-boost);
    return (int)(rng_dbl(r)*100)<accuracy;
}
static int accuracy_check(Mon *atk, Mon *def, const MoveDef *mv, Rng *r) {
    if(def && s8_boost_modern(atk,def)) return s8_boost_accuracy_check(atk,def,mv,r,0,1);
    if(def && s8_control_accuracy(atk,def,mv)) return 1;
    if(def && def->evasion) return s8_boost_accuracy(atk,def,mv,r,0);
    if (mv->acc > 100) return 1;
    int rank=s8_property_rank(atk,5,atk->acc);
    double eff = mv->acc * acc_mult(rank);
    if(current_gravity(atk)) {
        // Gravity (priority 0) and these items (priority -2) share
        // ModifyAccuracy: chain once, round once, THEN truncate stages.
        int modifier=6840;
        if(held_item(atk)==ITEM_WIDE) modifier=damage_chain(modifier,4505);
        if(held_item(atk)==ITEM_ZOOM && CUR_SECOND) modifier=damage_chain(modifier,4915);
        if(def && held_item(def)==ITEM_POWDER) modifier=damage_chain(modifier,3686);
        int accuracy=item_mod(mv->acc,modifier);
        eff=rank>=0?accuracy*(3+rank)/3:accuracy*3/(3-rank);
    } else {
        if (held_item(atk) == ITEM_WIDE) eff *= 1.1;                    // Wide Lens
        if (held_item(atk) == ITEM_ZOOM && CUR_SECOND) eff *= 1.2;      // Zoom Lens
        if (def && held_item(def) == ITEM_POWDER) eff *= 0.9;           // Bright Powder
    }
    return rng_dbl(r) * 100.0 < eff;
}

// Stage changes and flinches share one post-hit path for old and new moves.
// Legacy effects retain their skipped rolls and announcement behavior exactly.
static void s8_apply_stat_effect(Mon *me, Mon *foe, const S8StatEffect *e, int body_hit, Rng *r) {
    if(e->mode==S8_STAT_SECONDARY && !s8_guard_secondary(foe,e->self)) return;
    if(S8_PROPERTY && S8_PROPERTY->sheer && e->mode!=S8_STAT_SELFBOOST) return;
    // selfDrops and secondaries each draw once, even at 100%, into a
    // Substitute or after target KO. selfBoost has no chance draw.
    // Keep the engine's established splitmix/double mapping, not PS's PRNG.
    if(e->mode!=S8_STAT_SELFBOOST && rng_dbl(r)>=e->chance/100.0) return;
    Mon *recipient=e->self?me:foe;
    if(recipient->hp<=0 || (!e->self && !body_hit)) return;
    if(s8_boost_modern(me,foe)) {
        int delta[7]={0};for(int i=0;i<6;i++) delta[i]=e->delta[i];
        s8_change_boost_event(recipient,me,delta,e->mode==S8_STAT_SELFBOOST);return;
    }
    // The secondary list was filtered before chance rolls.
    // Existing Clear Body remains active; it never blocks self-inflicted drops.
    for(int i=0;i<6;i++) {
        int change=s8_ability_boost(recipient,me,e->delta[i]);
        if(!change) continue;
        int *stage=i==5?&recipient->acc:&recipient->st[i];
        int before=*stage; *stage=clamp_stage(before+change);
        if(*stage<before) recipient->stats_lowered_turn=1;
        if(*stage>before) recipient->stats_raised_turn=1;
        if(*stage!=before) obs_emit(EV_STAGE,recipient,i,*stage-before,0);
    }
    // One simultaneous boost object: do not consume Herb between its members.
    white_herb_check(recipient);
}
static void s8_apply_self_boost(Mon *me, Mon *foe, int mv, Rng *r) {
    const S8StatEffect *e=s8_stat_effect(mv);
    // Clanging Scales: after the successful hit's target effects/items, before
    // AfterMoveSecondarySelf (Life Orb). No extra roll, including on target KO.
    if(e && e->mode==S8_STAT_SELFBOOST) s8_apply_stat_effect(me,foe,e,1,r);
}
static void s8_apply_status_effects(Mon *me, Mon *foe, const S8StatusMove *m, int body_hit, Rng *r) {
    if((S8_PROPERTY && S8_PROPERTY->sheer) || !s8_guard_secondary(foe,0)) return;
    for(int i=0;i<m->count;i++) {
        const S8ExtraEffect *e=&m->effects[i];
        // Each secondary is an independent roll in definition order, including
        // 100%, Substitute, KO, immunity and an already present status.
        if(rng_dbl(r)>=e->chance/100.0 || !body_hit || foe->hp<=0) continue;
        switch(e->kind) {
        case S8_EXTRA_STATUS: inflict_status(foe,e->status,r); break;
        case S8_EXTRA_FLINCH:
            if(!CUR_SECOND && !foe->focusing) s8_guard_flinch(foe);
            break;
        case S8_EXTRA_CONFUSION: inflict_confusion_min(foe,m->confusion_min,r); break;
        case S8_EXTRA_BOOST: {
            const S8StatEffect boost={m->move,S8_STAT_SELFBOOST,100,0,
                {e->delta[0],e->delta[1],e->delta[2],e->delta[3],e->delta[4],e->delta[5]}};
            s8_apply_stat_effect(me,foe,&boost,body_hit,r);
            break;
        }
        }
    }
}
static void apply_move_effect(Mon *me, Mon *foe, int mv, int body_hit, Rng *r) {
    if(S8_PROPERTY && S8_PROPERTY->sheer) return;
    if(mv>=MV_COUNT) {
        if(mv==S8V_GRAVAPPLE) {
            const S8StatEffect drop={mv,S8_STAT_SECONDARY,100,0,{0,-1,0,0,0,0}};
            s8_apply_stat_effect(me,foe,&drop,body_hit,r);
        }
        if(mv==S8V_BARBBARRAGE || mv==S8V_INFERNALPARADE) {
            int chance=mv==S8V_BARBBARRAGE?50:30;
            if(s8_guard_secondary(foe,0) && rng_dbl(r)<chance/100.0 && body_hit && foe->hp>0)
                inflict_status(foe,mv==S8V_BARBBARRAGE?STATUS_PSN:STATUS_BRN,r);
        }
        const S8StatEffect *e=s8_stat_effect(mv);
        if(e && e->mode!=S8_STAT_SELFBOOST) s8_apply_stat_effect(me,foe,e,body_hit,r);
        const S8StatusMove *m=s8_status_move(mv);
        if(m) s8_apply_status_effects(me,foe,m,body_hit,r);
        return;
    }
    const MoveEffect *e=&MOVE_EFFECTS[mv];
    if(!e->kind || !s8_guard_secondary(foe,e->self)) return;
    int legacy=mv<V1_MOVE_COUNT;
    if(legacy && (!body_hit || foe->hp<=0)) return;
    Mon *recipient=e->self?me:foe;
    int blocked=!s8_boost_modern(me,foe) && e->kind==EFFECT_STAGE && e->delta<0 && !s8_ability_boost(recipient,me,e->delta);
    if(legacy && blocked && s8_current_ability(recipient)!=S8G_FLOWERVEIL) {
        if(e->chance==100) s8_ability_record(recipient,S8AE_TRY_BOOST,S8A_PUBLIC);
        if(e->chance==100) obs_emit(EV_ABILITY,recipient,AB_CB,0,0);
        return;
    }
    // New effects roll even at 100%, against Substitute or a KO target,
    // before moveHit checks eligibility. Old BD/MS keep their skipped rolls.
    if((!legacy || e->chance<100) && rng_dbl(r)>=e->chance/100.0) return;
    if((!e->self && (!body_hit || foe->hp<=0)) || (e->self && me->hp<=0)) return;
    // Clear Body suppresses its message for moves with secondaries. Only
    // the legacy guaranteed-drop announcements above retain the old contract.
    if(blocked) return;
    if(e->kind==EFFECT_STATUS) { inflict_status(recipient,e->stage,r); return; }
    if(e->kind==EFFECT_FLINCH) {
        if(!CUR_SECOND && !recipient->focusing) s8_guard_flinch(recipient); // only a remaining action can be lost
        return;
    }
    if(s8_boost_modern(me,foe)) {
        int delta[7]={0};delta[e->stage]=e->delta;
        s8_change_boost_event(recipient,me,delta,0);return;
    }
    int *stage=e->stage==5?&recipient->acc:&recipient->st[e->stage];
    int before=*stage; *stage=clamp_stage(*stage+e->delta);
    if(*stage<before) recipient->stats_lowered_turn=1;
    if(*stage>before) recipient->stats_raised_turn=1;
    if(legacy || *stage!=before) obs_emit(EV_STAGE,recipient,e->stage,legacy?e->delta:*stage-before,0);
    if(e->self) white_herb_check(recipient);
}

// Scoped only around an attack executed during a legacy sleep opportunity.
// The old timer reaches zero before the final sleeping action is resolved.
static int CUR_SLEEP_ACTION;

// ---------- Move execution ----------
// Executes one already-selected move (PP already consumed by caller, except Sleep Talk calls).
static int CUR_FUTURE_IMPACT, LAST_DIRECT_DAMAGE, HIT_LANDED, G5_COST_DONE;
static void g5_after_damage(Mon *me,Mon *foe,int mv,int dealt) {
    (void)foe;
    if(G5_COST_DONE) return;
    G5_COST_DONE=1;
    if((mv==MV_GI || mv==MV_HB || (s8_protect_move(mv) && s8_protect_move(mv)->kind==S8P_RECHARGE)) && HIT_LANDED) { obs2_emit(EV_RECHARGE_START,me,mv,0,0); me->recharge=1; me->state_features|=MON_POST_V1_STATE; }
    if(mv==MV_ROLL && HIT_LANDED) { if(FIELD.terrain) obs2_emit(EV_TERRAIN_END,me,0,0,0); FIELD.terrain=TERRAIN_NONE; FIELD.terrain_turns=0; }
    // Stage 8d: Rock Head guards this recoil; Reckless uses recoil/crash metadata
    // in the power modifier path, without changing the recoil fraction.
    int recoil_num=mv==MV_DE?33:mv>=MV_COUNT?S8_MOVES[mv].recoil_num:0;
    int recoil_den=mv==MV_DE?100:mv>=MV_COUNT?S8_MOVES[mv].recoil_den:1;
    int crash=mv>=MV_COUNT && S8_MOVES[mv].crash && !HIT_LANDED;
    if(mv==MV_BEAM || (recoil_num && dealt>0) || crash) {
        int loss=mv==MV_BEAM?(me->maxhp+1)/2:crash?me->maxhp/2:
            (int)(((int64_t)dealt*recoil_num+recoil_den/2)/recoil_den);
        if(loss<1) loss=1;
        loss=s8_resist_loss(me,loss,mv==MV_BEAM || crash?S8R_OTHER:S8R_RECOIL);
        if(!loss) return;
        int before=me->hp; me->hp-=loss;
        queue_faint(me,mv==MV_BEAM?FAINT_ATTEMPT_COST:FAINT_RECOIL);
        obs_hp(me,before,HP_RECOIL); berry_hp_check(me);
    }
}
// New multihit accuracy follows the pinned first-hit / later-hit ordering.
// Existing moves keep accuracy_check's historical mapping and rounding.
static int s8_multihit_accuracy(Mon *me, Mon *foe, const MoveDef *m, Rng *r, int first) {
    MoveDef adjusted;
    int weather=current_weather(foe);
    if((!strcmp(m->code,S8_MOVES[S8X_M_BLIZZARD].code) && weather==WEATHER_SNOW) ||
       ((!strcmp(m->code,S8_MOVES[S8X_M_HURRICANE].code) || !strcmp(m->code,S8_MOVES[S8X_M_THUNDER].code)) && weather==WEATHER_RAIN)) return 1;
    if(weather==WEATHER_SUN && (!strcmp(m->code,S8_MOVES[S8X_M_HURRICANE].code) || !strcmp(m->code,S8_MOVES[S8X_M_THUNDER].code))) {
        adjusted=*m;adjusted.acc=50;m=&adjusted;
    }
    if(s8_control_accuracy(me,foe,m)) return 1;
    if(s8_boost_modern(me,foe)) return s8_boost_accuracy_check(me,foe,m,r,0,first);
    int modifier=current_gravity(me)?6840:4096;
    if(held_item(me)==ITEM_WIDE) modifier=damage_chain(modifier,4505);
    if(held_item(me)==ITEM_ZOOM && CUR_SECOND) modifier=damage_chain(modifier,4915);
    if(held_item(foe)==ITEM_POWDER) modifier=damage_chain(modifier,3686);
    if(m->acc>100) return 1;
    if(first && foe->evasion) return s8_boost_accuracy(me,foe,m,r,0);
    int ar=s8_property_rank(me,5,me->acc),er=s8_property_rank(foe,6,foe->evasion);
    double accuracy=m->acc;
    if(first) {
        accuracy=item_mod(m->acc,modifier);
        accuracy=ar>=0?(int)accuracy*(3+ar)/3:(int)accuracy*3/(3-ar);
    } else {
        static const double boosts[]={1,4.0/3,5.0/3,2,7.0/3,8.0/3,3};
        accuracy=ar>=0?accuracy*boosts[ar]:accuracy/boosts[-ar];
        if(er>0) accuracy/=boosts[er];
        else if(er<0) accuracy*=boosts[-er];
        // runEvent finalizes chained modifiers only for integral relay values.
        if(accuracy==floor(accuracy)) accuracy=item_mod((int)accuracy,modifier);
    }
    return (int)(rng_dbl(r)*100)<accuracy; // PRNG.randomChance -> random(100)
}
// Boost objects use all seven ranks; direct set/swap/reset never triggers
// Clear Body or statsLoweredThisTurn. White Herb runs at the following Update.
static int *s8_boost_stage(Mon *m,int i) { return i<5?&m->st[i]:i==5?&m->acc:&m->evasion; }
// One boost object is transformed and capped before any TryBoost callback.
// Per-stat reactions run immediately; AfterBoost receives the capped object.
static int s8_boost_any(Mon *a,Mon *b,S8AbilityEventId event,const MoveDef *move,int value,int detail) {
    value=s8_ability_event(a,event,a,b,move,value,detail);
    if(b && b!=a) value=s8_ability_event(b,event,a,b,move,value,detail);
    return value;
}
static void s8_boost_callback(const Mon *holder,S8AbilityEvent *e) {
    const S8BoostAbility *q=s8_boost_ability(s8_current_ability(holder));
    Mon *m=(Mon *)holder;int id=q->ability;
    if(id==S8Q_OPPORTUNIST && holder->hp<=0 && e->event!=S8AE_END) return;
    if(e->event==S8AE_CHANGE_BOOST) {
        if(S8_BOOST_EFFECT!=S8Q_ZPOWER) e->value*=q->change;
    } else if(e->event==S8AE_TRY_BOOST) {
        if(id==S8Q_GUARDDOG) {
            if(S8_BOOST_EFFECT==S8_HIT_INTIMIDATE && e->detail==0 && e->value) {
                e->value=0;const int delta[7]={1,0,0,0,0,0,0};
                int saved=S8_BOOST_EFFECT;S8_BOOST_EFFECT=id;
                if(s8_change_boost(m,m,delta)) s8_ability_record(holder,e->event,S8A_PUBLIC);
                S8_BOOST_EFFECT=saved;
            }
        } else if(id==S8Q_MIRRORARMOR) {
            if(e->source && holder!=e->source && S8_BOOST_EFFECT!=S8Q_MIRROR &&
               e->value<0 && *s8_boost_stage(m,e->detail)!=-6) {
                int delta[7]={0};delta[e->detail]=e->value;e->value=0;
                if(e->source->hp>0) {
                    int saved=S8_BOOST_EFFECT;S8_BOOST_EFFECT=S8Q_MIRROR;
                    s8_ability_record(holder,e->event,S8A_PUBLIC);
                    s8_change_boost((Mon *)e->source,m,delta);S8_BOOST_EFFECT=saved;
                }
            }
        } else if(holder!=e->source && e->value<0 && (q->guard_stat==7 || q->guard_stat==e->detail)) e->value=0;
    } else if(e->event==S8AE_AFTER_EACH_BOOST) {
        if(e->source && e->source!=holder && e->value<0 && q->react_stat>=0) {
            int delta[7]={0};delta[q->react_stat]=2;
            int saved=S8_BOOST_EFFECT;S8_BOOST_EFFECT=id;
            if(s8_change_boost(m,m,delta)) s8_ability_record(holder,e->event,S8A_PUBLIC);
            S8_BOOST_EFFECT=saved;
        }
    } else if(e->event==S8AE_FOE_AFTER_BOOST) {
        if(S8_BOOST_EFFECT!=S8Q_COPY && S8_BOOST_EFFECT!=S8Q_MIRROR_HERB && e->value>0)
            m->ability_state.boosts[e->detail]+=e->value;
    } else if(id==S8Q_OPPORTUNIST) {
        if(e->event==S8AE_END) { memset(m->ability_state.boosts,0,sizeof(m->ability_state.boosts));return; }
        int delta[7],any=0;
        for(int i=0;i<7;i++) { delta[i]=m->ability_state.boosts[i];any|=delta[i]; }
        if(any) {
            memset(m->ability_state.boosts,0,sizeof(m->ability_state.boosts));
            int saved=S8_BOOST_EFFECT;S8_BOOST_EFFECT=S8Q_COPY;
            if(s8_change_boost(m,m,delta)) s8_ability_record(holder,e->event,S8A_PUBLIC);
            S8_BOOST_EFFECT=saved;
        }
    } else if(e->event==S8AE_MODIFY_ACCURACY || e->event==S8AE_SOURCE_MODIFY_ACCURACY) {
        if(q->condition==1 && current_weather(holder)!=WEATHER_SAND) return;
        if(q->condition==2 && current_weather(holder)!=WEATHER_SNOW) return;
        if(q->condition==3 && !holder->confusion) return;
        if(q->condition==4 && (!e->move || e->move->cat!=CAT_PHYS)) return;
        e->value=damage_chain(e->value,q->accuracy);
    } else if(e->event==S8AE_MODIFY_MOVE) {
        if(S8_PROPERTY && S8_PROPERTY->user==holder) S8_PROPERTY->ignore_evasion=1;
    } else if(e->event==S8AE_ATK) {
        // Hustle returns a directly rounded stat, before lower-priority modifiers.
        // Its row is excluded from the separate fixed-point modifier collector.
        e->value=item_mod(e->value,6144);
    } else if(e->event==S8AE_IMMUNITY) {
        if((id==S8Q_SANDVEIL && e->detail==S8R_SAND) || (id==S8Q_SNOWCLOAK && e->detail==S8R_HAIL)) e->value=0;
    } else if(e->event==S8AE_ALWAYS_ACCURACY) { if(e->move) e->value=1; }
    else if(e->event==S8AE_INVULNERABILITY) { if(e->move) e->value=0; }
    else if(e->event==S8AE_TRY_MOVE) {
        const char *id=s8_power_move(e->move)->id;
        if(!strcmp(id,"explosion") || !strcmp(id,"selfdestruct") || !strcmp(id,"mistyexplosion") || !strcmp(id,"mindblown")) {
            e->value=0;s8_ability_record(holder,e->event,S8A_PUBLIC);
        }
    } else if(e->event==S8AE_ANY_DAMAGE) {
        if(e->detail==S8H_AFTERMATH) e->value=0;
    } else if(e->event==S8AE_DRAG_OUT) {
        s8_ability_record(holder,e->event,S8A_PUBLIC);e->value=0;
    } else if((e->event==S8AE_TRY_HIT || e->event==S8AE_TRY_HIT_SIDE) && e->detail==1) {
        const S8Move *move=s8_power_move(e->move);
        if(holder!=e->source && !S8_BOUNCED && (move->flags&S8_FLAG_REFLECTABLE) && s8_semi(holder)<0) e->value=-1;
    }
}
static int s8_boost_change(Mon *target,Mon *source,const int delta[7],int public_message) {
    if(target->hp<=0) return 0;
    int boost[7],changed=0,attack_before=target->st[0],announce=0;
    for(int i=0;i<7;i++) {
        int value=s8_ability_event(target,S8AE_CHANGE_BOOST,source,target,S8_STATUS_EFFECT,delta[i],i);
        int before=*s8_boost_stage(target,i);
        boost[i]=clamp_stage(before+value)-before;
    }
    // TryBoost is complete for the object before applying the first stat.
    for(int i=0;i<7;i++) {
        int value=s8_ability_event(target,S8AE_TRY_BOOST,source,target,S8_STATUS_EFFECT,boost[i],i);
        if(boost[i]<0 && !value && s8_current_ability(target)!=S8Q_MIRRORARMOR && s8_current_ability(target)!=S8Q_GUARDDOG) announce=1;
        boost[i]=value;
    }
    if(announce && public_message) {
        int id=s8_current_ability(target);
        int octolock=S8_STATUS_EFFECT && !strcmp(s8_power_move(S8_STATUS_EFFECT)->id,"octolock");
        if(!s8_guard_move(S8_STATUS_EFFECT)[2] && (!octolock || (id!=S8Q_BIGPECKS && id!=S8Q_WHITESMOKE && id!=S8Q_CLEARBODY)))
            s8_ability_record(target,S8AE_TRY_BOOST,S8A_PUBLIC);
    }
    for(int i=0;i<7;i++) {
        int *rank=s8_boost_stage(target,i),before=*rank;
        *rank=clamp_stage(before+boost[i]);
        if(*rank==before) continue;
        changed=1;if(i<6) obs_emit(EV_STAGE,target,i,*rank-before,0);
        if(*rank<before) target->stats_lowered_turn=1;
        if(*rank>before) target->stats_raised_turn=1;
        s8_ability_event(target,S8AE_AFTER_EACH_BOOST,source,target,S8_STATUS_EFFECT,boost[i],i);
    }
    s8_ability_event(target,S8AE_AFTER_BOOST,source,target,NULL,target->st[0]-attack_before,S8_BOOST_EFFECT);
    Mon *foe=field_member(target)?FIELD.seat[target==FIELD.seat[0]]:(source!=target?source:NULL);
    if(foe) for(int i=0;i<7;i++)
        s8_ability_event(foe,S8AE_FOE_AFTER_BOOST,source,target,S8_STATUS_EFFECT,boost[i],i);
    white_herb_check(target);return changed;
}

static int s8_change_boost_event(Mon *target,Mon *source,const int delta[7],int public_message) {
    Mon *other=field_member(target)?FIELD.seat[target==FIELD.seat[0]]:source;
    if(s8_boost_modern(target,source) || s8_boost_modern(target,other))
        return s8_boost_change(target,source,delta,public_message);
    int changed=0,lowered=0;
    if(target->hp<=0) return 0;
    int announce=0,attack_before=target->st[0];
    for(int i=0;i<7;i++) {
        int change=s8_ability_event(target,S8AE_TRY_BOOST,source,target,NULL,delta[i],i);
        if(delta[i]<0 && !change && !(S8_BOOST_EFFECT==S8_HIT_INTIMIDATE &&
           s8_guard_row(target) && s8_guard_row(target)->boost==2 && i==0)) announce=1;
        if(!change) continue;
        int *rank=s8_boost_stage(target,i),before=*rank;
        *rank=clamp_stage(before+change);
        if(*rank!=before) { changed=1; if(i<6) obs_emit(EV_STAGE,target,i,*rank-before,0); }
        if(*rank<before) lowered=1;
        if(*rank>before) target->stats_raised_turn=1;
    }
    if(announce && (s8_current_ability(target)==S8G_FLOWERVEIL?
       !s8_guard_move(S8_STATUS_EFFECT)[2]:public_message))
        s8_ability_record(target,S8AE_TRY_BOOST,S8A_PUBLIC);
    if(lowered) target->stats_lowered_turn=1;
    s8_ability_event(target,S8AE_AFTER_BOOST,source,target,NULL,target->st[0]-attack_before,S8_BOOST_EFFECT);
    white_herb_check(target);
    return changed;
}
static int s8_change_boost(Mon *target,Mon *source,const int delta[7]) {
    return s8_change_boost_event(target,source,delta,1);
}
// Shared volatile End seam for Spit Up and future Swallow. Debts are recorded
// independently from current ranks, and survive Haze, swaps and saturation.
static void s8_consume_stockpile(Mon *m) {
    if(!m->stockpile) return;
    int debt[7]={0,m->stock_def,0,m->stock_spd,0,0,0};
    s8_change_boost(m,m,debt);
    m->stockpile=m->stock_def=m->stock_spd=0;
}
static int s8_heal_hp(Mon *m,int amount) {
    if(m->hp<=0 || m->healblock || m->hp>=m->maxhp || amount<=0) return 0;
    int before=m->hp;
    m->hp=amount<m->maxhp-before?before+amount:m->maxhp;
    obs_hp(m,before,HP_HEAL);
    return m->hp-before;
}
// TryHeal modifiers apply after callbacks (not to Liquid Ooze's retaliation).
// effects 0..2 are drain/Leech Seed/Strength Sap; 3..4 are ring/root.
static int s8_heal_from(Mon *m,int amount,int effect) {
    if(effect>=0 && effect<=4 && held_item(m)==ITEM_BIGROOT) amount=item_mod(amount,5324);
    return s8_heal_hp(m,amount);
}
static void s8_property_healblock(const Mon *holder,S8AbilityEvent *e) {
    (void)holder;e->value=0;
}
static int s8_property_drain(Mon *target,Mon *source,int amount,int effect) {
    if(target->hp<=0 || amount<=0) return 0;
    // TryHeal collects target Condition (subOrder 2) and source Ability (7).
    // Speed precedes subOrder; Heal Block wins a speed tie. No random tie.
    S8AbilityEvent e={S8AE_DRAIN,source,target,NULL,amount,effect};
    S8AbilityCall calls[2];int n=0;
    const S8AbilityHandler blocked={-1,S8AE_DRAIN,0,0,2,s8_property_healblock};
    if(target->healblock) calls[n++]=(S8AbilityCall){target,&blocked,eff_speed(target),0};
    for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
        const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
        if(row->event==S8AE_DRAIN && s8_has_ability(source,row->ability))
            calls[n++]=(S8AbilityCall){source,row,eff_speed(source),0};
    }
    s8_ability_run_queue(calls,n,&e,NULL);
    return s8_heal_from(target,e.value,effect);
}
// ---------- Stage 8c-9b trapping, drain links and side hazards ----------
// In this no-switch format every opposing-source volatile refers to the other
// seat. Linked trapped/trapper ends on source faint; seed/Curse do not.
static void s8_trap_links_update(Mon *a,Mon *b) {
    if(a->hp>0 && b->hp>0) return;
    a->trapped=a->trapper=b->trapped=b->trapper=0;
}
static int s8_add_trapped(Mon *target,Mon *source) {
    if(target->hp<=0 || source->hp<=0 || target->trapped || has_type(target,T_GHOST)) return 0;
    target->trapped=1;source->trapper=1;
    target->state_features|=MON_POST_V1_STATE;source->state_features|=MON_POST_V1_STATE;
    return 1;
}
static int s8_add_hazard(Mon *side,const S8TrapMove *row) {
    int *layers=row->hazard==1?&side->spikes:row->hazard==2?&side->toxic_spikes:
        row->hazard==3?&side->sticky_web:&side->stealth_rock;
    if(*layers>=row->layers) return 0;
    ++*layers;side->state_features|=MON_POST_V1_STATE;
    if(row->hazard==4) obs2_emit(EV_ROCKS,side,0,0,0);
    return 1;
}
static int s8_link_damage(Mon *m,int divisor) {
    if(m->hp<=0) return 0;
    int loss=m->maxhp/divisor,before=m->hp;
    if(loss<1) loss=1;
    loss=s8_resist_loss(m,loss,S8R_OTHER);if(!loss) return 0;
    m->hp=loss<before?before-loss:0;
    queue_faint(m,FAINT_DAMAGE);obs_hp(m,before,HP_RECOIL);
    // Battle.damage returns the actual damage, before Update's berry healing.
    int dealt=before-m->hp;berry_hp_check(m);return dealt;
}
static void s8_trap_residual(Mon *a,Mon *b,int order) {
    Mon *ms[]={a,b};int active[2];
    for(int i=0;i<2;i++) active[i]=ms[i]->hp>0 && (order==8?ms[i]->leech_seed:
        order==12?ms[i]->cursed:order==13?ms[i]->partial_trap:ms[i]->octolock);
    int first=0;
    if(active[0] && active[1]) {
        double sa=s8_action_speed(a),sb=s8_action_speed(b);
        if(sb>sa || (sb==sa && BATTLE_RNG && rng_dbl(BATTLE_RNG)<.5)) first=1;
    }
    for(int k=0;k<2;k++) {
        int i=k?1-first:first;Mon *m=ms[i],*source=ms[1-i];
        if(!active[i] || m->hp<=0) continue;
        if(order==8) {
            if(source->hp>0) { int dealt=s8_link_damage(m,8);s8_property_drain(source,m,dealt,1); }
        } else if(order==12) s8_link_damage(m,4);
        else if(order==13) {
            // residualEvent decrements duration BEFORE calling onResidual.
            if(!--m->partial_trap || source->hp<=0) { m->partial_trap=m->partial_move=0;continue; }
            s8_link_damage(m,held_item(source)==ITEM_BINDINGBAND?6:S8T_PARTIAL_DIVISOR);
        } else {
            if(source->hp<=0) { m->octolock=0;continue; }
            const int delta[7]={0,-1,0,-1,0,0,0};s8_change_boost_event(m,source,delta,0);
        }
        s8_trap_links_update(a,b);
        if(a->hp<=0 || b->hp<=0) break;
    }
}
static void s8_execute_trap_status(Mon *me,Mon *foe,const S8TrapMove *row,Rng *r) {
    int success=0;
    if(row->kind==S8T_HAZARD) { success=s8_add_hazard(foe,row);goto done; }
    if(row->kind==S8T_GHOST_CURSE) {
        if(foe->cursed || foe->hp<=0) goto done;
        int before=me->hp,cost=me->maxhp/2;if(cost<1) cost=1;
        me->hp=cost<before?before-cost:0;
        queue_faint(me,FAINT_ATTEMPT_COST);obs_hp(me,before,HP_COST);
        foe->cursed=1;foe->state_features|=MON_POST_V1_STATE;
        berry_hp_check(me);success=1;goto done;
    }
    if(row->protect && s8_is_protected(foe,row->move)) { attempt_result(me,ATTEMPT_NOT_FAIL);return; }
    // TryImmunity precedes accuracy; these status moves ignore type-chart
    // damage immunities (only Leech Seed Grass and Octolock Ghost are special).
    if((row->kind==S8T_SEED && has_type(foe,T_GRASS)) ||
       (row->kind==S8T_OCTO && has_type(foe,T_GHOST))) goto done;
    if(!s8_boost_accuracy(me,foe,move_def(row->move),r,0) || s8_sub_blocks(foe,row->move)) goto done;
    if(row->kind==S8T_TRAP) success=s8_add_trapped(foe,me);
    else {
        int *flag=row->kind==S8T_SEED?&foe->leech_seed:&foe->octolock;
        if(!*flag && foe->hp>0) { *flag=1;foe->state_features|=MON_POST_V1_STATE;success=1; }
    }
done:
    if(!success) { attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0); }
}
static void s8_trap_hit(Mon *me,Mon *foe,const S8TrapMove *row,int sub,Rng *r) {
    if(!row) return;
    // onAfterSubDamage runs in Substitute's TryPrimaryHit, before secondaries.
    if(sub && row->kind==S8T_EDGE && me->hp>0 && !(S8_PROPERTY && S8_PROPERTY->sheer)) s8_add_hazard(foe,row);
    if(!sub && foe->hp>0) {
        if(row->kind==S8T_PARTIAL && !foe->partial_trap) {
            foe->partial_trap=S8T_DURATION_MIN+(int)(rng_dbl(r)*S8T_DURATION_RANGE);
            foe->partial_move=row->move;foe->state_features|=MON_POST_V1_STATE;
        } else if(row->kind==S8T_JAW) {
            s8_add_trapped(me,foe);s8_add_trapped(foe,me);
        }
    }
    // Even empty secondary objects consume a roll (battle-actions.secondaries).
    if((row->kind==S8T_SECONDARY_TRAP || row->kind==S8T_EDGE) && !(S8_PROPERTY && S8_PROPERTY->sheer) && s8_guard_secondary(foe,0)) {
        (void)rng_dbl(r);
        if(row->kind==S8T_SECONDARY_TRAP && !sub) s8_add_trapped(foe,me);
    }
}

// ---------- Stage 8c-9c CONTROL resolution ----------
static void s8_control_links_update(Mon *a,Mon *b) {
    Mon *ms[]={a,b};
    for(int i=0;i<2;i++) if(ms[1-i]->hp<=0) {
        ms[i]->control[S8C_ATTRACT]=ms[i]->control[S8C_SYRUPBOMB]=0;
    }
}
static int s8_control_add(Mon *m,int kind,int duration) {
    if(m->hp<=0 || (m->control[kind] && kind!=S8C_CHARGE)) return 0;
    if(!s8_guard_volatile(m,kind)) return 0;
    m->control[kind]=duration?duration:1;m->state_features|=MON_POST_V1_STATE;return 1;
}
static int s8_control_will_move(const Mon *m) {
    return field_member(m) && !FIELD.consumed[m==FIELD.seat[1]];
}
// ---------- Stage 8d-1c: shared hit/status/volatile event resolution ----------
static int s8_hit_has_item(const Mon *m,const char *id) {
    int item=s8_item_table(m);
    return held_item(m)!=ITEM_NONE && item>=0 && !strcmp(S8_ITEMS[item].id,id);
}
static int s8_hit_has_named_ability(const Mon *m,const char *id) {
    int ab=s8_current_ability(m);
    return ab>=0 && !strcmp(S8_ABILITIES[ab].id,id) && s8_has_ability(m,ab);
}
static int s8_hit_contact(const MoveDef *move,const Mon *attacker) {
    return move && move->contact && !s8_hit_has_item(attacker,"protectivepads");
}
static int s8_hit_powder(const Mon *m) {
    return !has_type(m,T_GRASS) && !s8_has_ability(m,S8R_OVERCOAT) && !s8_hit_has_item(m,"safetygoggles");
}
static int s8_hit_random(int n) {
    Rng *r=S8_HIT_CONTEXT?S8_HIT_CONTEXT->rng:S8_POWER_RNG?S8_POWER_RNG:BATTLE_RNG;
    if(!r) { fprintf(stderr,"HIT ability RNG context missing\n");exit(2); }
    return (int)(rng_dbl(r)*n);
}
static int s8_hit_chance(int numerator,int denominator) { return s8_hit_random(denominator)<numerator; }
static void s8_hit_retaliate(Mon *m,int amount) {
    if(m->hp<=0 || amount<=0) return;
    amount=s8_resist_loss(m,amount,S8R_OTHER);
    if(!amount) return;
    int before=m->hp;m->hp=amount<before?before-amount:0;
    queue_faint(m,FAINT_DAMAGE);obs_hp(m,before,HP_RECOIL);berry_hp_check(m);
}
static void s8_hit_inflict(Mon *target,Mon *source,int status,const char *cause) {
    Mon *saved_source=S8_EFFECT_SOURCE;const MoveDef *saved_move=S8_STATUS_EFFECT;
    const char *saved_cause=S8_STATUS_CAUSE;
    S8_EFFECT_SOURCE=source;S8_STATUS_EFFECT=NULL;S8_STATUS_CAUSE=cause;
    Rng *r=S8_HIT_CONTEXT?S8_HIT_CONTEXT->rng:S8_POWER_RNG?S8_POWER_RNG:BATTLE_RNG;
    // Synchronize runs before Lum (-1). An already set status terminates the
    // opposite Synchronize callback naturally, before either berry can cure it.
    if(status) inflict_status(target,status,r);else inflict_sleep(target,r);
    S8_EFFECT_SOURCE=saved_source;S8_STATUS_EFFECT=saved_move;S8_STATUS_CAUSE=saved_cause;
}
static void s8_hit_attract(Mon *target,Mon *source) {
    Mon *saved=S8_EFFECT_SOURCE;const MoveDef *effect=S8_STATUS_EFFECT;
    S8_EFFECT_SOURCE=source;S8_STATUS_EFFECT=NULL;
    if((target->sex==1 && source->sex==2) || (target->sex==2 && source->sex==1))
        s8_control_add(target,S8C_ATTRACT,1);
    S8_EFFECT_SOURCE=saved;S8_STATUS_EFFECT=effect;
}
static void s8_hit_disable(Mon *target,const MoveDef *move) {
    // last_move is updated after execute_move in the legacy API. During this
    // callback Showdown's lastMove already denotes the currently used move.
    int last=target->last_move;
    if(S8_HIT_CONTEXT && S8_HIT_CONTEXT->used_move) last=S8_HIT_CONTEXT->used_move-1;
    else if(!CUR_FUTURE_IMPACT && !S8_CALL_DEPTH) {
        for(int i=0;i<S8_MOVES_COUNT;i++) if(!strcmp(S8_MOVES[i].code,move->code)) { last=i;break; }
    }
    int slot=find_move(target,last);
    if(last<0 || (slot>=0 && target->pp[slot]<=0)) return;
    int duration=5-(s8_control_will_move(target) || !CUR_FUTURE_IMPACT);
    Mon *saved=S8_EFFECT_SOURCE;const MoveDef *effect=S8_STATUS_EFFECT;
    S8_EFFECT_SOURCE=S8_HIT_CONTEXT?(Mon *)S8_HIT_CONTEXT->user:NULL;S8_STATUS_EFFECT=NULL;
    if(s8_control_add(target,S8C_DISABLE,duration)) target->disabled_move=last;
    S8_EFFECT_SOURCE=saved;S8_STATUS_EFFECT=effect;
}
static void s8_hit_berserk(Mon *holder,S8AbilityEvent *e) {
    S8HitContext *h=S8_HIT_CONTEXT;
    holder->ability_state.checked_berserk=1;
    if(!e->source || e->source==holder || holder->hp<=0 || !h->total || !h->has_last) return;
    int damage=h->multihit && !h->smart_target?h->total:h->last_damage;
    if(2*holder->hp<=holder->maxhp && 2*(holder->hp+damage)>holder->maxhp) {
        const int delta[7]={0,0,1,0,0,0,0};s8_change_boost(holder,holder,delta);
    }
}
static void s8_hit_before(Mon *source,Mon *target,const MoveDef *move,int multihit) {
    S8_HIT_CONTEXT->multihit=multihit;
    s8_ability_event(target,S8AE_BEFORE_DAMAGE,source,target,move,1,multihit);
}
static void s8_hit_onhit(Mon *source,Mon *target,const MoveDef *move,int crit) {
    S8_HIT_CONTEXT->crit=crit;
    s8_ability_event(target,S8AE_HIT,source,target,move,1,crit);
}
static void s8_hit_damaging(Mon *source,Mon *target,const MoveDef *move,int damage) {
    // Collect target and source reactions together: Order, priority, captured
    // speed and explicit tie RNG match the 8d-0 event layer.
    S8AbilityCall calls[sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS)];int n=0;
    S8AbilityEvent e={S8AE_DAMAGING_HIT,source,target,move,damage,0};
    S8_HIT_CONTEXT->damage=damage;S8_HIT_CONTEXT->last_hp=target->hp;
    for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
        const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
        const Mon *holder=row->event==S8AE_DAMAGING_HIT?target:row->event==S8AE_SOURCE_DAMAGING_HIT?source:NULL;
        if(!holder || !s8_has_ability(holder,row->ability) || s8_property_ignored(holder)) continue;
        if(holder==target && FIELD.ignore_breakable && s8_resist_is_breakable(row->ability)) continue;
        if(row->ability==S8H_AFTERMATH && !s8_boost_any(source,target,S8AE_ANY_DAMAGE,move,1,S8H_AFTERMATH)) continue;
        calls[n++]=(S8AbilityCall){holder,row,eff_speed(holder),0};
    }
    int original_hp=source->hp;
    s8_ability_run_queue(calls,n,&e,S8_HIT_CONTEXT->rng);
    s8_ability_event(source,S8AE_EMERGENCY_EXIT,source,source,NULL,original_hp,0);
    if(s8_current_ability(target)==S8F_DISGUISE) s8_ability_event(target,S8AE_UPDATE,target,target,NULL,1,0);
    S8_HIT_CONTEXT->total+=damage;S8_HIT_CONTEXT->last_damage=damage;S8_HIT_CONTEXT->has_last=1;
}
// Stage 8i-7: the task's 1on1 policy records attempts despite no bench.
// Multi-member fixtures retain the usual canSwitch predicate. Resolution still
// uses s8_can_switch, so a recorded flag never fabricates a replacement.
static int s8_switch_item_can_attempt(const Mon *m) {
    if(s8_can_switch(m)) return 1;
    if(!field_member(m)) return 0;
    int side=m==FIELD.seat[1];
    return FIELD.party_count[side]==1 && FIELD.party[side][0]==m;
}
static int s8_switch_item_use(Mon *m,int item) {
    if(m->hp<=0 || m->item_used || held_item(m)!=item) return 0;
    if(field_member(m)) s8_item_enable(FIELD.seat[0],FIELD.seat[1]);
    s8_item_spend(m,1);return 1;
}
static void s8_switch_item_secondary(Mon *source,Mon *target,const MoveDef *move) {
    if(!source || source==target || !move || move->cat==CAT_STATUS || target->hp<=0) return;
    int item=held_item(target);
    if(item==ITEM_EJECTBUTTON) {
        if((s8_power_move(move)->flags&S8_FLAG_FUTUREMOVE) || CUR_FUTURE_IMPACT ||
           !s8_switch_item_can_attempt(target) || target->force_switch_flag ||
           target->being_called_back || target->is_sky_dropped || target->commanding || target->commanded) return;
        for(int side=0;side<2;side++) if(FIELD.seat[side] && FIELD.seat[side]->switch_flag) return;
        target->switch_flag=1;target->switch_item_attempts++;
        target->state_features|=MON_POST_V1_STATE;
        // champions override: never clear the attacker's switchFlag on success.
        if(!s8_switch_item_use(target,item)) target->switch_flag=0;
    } else if(item==ITEM_REDCARD) {
        if(source->hp<=0 || !field_member(source) || !s8_switch_item_can_attempt(source) ||
           source->force_switch_flag || target->force_switch_flag) return;
        if(!s8_switch_item_use(target,item)) return;
        target->switch_item_attempts++;source->item_drag_out_attempts++;
        target->state_features|=MON_POST_V1_STATE;source->state_features|=MON_POST_V1_STATE;
        // DragOut targets the attacker; item consumption precedes its vetoes.
        int allowed=s8_ability_event(source,S8AE_DRAG_OUT,target,source,move,1,0);
        if(allowed && !source->ingrain) source->force_switch_flag=1;
    }
}
static void s8_hit_finish(Mon *source,Mon *target,const MoveDef *move) {
    if(S8_HIT_CONTEXT->finished || !S8_HIT_CONTEXT->has_last) return;
    S8_HIT_CONTEXT->finished=1;
    // Eject Button priority 2 precedes the existing priority-0 ability callbacks.
    if(held_item(target)==ITEM_EJECTBUTTON) s8_switch_item_secondary(source,target,move);
    if(s8_current_ability(target)!=S8I_PICKPOCKET || !(S8_PROPERTY && S8_PROPERTY->sheer))
        s8_ability_event(target,S8AE_AFTER_SECONDARY,source,target,move,1,0);
    if(held_item(target)==ITEM_REDCARD) s8_switch_item_secondary(source,target,move);
    // Single-hit healing berries were held until the half-HP crossing test.
    if(s8_has_ability(target,S8H_BERSERK)) berry_hp_check(target);
}
// Stage 8d-1d: callbacks operate on an action-local copy, never the move table.
static void s8_property_modify(const Mon *holder,S8AbilityEvent *e) {
    S8PropertyContext *p=S8_PROPERTY;
    if(!p || holder!=p->user || !e->move) return;
    int id=s8_current_ability(holder),mv=p->move;
    if(e->event==S8AE_MODIFY_TYPE) {
        int type=-1;
        if(id==S8D_LIQUIDVOICE) {
            if(S8_MOVES[mv].flags&S8_FLAG_SOUND) type=T_WATER;
        } else if(e->move->type==T_NORMAL && S8_PROPERTY_MOVES[mv][3]) {
            type=id==S8D_AERILATE?T_FLY:id==S8D_DRAGONIZE?T_DRAGON:id==S8D_PIXILATE?T_FAIRY:T_ICE;
            p->converted=id+1;
        }
        if(type>=0) {
            S8_CONTROL_OVERRIDE=*e->move;S8_CONTROL_OVERRIDE.type=type;S8_CONTROL_OVERRIDE_ID=mv;
        }
    } else if(e->event==S8AE_HIT_PROTECT) {
        if(e->move->contact) { p->bypass=id+1;e->value=0; }
    } else {
        if(id==S8D_INFILTRATOR) p->infiltrates=1;
        if(id==S8D_LONGREACH) {
            S8_CONTROL_OVERRIDE=*e->move;S8_CONTROL_OVERRIDE.contact=0;S8_CONTROL_OVERRIDE_ID=mv;
        }
        if(id==S8D_MOLDBREAKER) p->ignore=1;
        if(id==S8D_SCRAPPY) p->scrappy=1;
        if(id==S8D_SHEERFORCE && S8_PROPERTY_MOVES[mv][0]) p->sheer=1;
        if(id==S8D_STENCH && e->move->cat!=CAT_STATUS && !S8_PROPERTY_MOVES[mv][1]) p->stench=1;
        // Preparation is after move-local ModifyMove. spreadHit is false in 1on1.
        if(id==S8D_PARENTALBOND && S8_PROPERTY_MOVES[mv][2]) p->parental=1;
    }
}
static void s8_property_power(const Mon *holder,S8AbilityEvent *e) {
    if(!S8_PROPERTY || S8_PROPERTY->user!=holder) return;
    if(S8_PROPERTY->converted==s8_current_ability(holder)+1) e->value=damage_chain(e->value,4915);
    if(s8_current_ability(holder)==S8D_SHEERFORCE && S8_PROPERTY->sheer) e->value=damage_chain(e->value,5325);
}
static void s8_property_survive(const Mon *holder,S8AbilityEvent *e) {
    if(holder->hp==holder->maxhp && e->value>=holder->hp) {
        s8_ability_record(holder,S8AE_HP_DAMAGE,S8A_PUBLIC);e->value=holder->hp-1;
    }
}
static void s8_property_sturdy_try(const Mon *holder,S8AbilityEvent *e) {
    if(e->move && s8_ohko(S8_PROPERTY?S8_PROPERTY->move:-1)) {
        s8_ability_record(holder,S8AE_TRY_HIT,S8A_PUBLIC);e->value=0;
    }
}
static void s8_property_ooze(const Mon *holder,S8AbilityEvent *e) {
    (void)holder;
    if(e->value>0 && e->detail>=0 && e->detail<=2) { s8_hit_retaliate((Mon *)e->target,e->value);e->value=0; }
}
static void s8_property_eelevate(const Mon *holder,S8AbilityEvent *e) {
    if(!e->move || holder->hp<=0) return;
    int best=0,delta[7]={0};
    for(int i=1;i<5;i++) if(holder->stats[i+1]>holder->stats[best+1]) best=i;
    delta[best]=e->value;s8_change_boost((Mon *)holder,(Mon *)holder,delta);
}
static void s8_property_stench(Mon *me,Mon *foe,int mv,int body,Rng *r) {
    (void)me;(void)mv;
    if(S8_PROPERTY && S8_PROPERTY->stench && s8_guard_secondary(foe,0) && rng_dbl(r)<0.10 && body && foe->hp>0 && !CUR_SECOND && !foe->focusing) s8_guard_flinch(foe);
}
static void s8_hit_flinch(Mon *m) {
    s8_ability_event(m,S8AE_FLINCH,m,m,NULL,1,0);
}
static void s8_execute_control(Mon *me,Mon *foe,const S8ControlMove *row,Rng *r) {
    int k=row->kind,duration=row->duration,success=0;
    if(k==S8C_DRAGONCHEER) goto done; // adjacentAlly does not include self
    if(k==S8C_PERISHSONG) {
        success=s8_control_add(me,k,duration);
        if(s8_ability_event(foe,S8AE_TRY_HIT,me,foe,move_def(row->move),1,0))
            success=s8_control_add(foe,k,duration) || success;
        else success=1; // the callback's null result counts as handled
        goto done;
    }
    if(k==S8C_CHARGE || k==S8C_MINIMIZE || k==S8C_FOCUSENERGY || k==S8C_IMPRISON || k==S8C_DESTINYBOND) {
        if(k==S8C_DESTINYBOND && me->control[k]) { me->control[k]=0;goto done; }
        if(k==S8C_CHARGE || k==S8C_MINIMIZE) {
            int delta[7]={0};delta[k==S8C_CHARGE?3:6]=k==S8C_CHARGE?1:2;
            success=s8_change_boost(me,me,delta);
        }
        success=s8_control_add(me,k,duration) || success;goto done;
    }
    if(row->protect && s8_is_protected(foe,row->move)) { attempt_result(me,ATTEMPT_NOT_FAIL);return; }
    if(k==S8C_ATTRACT && !((me->sex==1 && foe->sex==2) || (me->sex==2 && foe->sex==1))) goto done;
    if(!s8_boost_accuracy(me,foe,move_def(row->move),r,0)) goto done;
    if(k==S8C_ELECTRIFY && FIELD.turn>0 && !s8_control_will_move(foe)) goto done;
    if(k==S8C_DISABLE || k==S8C_ENCORE) {
        int last=foe->last_move,slot=find_move(foe,last);
        if(last<0 || (slot>=0 && foe->pp[slot]<=0)) goto done;
        if(k==S8C_ENCORE && (slot<0 || (S8_MOVES[last].flags&S8_FLAG_FAILENCORE))) goto done;
        if(k==S8C_DISABLE && s8_control_will_move(foe)) duration--;
        if(k==S8C_ENCORE && !s8_control_will_move(foe)) duration++;
    }
    if(k==S8C_TAUNT && FIELD.turn>0 && !s8_control_will_move(foe)) duration++;
    if(k==S8C_YAWN && (foe->status || foe->sleep)) goto done;
    if(k==S8C_LOCKON && me->control[k]) goto done;
    if(!row->bypass && s8_sub_blocks(foe,row->move)) goto done;
    if(k==S8C_YAWN && foe->safeguard && !(S8_PROPERTY && S8_PROPERTY->infiltrates)) goto done;
    success=s8_control_add(k==S8C_LOCKON?me:foe,k,duration);
    if(success && k==S8C_DISABLE) foe->disabled_move=foe->last_move;
    if(success && k==S8C_ENCORE) foe->encored_move=foe->last_move;
done:
    if(!success) { attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0); }
}
static void s8_control_hit(Mon *me,Mon *foe,const S8ControlMove *row,int sub,Rng *r) {
    if(!row) return;
    if(row->secondary && (!s8_guard_secondary(foe,0) || (S8_PROPERTY && S8_PROPERTY->sheer) || (int)(rng_dbl(r)*100)>=row->secondary)) return;
    int k=row->kind;
    if(k==S8C_GLAIVERUSH) { s8_control_add(me,k,0);return; }
    if(sub || foe->hp<=0) return;
    if(k==S8C_SMACKDOWN) {
        int applies=has_type(foe,T_FLY) || s8_has_ability(foe,S8D_LEVITATE) || s8_has_ability(foe,S8D_EELEVATE);
        if(current_gravity(foe) || foe->ingrain || held_item(foe)==ITEM_IRONBALL) applies=0;
        if(s8_cancel_air_charge(foe)) applies=1;
        if(foe->magnet_rise) { applies=1;foe->magnet_rise=0; }
        if(!applies) return;
    }
    s8_control_add(foe,k,row->duration);
}
static void s8_control_destiny(Mon *source,Mon *target) {
    if(target->hp>0 || !target->control[S8C_DESTINYBOND] || source->hp<=0) return;
    queue_faint(target,FAINT_DAMAGE);
    int before=source->hp;source->hp=0;queue_faint(source,FAINT_DAMAGE);obs_hp(source,before,HP_COST);
}
// ---------- Stage 8c-9d common volatile lifecycle ----------
static int s8_move_sound(int mv) {
    return mv>=0 && !!(S8_MOVES[mv].flags&S8_FLAG_SOUND);
}
static int s8_rampage_locked(const Mon *m) { return m->rampage_duration || m->uproar; }
static int s8_rampage_slot(const Mon *m,int slot) {
    if(!s8_rampage_locked(m)) return slot;
    return find_move(m,m->rampage_duration?m->rampage_move:S8R_M_UPROAR);
}
static void s8_rampage_end(Mon *m,Rng *r) {
    int confuse=m->rampage_true<=1;
    m->rampage_duration=m->rampage_true=m->rampage_move=0;
    if(confuse && r) {
        Mon *saved=S8_EFFECT_SOURCE;S8_EFFECT_SOURCE=m;
        inflict_confusion(m,r);S8_EFFECT_SOURCE=saved;
    }
}
static void s8_rampage_after(Mon *m,Rng *r) {
    // AfterMove is reached for attempted failures, but not BeforeMove aborts.
    if(m->rampage_duration==1) s8_rampage_end(m,r);
}
static void s8_rampage_hit(Mon *me,Mon *foe,const S8RampageMove *row,int sub,Rng *r) {
    if(!row) return;
    if(row->secondary && (!s8_guard_secondary(foe,0) || (S8_PROPERTY && S8_PROPERTY->sheer) || (int)(rng_dbl(r)*100)>=row->secondary)) return;
    if(row->kind==S8R_THROATCHOP) {
        if(!sub && foe->hp>0 && !foe->throatchop) {
            foe->throatchop=row->duration;foe->state_features|=MON_POST_V1_STATE;
        }
    } else if(me->hp>0) {
        if(row->kind==S8R_UPROAR) {
            if(!me->uproar) { me->uproar=row->duration;me->uproar_struggle=0; }
        } else if(!me->rampage_duration) {
            me->rampage_duration=row->duration;me->rampage_true=2+rng_int(r,2);me->rampage_move=row->move;
        } else if(me->rampage_true>=2) me->rampage_duration=row->duration;
        me->state_features|=MON_POST_V1_STATE;
    }
}
static void s8_rampage_residual(Mon *m,int kind) {
    if(kind==-2 && m->rampage_duration) {
        if(!--m->rampage_duration) s8_rampage_end(m,BATTLE_RNG);
        else if(m->sleep) m->rampage_duration=m->rampage_true=m->rampage_move=0;
        else m->rampage_true--;
    } else if(kind==-3 && m->uproar) {
        if(!--m->uproar || m->throatchop || m->uproar_struggle) m->uproar=m->uproar_struggle=0;
    } else if(kind==-4 && m->throatchop) m->throatchop--;
}

static void s8_control_begin_action(Mon *m) { m->control[S8C_GLAIVERUSH]=0; }
static void s8_control_abort(Mon *m,int mv) {
    m->control[S8C_DESTINYBOND]=0;
    if(mv>=0 && mv!=S8C_M_CHARGE && move_def(mv)->type==T_ELEC) m->control[S8C_CHARGE]=0;
}
static int s8_control_blocked(const Mon *m,const Mon *foe,int mv,int admission) {
    if(mv<0) return 0; // Struggle is never disabled
    if(admission && ((mv==S8U_M_GIGATONHAMMER && m->action_move==mv+1) ||
       ((mv==S8U_M_FAKEOUT || mv==S8U_M_FIRSTIMPRESSION) && FIELD.turn>1))) return 1;
    if(m->control[S8C_DISABLE] && mv==m->disabled_move && !(S8_MOVES[mv].flags&S8_FLAG_CANTUSETWICE)) return 1;
    if(m->throatchop && s8_move_sound(mv)) return 1; // BeforeMove 6, ahead of confusion
    if(m->control[S8C_TAUNT] && move_def(mv)->cat==CAT_STATUS) return 1;
    if(foe->hp>0 && foe->control[S8C_IMPRISON] && find_move(foe,mv)>=0) return 1;
    // Torment is a request restriction, not a BeforeMove handler. It cannot
    // cancel a repeat which was already queued when the volatile was added.
    return admission && m->control[S8C_TORMENT] && m->last_move==mv;
}
static int s8_control_redirect(const Mon *m,int slot) {
    slot=s8_rampage_slot(m,slot);
    if(slot<0 || !m->control[S8C_ENCORE] || m->charging) return slot;
    int forced=find_move(m,m->encored_move);
    return forced>=0 && m->pp[forced]>0?forced:slot;
}
static int s8_control_commit(const Mon *m,const Mon *foe,int slot) {
    if(!FIELD.control_context || m->charging) return slot;
    if(s8_rampage_locked(m)) return s8_rampage_slot(m,slot); // hard-lock request ignores PP/Disable/Torment
    slot=s8_control_redirect(m,slot);
    int available=0,first=-1;
    for(int i=0;i<m->nmoves;i++) if(m->pp[i]>0 && !s8_control_blocked(m,foe,m->moves[i],1) &&
       (!m->control[S8C_ENCORE] || m->moves[i]==m->encored_move)) { available++;if(first<0) first=i; }
    if(!available) return -1;
    if(slot>=0 && s8_control_blocked(m,foe,m->moves[slot],1)) return first;
    return slot;
}
static int s8_control_before_move(Mon *m,Mon *foe,int mv,Rng *r) {
    s8_control_links_update(m,foe);
    if(s8_control_blocked(m,foe,mv,0) || !status_before_move(m,r)) {
        s8_control_abort(m,mv);attempt_result(m,ATTEMPT_SKIP_FAIL);m->charging=0;return 0;
    }
    if(mv!=S8C_M_DESTINYBOND) m->control[S8C_DESTINYBOND]=0; // priority -1
    return 1;
}
// Merge order 13/14 with previous trap handlers. Each same-order handler is
// ordered by holder Speed; tied handlers are sampled, including on one holder.
static void s8_control_residual(Mon *a,Mon *b,int order) {
    if(!FIELD.control_context) return;
    s8_control_links_update(a,b);
    typedef struct { Mon *m; int kind,speed; } ControlResidualJob;
    ControlResidualJob jobs[2*(S8C_COUNT+4)];int n=0;
    Mon *ms[]={a,b};
    for(int i=0;i<2;i++) if(ms[i]->hp>0) {
        Mon *m=ms[i];int speed=FIELD.control_residual_active?FIELD.control_residual_speed[i]:s8_speed_stat(m);
        for(int k=0;k<S8C_COUNT;k++) {
            const S8ControlMove *row=&S8_CONTROL_MOVES[k];
            if(m->control[k] && row->order==order && (row->duration || k==S8C_SALTCURE))
                jobs[n++]=(ControlResidualJob){m,k,speed};
        }
        if(order==0 && m->rampage_duration) jobs[n++]=(ControlResidualJob){m,-2,speed};
        if(order==28 && m->uproar) jobs[n++]=(ControlResidualJob){m,-3,speed};
        if(order==22 && m->throatchop) jobs[n++]=(ControlResidualJob){m,-4,speed};
        if((order==13 && m->partial_trap) || (order==14 && m->octolock)) jobs[n++]=(ControlResidualJob){m,-1,speed};
    }
    while(n) {
        int best=-1,ties[2*(S8C_COUNT+4)],nt=0;
        for(int i=0;i<n;i++) {
            if(best<0 || jobs[i].speed>jobs[best].speed) { best=i;nt=0; }
            if(jobs[i].speed==jobs[best].speed) ties[nt++]=i;
        }
        if(nt>1 && BATTLE_RNG) best=ties[(int)(rng_dbl(BATTLE_RNG)*nt)];
        Mon *m=jobs[best].m,*source=m==a?b:a;int k=jobs[best].kind;
        for(int i=best;i<n-1;i++) jobs[i]=jobs[i+1];
        n--;
        if(m->hp<=0) continue;
        if(k<=-2) s8_rampage_residual(m,k);
        else if(k<0) {
            if(order==13) {
                if(!--m->partial_trap || source->hp<=0) m->partial_trap=m->partial_move=0;
                else s8_link_damage(m,held_item(source)==ITEM_BINDINGBAND?6:S8T_PARTIAL_DIVISOR);
            } else if(source->hp<=0) m->octolock=0;
            else { const int delta[7]={0,-1,0,-1,0,0,0};s8_change_boost(m,source,delta); }
        } else if(k==S8C_SALTCURE) s8_link_damage(m,has_type(m,T_WATER)||has_type(m,T_STEEL)?8:16);
        else if(m->control[k]) {
            int expired=!--m->control[k];
            if(k==S8C_PERISHSONG && expired) { int before=m->hp;m->hp=0;queue_faint(m,FAINT_DAMAGE);obs_hp(m,before,HP_COST); }
            if(k==S8C_YAWN && expired && BATTLE_RNG) {
                Mon *saved=S8_EFFECT_SOURCE;S8_EFFECT_SOURCE=source;S8_YAWN_END=1;
                inflict_sleep(m,BATTLE_RNG);S8_YAWN_END=0;S8_EFFECT_SOURCE=saved;
            }
            if(k==S8C_SYRUPBOMB && !expired && source->hp>0) { const int delta[7]={0,0,0,0,-1,0,0};s8_change_boost(m,source,delta); }
            if(k==S8C_ENCORE && pp_of(m,m->encored_move)<=0) m->control[k]=0;
        }
        s8_control_links_update(a,b);s8_trap_links_update(a,b);
        if(a->hp<=0 || b->hp<=0) return;
    }
}

// ---------- Stage 8c-9a FIELD resolution ----------
static void s8_field_item_update(Mon *a,Mon *b) {
    Mon *ms[]={a,b};
    for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        white_herb_check(m); status_berry_check(m); berry_hp_check(m);
        // Leppa's Update resumes too, including a slot exhausted during suppression.
        if(m->hp>0) for(int slot=0;slot<m->nmoves;slot++) leppa_check(m,slot);
    }
}
static void s8_field_residual(Mon *a,Mon *b,int order) {
    if(!field_member(a)) return;
    if(order==5) {
        if(FIELD.terrain!=TERRAIN_GRASSY) return;
        Mon *ms[]={a,b}; int first=0;
        double sa=s8_action_speed(a),sb=s8_action_speed(b);
        if(sb>sa || (sb==sa && BATTLE_RNG && rng_dbl(BATTLE_RNG)<.5)) first=1;
        for(int k=0;k<2;k++) {
            Mon *m=ms[k?1-first:first];
            if(is_grounded(m) && s8_semi(m)<0) s8_heal_hp(m,s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,m->maxhp/16>0?m->maxhp/16:1,ITEM_LEFTOVERS));
        }
        return;
    }
    if(FIELD.trick_room) FIELD.trick_room--;
    if(FIELD.gravity && !--FIELD.gravity) obs2_emit(EV_GRAVITY_END,a,0,0,0);
    if(FIELD.wonder_room) FIELD.wonder_room--;
    if(FIELD.magic_room && !--FIELD.magic_room) s8_field_item_update(a,b);
    if(FIELD.terrain_turns && !--FIELD.terrain_turns) {
        FIELD.terrain=TERRAIN_NONE; obs2_emit(EV_TERRAIN_END,a,0,0,0);
        FIELD.terrain_source=NULL;s8_field_change(S8AE_TERRAIN_CHANGE,a);
    }
    if(FIELD.fairy_lock) FIELD.fairy_lock--;
}
// ---------- Stage 8d-3b: weather stats, ordered residuals and queue effects ----------
static int s8_weather_status(const Mon *m) { return m->status || m->sleep; }
static void s8_weather_callback(const Mon *holder,S8AbilityEvent *e) {
    Mon *m=(Mon *)holder;int id=s8_current_ability(holder);
    if(e->event==S8AE_SLEEP_COUNTER) { e->value=2;return; }
    if(e->event==S8AE_SPEED) {
        int w=current_weather(m),field=s8_field_weather(m),factor=4096;
        if((id==S8V3_CHLOROPHYLL && w==WEATHER_SUN) ||
           (id==S8V3_SWIFTSWIM && w==WEATHER_RAIN) ||
           (id==S8V3_SANDRUSH && field==WEATHER_SAND) ||
           (id==S8V3_SLUSHRUSH && (field==WEATHER_SNOW || field==WEATHER_HAIL)) ||
           (id==S8V3_SURGESURFER && field_member(m) && FIELD.terrain==TERRAIN_ELECTRIC)) factor=8192;
        if(id==S8V3_QUICKFEET && s8_weather_status(m)) factor=6144;
        e->value=damage_chain(e->value,factor);return;
    }
    if(e->event==S8AE_SPA) {
        if(current_weather(m)==WEATHER_SUN) e->value=damage_chain(e->value,6144);
    } else if(e->event==S8AE_BASE_POWER) {
        if(s8_field_weather(m)==WEATHER_SAND && e->move &&
           (e->move->type==T_ROCK || e->move->type==T_GROUND || e->move->type==T_STEEL)) e->value=damage_chain(e->value,5325);
    } else if(e->event==S8AE_IMMUNITY) {
        if((id==S8V3_ICEBODY && e->detail==S8R_HAIL) ||
           ((id==S8V3_SANDFORCE || id==S8V3_SANDRUSH) && e->detail==S8R_SAND)) e->value=0;
    } else if(e->event==S8AE_INDIRECT) {
        if(e->detail==S8R_POISON) { s8_heal_hp(m,m->maxhp/8>0?m->maxhp/8:1);e->value=0; }
    } else if(e->event==S8AE_WEATHER_TICK) {
        if(m->hp<=0) return;
        if(id==S8V3_ICEBODY && (e->detail==WEATHER_SNOW || e->detail==WEATHER_HAIL))
            s8_heal_hp(m,s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,m->maxhp/16>0?m->maxhp/16:1,ITEM_LEFTOVERS));
        if(current_weather(m)!=e->detail) return;
        if(id==S8V3_RAINDISH && e->detail==WEATHER_RAIN) s8_heal_hp(m,s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,m->maxhp/16>0?m->maxhp/16:1,ITEM_LEFTOVERS));
        if(id==S8V3_SOLARPOWER && e->detail==WEATHER_SUN) m->hp-=s8_resist_loss(m,m->maxhp/8>0?m->maxhp/8:1,S8R_OTHER);
    } else if(e->event==S8AE_PRIORITY) {
        if(e->move && ((id==S8V3_PRANKSTER && e->move->cat==CAT_STATUS) ||
           (id==S8V3_GALEWINGS && e->move->type==T_FLY && m->hp==m->maxhp))) e->value++;
    } else if(e->event==S8AE_FRACTIONAL_PRIORITY) {
        if(id==S8V3_STALL) e->value=-1; // exact tenths; never changes move.priority
        else if(e->move && e->move->cat!=CAT_STATUS && BATTLE_RNG && rng_int(BATTLE_RNG,10)<3) {
            e->value=1;s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    } else if(e->event==S8AE_FOE_TRY_MOVE) {
        const S8Move *move=s8_power_move(e->move);const char *target=move->target;
        if(!target || !strcmp(target,"foeSide")) return;
        int all=!strcmp(target,"all");
        if(all && strcmp(move->id,"perishsong") && strcmp(move->id,"flowershield") && strcmp(move->id,"rototiller")) return;
        if((e->target==holder || all) && e->detail>0 && !FIELD.ignore_breakable && !s8_property_ignored(holder)) {
            e->value=0;s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    } else if(e->event==S8AE_RESIDUAL) {
        if(m->hp<=0) return;
        if(id==S8V3_HYDRATION || id==S8V3_SHEDSKIN) {
            if(s8_weather_status(m) && (id==S8V3_HYDRATION?current_weather(m)==WEATHER_RAIN:
               BATTLE_RNG && rng_int(BATTLE_RNG,100)<33)) {
                s8_ability_record(m,e->event,S8A_PUBLIC);clear_major_status(m);
            }
        } else if(id==S8V3_HEALER) {
            // adjacentAllies() is empty in the admitted single-slot 1on1.
            // No RNG is consumed when no ally has a status.
        } else if(id==S8V3_SPEEDBOOST) {
            if(m->active_turns) { int boost[7]={0,0,0,0,1,0,0};s8_change_boost(m,m,boost); }
        } else if(id==S8V3_MOODY) {
            int candidates[5],n=0,plus=-1,boost[7]={0};
            for(int i=0;i<5;i++) if(m->st[i]<6) candidates[n++]=i;
            if(n && BATTLE_RNG) { plus=candidates[rng_int(BATTLE_RNG,n)];boost[plus]=2; }
            n=0;
            for(int i=0;i<5;i++) if(m->st[i]>-6 && i!=plus) candidates[n++]=i;
            if(n && BATTLE_RNG) boost[candidates[rng_int(BATTLE_RNG,n)]]=-1;
            s8_change_boost(m,m,boost); // one boost object, original ranks choose both
        }
    }
}
static int s8_weather_context(const Mon *a,const Mon *b) {
    return s8_weather_implemented(s8_current_ability(a)) || s8_weather_implemented(s8_current_ability(b));
}
static void s8_weather_peer(const Mon *holder,S8AbilityEvent *e) {
    Mon *m=(Mon *)holder;
    if(m->hp<=0) return;
    if(e->detail==1) { // Grassy Terrain, residual 5.2
        if(is_grounded(m) && s8_semi(m)<0) s8_heal_hp(m,s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,m->maxhp/16>0?m->maxhp/16:1,ITEM_LEFTOVERS));
    } else if(e->detail==2) { // Leftovers, residual 5.4
        if(held_item(m)==ITEM_LEFTOVERS && s8_heal_hp(m,s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,m->maxhp/16>0?m->maxhp/16:1,ITEM_LEFTOVERS))) obs_item(m,0);
    } else s8_rampage_residual(m,-3); // Uproar duration, residual 28.1
}
static void s8_weather_grass(const Mon *m,S8AbilityEvent *e) { S8AbilityEvent x=*e;x.detail=1;s8_weather_peer(m,&x); }
static void s8_weather_leftovers(const Mon *m,S8AbilityEvent *e) { S8AbilityEvent x=*e;x.detail=2;s8_weather_peer(m,&x); }
static void s8_weather_uproar(const Mon *m,S8AbilityEvent *e) { S8AbilityEvent x=*e;x.detail=3;s8_weather_peer(m,&x); }
static void s8_weather_residual(Mon *a,Mon *b,int order) {
    Mon *ms[]={a,b};S8AbilityCall calls[8];int n=0;
    const S8AbilityHandler grass={-1,S8AE_RESIDUAL,5,0,2,s8_weather_grass};
    const S8AbilityHandler leftovers={-1,S8AE_RESIDUAL,5,0,4,s8_weather_leftovers};
    const S8AbilityHandler uproar={-1,S8AE_RESIDUAL,28,0,1,s8_weather_uproar};
    for(int k=0;k<2;k++) if(ms[k]->hp>0) {
        if(order==5 && FIELD.terrain==TERRAIN_GRASSY) calls[n++]=(S8AbilityCall){ms[k],&grass,FIELD.ability_residual_speed[k],0};
        if(order==5 && held_item(ms[k])==ITEM_LEFTOVERS) calls[n++]=(S8AbilityCall){ms[k],&leftovers,FIELD.ability_residual_speed[k],0};
        if(order==28 && ms[k]->uproar) calls[n++]=(S8AbilityCall){ms[k],&uproar,FIELD.ability_residual_speed[k],0};
    }
    for(int k=0;k<2;k++) if(ms[k]->hp>0)
        for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
            const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
            if(row->event==S8AE_RESIDUAL && row->order==order && s8_weather_implemented(row->ability) && s8_has_ability(ms[k],row->ability))
                calls[n++]=(S8AbilityCall){ms[k],row,FIELD.ability_residual_speed[k],0};
        }
    S8AbilityEvent e={S8AE_RESIDUAL,NULL,NULL,NULL,1,0};
    s8_ability_run_queue(calls,n,&e,BATTLE_RNG);
}
static void s8_earlybird_before_move(Mon *m) {
    if(m->sleep>0 && s8_has_ability(m,S8V3_EARLYBIRD)) {
        m->sleep-=s8_ability_event(m,S8AE_SLEEP_COUNTER,m,m,NULL,1,0);
        obs2_emit(EV_SLEEP_TICK,m,0,0,0);
        if(m->sleep<=0) { m->sleep=1;clear_major_status(m); }
    }
}
static int s8_prankster_immune(const Mon *me,const Mon *target) {
    return target!=me && S8_PROPERTY && S8_PROPERTY->prankster && s8_has_ability(me,S8V3_PRANKSTER) && has_type(target,T_DARK);
}
static int s8_order_try_move(Mon *me,Mon *foe,int mv) {
    const char *target=S8_MOVES[mv].target;
    int self=!strcmp(target,"self") || !strcmp(target,"allies") || !strcmp(target,"adjacentAllyOrSelf") || !strcmp(target,"allySide") || !strcmp(target,"allyTeam");
    int priority=S8_PROPERTY?S8_PROPERTY->priority:s8_move_priority(me,mv);
    if(!s8_ability_event(foe,S8AE_FOE_TRY_MOVE,me,self?me:foe,move_def(mv),1,priority)) return 0;
    return 1;
}

// ---------- Stage 8d-3a: shared field setters and reviewed field effects ----------
static void s8_field_change(S8AbilityEventId event,const Mon *source) {
    for(int i=0;i<2;i++) if(FIELD.seat[i] && FIELD.seat[i]->hp>0)
        s8_ability_event(FIELD.seat[i],event,source,FIELD.seat[i],NULL,1,0);
    if(event==S8AE_TERRAIN_CHANGE)
        for(int i=0;i<2;i++) if(FIELD.seat[i]) s8_seed_check(FIELD.seat[i]);
}
static int s8_duration(const Mon *source,int kind,int weather) {
    int item=held_item(source);
    if(weather==WEATHER_HAIL) weather=WEATHER_SNOW;
    for(size_t i=0;i<sizeof(S8_DURATION_ITEMS)/sizeof(*S8_DURATION_ITEMS);i++) {
        const S8DurationItem *row=&S8_DURATION_ITEMS[i];
        if(row->item==item && row->kind==kind && (kind!=S8D_WEATHER || row->weather==weather)) return 8;
    }
    return 5;
}
static int s8_field_duration(const Mon *source,int weather) {
    return s8_duration(source,weather?S8D_WEATHER:S8D_TERRAIN,weather);
}
static int s8_set_weather(Mon *source,int weather) {
    if(!field_member(source) || FIELD.weather==weather) return 0;
    if(FIELD.weather) obs2_emit(EV_WEATHER_END,source,0,0,0);
    FIELD.weather=weather;FIELD.weather_turns=s8_field_duration(source,weather);FIELD.weather_source=source;
    FIELD.seat[0]->weather=FIELD.seat[1]->weather=weather;
    obs2_emit(EV_WEATHER_START,source,weather,0,0);
    obs3_duration_announce(source,O3_WEATHER,FIELD.weather_turns,1);
    s8_field_change(S8AE_WEATHER_CHANGE,source);return 1;
}
static int s8_set_terrain(Mon *source,int terrain) {
    if(!field_member(source) || FIELD.terrain==terrain) return 0;
    if(FIELD.terrain) obs2_emit(EV_TERRAIN_END,source,0,0,0);
    FIELD.terrain=terrain;FIELD.terrain_turns=s8_field_duration(source,0);FIELD.terrain_source=source;
    obs2_emit(EV_TERRAIN_START,source,terrain,0,0);
    obs3_duration_announce(source,O3_TERRAIN,FIELD.terrain_turns,1);
    s8_field_change(S8AE_TERRAIN_CHANGE,source);return 1;
}
static void s8_field_callback(const Mon *holder,S8AbilityEvent *e) {
    Mon *m=(Mon *)holder;int id=s8_current_ability(holder);
    if(id==S8W_CLOUDNINE) {
        if(e->event==S8AE_SWITCH_IN) { s8_ability_record(holder,e->event,S8A_PUBLIC);return; }
        m->ability_state.ending=e->event==S8AE_END;
        s8_field_change(S8AE_WEATHER_CHANGE,holder);return;
    }
    if(e->event==S8AE_BASE_POWER) {
        if(e->source!=e->target && e->move && e->move->cat!=CAT_STATUS && e->move->type==T_FAIRY && !S8_FIELD_AURA) {
            S8_FIELD_AURA=holder;e->value=damage_chain(e->value,5448);
        } // Aura Break is not admitted by this unit.
        return;
    }
    if(e->event==S8AE_WEATHER_DAMAGE) {
        // Exact sunnyday callback, followed by Mega Sol's fast exit.
        if(!strcmp(s8_power_move(e->move)->id,"hydrosteam") || e->move->type==T_FIRE) e->value=6144;
        else if(e->move->type==T_WATER) e->value=2048;
        return;
    }
    for(unsigned i=0;i<sizeof(S8_FIELD_ABILITIES)/sizeof(*S8_FIELD_ABILITIES);i++) {
        const S8FieldAbility *row=&S8_FIELD_ABILITIES[i];
        if(row->ability!=id || row->event!=(int)e->event) continue;
        if(row->weather || row->terrain) {
            // The Kyogre/Blue Orb and Groudon/Red Orb exceptions are outside
            // this dataset; retain the raw identity guard for future vocabularies.
            if(e->event==S8AE_START && m->spec_v3 &&
               ((!strcmp(S8_SPECIES[m->table_species_id].id,"kyogre") && s8_hit_has_item(m,"blueorb")) ||
                (!strcmp(S8_SPECIES[m->table_species_id].id,"groudon") && s8_hit_has_item(m,"redorb")))) return;
            int changed=row->weather?s8_set_weather(m,row->weather):s8_set_terrain(m,row->terrain);
            if(changed) s8_ability_record(holder,e->event,S8A_PUBLIC);
            return;
        }
    }
    if(id==S8W_FAIRYAURA) { s8_ability_record(holder,e->event,S8A_PUBLIC);return; }
    if(id==S8W_INTIMIDATE || id==S8W_SUPERSWEETSYRUP) {
        if(id==S8W_SUPERSWEETSYRUP) {
            if(m->ability_individual_flags&UINT64_C(1)) return;
            m->ability_individual_flags|=UINT64_C(1);
            s8_ability_record(holder,e->event,S8A_PUBLIC);
        }
        Mon *foe=field_member(holder)?FIELD.seat[holder==FIELD.seat[0]]:NULL;
        if(!foe || foe->hp<=0) return;
        if(id==S8W_INTIMIDATE) s8_ability_record(holder,e->event,S8A_PUBLIC);
        if(foe->sub_hp) { obs_emit(EV_HIT,foe,HIT_IMMUNE,0,0);return; }
        int delta[7]={0};delta[id==S8W_INTIMIDATE?0:6]=-1;
        int effect=S8_BOOST_EFFECT;const MoveDef *status=S8_STATUS_EFFECT;
        Mon *source=S8_EFFECT_SOURCE;
        S8_BOOST_EFFECT=id;S8_STATUS_EFFECT=NULL;S8_EFFECT_SOURCE=m;
        // isSecondary=true: Clear Body/White Smoke/Flower Veil hide the
        // prevention message, while reactions still see the ability source.
        s8_change_boost_event(foe,m,delta,0);
        S8_BOOST_EFFECT=effect;S8_STATUS_EFFECT=status;S8_EFFECT_SOURCE=source;return;
    }
    if(id==S8W_SCREENCLEANER && field_member(holder)) {
        Mon *sides[]={m,FIELD.seat[holder==FIELD.seat[0]]};int activated=0;
        for(int screen=0;screen<3;screen++) for(int side=0;side<2;side++) {
            Mon *target=sides[side];
            int *timer=screen==0?&target->reflect:screen==1?&target->light_screen:&target->aurora_veil;
            if(!*timer) continue;
            if(!activated++) s8_ability_record(holder,e->event,S8A_PUBLIC);
            *timer=0;if(screen<2) obs2_emit(EV_SCREEN_END,target,screen,0,0);
        }
    } else if(id==S8W_TOXICDEBRIS && e->move && e->move->cat==CAT_PHYS && e->source) {
        Mon *side=(Mon *)e->source;
        if(s8_resist_allies(holder,side)) side=field_member(side)?FIELD.seat[side==FIELD.seat[0]]:NULL;
        if(side && side->toxic_spikes<2) {
            const S8TrapMove *hazard=s8_trap_move(S8T_TOXICSPIKES);
            if(hazard && s8_add_hazard(side,hazard)) s8_ability_record(holder,e->event,S8A_PUBLIC);
        }
    }
}

static void s8_execute_field(Mon *me,Mon *foe,const S8FieldMove *row) {
    if(!field_member(me)) field_bind(me,foe);
    if(row->kind==S8F_TERRAIN) {
        if(!s8_set_terrain(me,row->value)) goto fail;
    } else if(row->kind==S8F_WEATHER) {
        if(!s8_set_weather(me,WEATHER_SNOW)) goto fail;
    } else if(row->kind==S8F_SIDE) {
        int *timer=row->move==S8F_AURORAVEIL?&me->aurora_veil:
            row->move==S8F_SAFEGUARD?&me->safeguard:&me->tailwind;
        if(*timer || (row->move==S8F_AURORAVEIL && current_weather(me)!=WEATHER_SNOW)) goto fail;
        *timer=row->move==S8F_AURORAVEIL?s8_duration(me,S8D_SCREEN,0):row->duration;
    } else {
        int *timer=row->move==S8F_TRICKROOM?&FIELD.trick_room:row->move==S8F_MAGICROOM?&FIELD.magic_room:
            row->move==S8F_WONDERROOM?&FIELD.wonder_room:&FIELD.fairy_lock;
        if(row->move==S8F_FAIRYLOCK && *timer) goto fail;
        *timer=*timer?0:row->duration;
        if(row->move==S8F_MAGICROOM && !*timer) s8_field_item_update(me,foe);
    }
    me->state_features|=MON_POST_V1_STATE; foe->state_features|=MON_POST_V1_STATE;
    return;
fail:
    attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0);
}
// The three new residual orders never draw or change state for old vocabulary.
static void s8_stateful_residual(Mon *a,Mon *b,int order) {
    Mon *ms[]={a,b}; int active[2];
    for(int i=0;i<2;i++) active[i]=order==4?ms[i]->wish_pending:
        order==6?ms[i]->aquaring:ms[i]->ingrain;
    int first=0;
    if(active[0] && active[1]) {
        double sa=s8_action_speed(a),sb=s8_action_speed(b);
        if(sb>sa || (sb==sa && BATTLE_RNG && rng_dbl(BATTLE_RNG)<.5)) first=1;
    }
    for(int k=0;k<2;k++) {
        int i=k?1-first:first; Mon *m=ms[i];
        if(!active[i]) continue;
        if(order==4) {
            if(((FIELD.turn-1)&255)<=m->wish_start) continue;
            m->wish_pending=0;
            s8_heal_hp(m,m->wish_hp); m->wish_hp=m->wish_start=0;
        } else {
            int amount=m->maxhp/16;
            s8_heal_from(m,amount>0?amount:1,order==6?3:4);
        }
    }
}
static void s8_execute_stateful_heal(Mon *me,const S8StatefulHeal *row) {
    if(me->hp<=0) goto fail;
    switch(row->kind) {
    case S8SH_RING:
    case S8SH_ROOT: {
        int *flag=row->kind==S8SH_RING?&me->aquaring:&me->ingrain;
        if(*flag) goto fail;
        *flag=1; break;
    }
    case S8SH_BELL:
        if(!me->status && !me->sleep) goto fail;
        clear_major_status(me); break;
    case S8SH_WEATHER: {
        int weather=current_weather(me);
        int modifier=weather==WEATHER_SUN?2732:weather==WEATHER_NONE?2048:1024;
        if(!s8_heal_hp(me,((int64_t)me->maxhp*modifier+2047)/4096))
            attempt_result(me,ATTEMPT_NOT_FAIL);
        break;
    }
    case S8SH_ROOST:
        if(!s8_heal_hp(me,(me->maxhp+1)/2)) goto fail;
        me->roost=1; break;
    case S8SH_SWALLOW: {
        if(!me->stockpile) goto fail;
        int modifier=me->stockpile==1?1024:me->stockpile==2?2048:4096;
        if(!s8_heal_hp(me,((int64_t)me->maxhp*modifier+2047)/4096))
            attempt_result(me,ATTEMPT_NOT_FAIL);
        s8_consume_stockpile(me); break;
    }
    case S8SH_WISH:
        if(me->wish_pending) goto fail;
        me->wish_pending=1; me->wish_start=(FIELD.turn-1)&255;
        me->wish_hp=me->maxhp/2; if(!me->wish_hp) me->wish_hp=1;
        break;
    default: goto fail;
    }
    return;
fail:
    attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0);
}
static void s8_execute_heal(Mon *me,Mon *foe,const S8HealMove *row,Rng *r) {
    Mon *target=row->target?foe:me;
    if(row->target) {
        if(row->protect && s8_is_protected(foe,row->move)) {
            attempt_result(me,ATTEMPT_NOT_FAIL);obs_emit(EV_HIT,me,HIT_PROTECT,0,0);return;
        }
        if(!s8_boost_accuracy(me,foe,move_def(row->move),r,0)) goto fail;
        if(!row->bypass && s8_sub_blocks(foe,row->move)) goto fail;
    }
    switch(row->kind) {
    case S8H_SIMPLE:
        if(!s8_heal_hp(target,((int64_t)target->maxhp*row->num+row->den/2)/row->den)) goto fail;
        break;
    case S8H_PULSE:
        // Heal Pulse owns this move-local Mega Launcher interaction.
        if(!s8_heal_hp(target,s8_ability_event(me,S8AE_HEAL_PULSE,me,target,
            move_def(row->move),(target->maxhp+1)/2,target->maxhp))) attempt_result(me,ATTEMPT_NOT_FAIL);
        break;
    case S8H_SAP: {
        if(foe->st[0]==-6) goto fail;
        // getStat(atk,false,true): stages included, burn/items excluded.
        int rank=s8_property_rank(foe,0,foe->st[0]),atk=rank>=0?foe->stats[1]*(2+rank)/2:foe->stats[1]*2/(2-rank);
        const int delta[7]={-1,0,0,0,0,0,0};
        int success=s8_change_boost(foe,me,delta);
        if(!s8_property_drain(me,foe,atk,2) && !success) goto fail;
        break;
    }
    case S8H_SPLIT: {
        int average=(me->hp+foe->hp)/2;if(average<1) average=1;
        Mon *mons[]={foe,me};
        for(int i=0;i<2;i++) {
            Mon *m=mons[i];int before=m->hp;
            if(m->hp>0) m->hp=average<m->maxhp?average:m->maxhp;
            obs_hp(m,before,m->hp>=before?HP_HEAL:HP_COST);
        }
        // sethp is neither damage nor heal: no Heal Block, Sash or recoil.
        berry_hp_check(foe);berry_hp_check(me);break;
    }
    default: goto fail;
    }
    return;
fail:
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);
}
static void s8_execute_primary_status(Mon *me,Mon *foe,int mv,Rng *r) {
    const S8PrimaryStatus *row=s8_primary_status(mv);
    if(row->protect && s8_is_protected(foe,row->move)) {
        attempt_result(me,ATTEMPT_NOT_FAIL); obs_emit(EV_HIT,me,HIT_PROTECT,0,0); return;
    }
    // Status moves ignore the damage type chart unless explicitly opted in.
    if((row->type_immunity && damage_effectiveness(foe,mv)==0) ||
       (row->powder && has_type(foe,T_GRASS))) {
        attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_IMMUNE,0,0); return;
    }
    // No accuracy call for Poison Toxic: also the future semi-invulnerable seam.
    if(!(mv==S8S_TOXIC && has_type(me,T_POISON)) &&
       !s8_boost_accuracy(me,foe,move_def(mv),r,0)) {
        attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_MISS,0,0); return;
    }
    if(!row->bypass && s8_sub_blocks(foe,row->move)) {
        attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_SUB,0,0); return;
    }
    // runMoveEffects order: boosts, primary status, volatile. A primary status
    // failure ends the move even if its preceding boost changed a rank.
    int success=0,has_boost=0;
    for(int i=0;i<7;i++) if(row->delta[i]) has_boost=1;
    if(has_boost) success=s8_change_boost(foe,me,row->delta);
    if(row->status || row->sleep) {
        if(!(row->sleep?inflict_sleep(foe,r):inflict_status(foe,row->status,r))) goto fail;
        success=1;
    }
    if(row->confusion) {
        success=inflict_confusion(foe,r) || success;
        s8_ability_event(foe,S8AE_HIT,me,foe,move_def(mv),1,0);
    }
    if(success) return;
fail:
    attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0);
}
static void s8_execute_boost(Mon *me,Mon *foe,int mv,Rng *r) {
    const S8BoostMove *row=s8_boost_move(mv);
    if(!row) { fprintf(stderr,"status boost rejected: unimplemented table ID %d\n",mv); exit(2); }
    int delta[7];memcpy(delta,row->delta,sizeof(delta));
    Mon *target=row->target==1?foe:me;
    int success=1;
    if((mv==S8B_STOCKPILE && me->stockpile>=3) || (mv==S8B_NORETREAT && me->no_retreat) ||
       (mv==S8B_CLANGOROUSSOUL && (me->maxhp==1 || me->hp*100<=me->maxhp*33))) goto fail;
    if(row->target==2) goto fail; // adjacent ally absent in single-mon format
    if(row->target==1) {
        if(row->protect && s8_is_protected(foe,row->move)) { attempt_result(me,ATTEMPT_NOT_FAIL); return; }
        if(row->powder && has_type(foe,T_GRASS)) goto fail;
        if(!s8_boost_accuracy(me,foe,move_def(mv),r,0)) goto fail;
        if(!row->bypass && s8_sub_blocks(foe,row->move)) goto fail;
    }
    switch(mv) {
    case S8B_ACUPRESSURE: {
        int choices[7],count=0;
        for(int i=0;i<7;i++) if(*s8_boost_stage(me,i)<6) choices[count++]=i;
        if(!count) goto fail;
        delta[choices[(int)(rng_dbl(r)*count)]]=2;
        success=s8_change_boost(me,me,delta); break;
    }
    case S8B_BELLYDRUM: {
        if(me->maxhp==1 || me->hp*2<=me->maxhp || me->st[0]>=6) goto fail;
        int before=me->hp;me->hp-=me->maxhp/2;obs_hp(me,before,HP_COST);
        delta[0]=12;s8_change_boost(me,me,delta);berry_hp_check(me);break;
    }
    case S8B_CLANGOROUSSOUL: {
        if(!s8_change_boost(me,me,delta)) goto fail;
        int before=me->hp,cost=me->maxhp*33/100;
        me->hp-=cost>0?cost:1;obs_hp(me,before,HP_COST);berry_hp_check(me);break;
    }
    case S8B_CURSE:
        delta[0]=delta[1]=1;delta[4]=-1;success=s8_change_boost(me,me,delta);break;
    case S8B_GROWTH:
        if(current_weather(me)==WEATHER_SUN) delta[0]=delta[2]=2;
        success=s8_change_boost(me,me,delta);break;
    case S8B_MAGNETICFLUX: {
        // Predicate only, no Plus/Minus ability mechanics implemented here.
        int aid=s8_current_ability(me);
        const char *ability=aid>=0 && aid<S8_ABILITIES_COUNT && s8_has_ability(me,aid)?S8_ABILITIES[aid].id:"";
        if(strcmp(ability,"plus") && strcmp(ability,"minus")) goto fail;
        delta[1]=delta[3]=1;success=s8_change_boost(me,me,delta);break;
    }
    case S8B_NORETREAT:
        s8_change_boost(me,me,delta);me->no_retreat=1;break;
    case S8B_STOCKPILE: {
        int defense=me->st[1],special=me->st[3];
        delta[1]=delta[3]=1;s8_change_boost(me,me,delta);
        me->stockpile++;
        if(me->st[1]!=defense) me->stock_def--;
        if(me->st[3]!=special) me->stock_spd--;
        break;
    }
    case S8B_HAZE:
        for(int i=0;i<7;i++) { *s8_boost_stage(me,i)=0;*s8_boost_stage(foe,i)=0; }
        break;
    case S8B_TOPSYTURVY:
        success=0;
        for(int i=0;i<7;i++) { int *v=s8_boost_stage(foe,i);if(*v) success=1;*v=-*v; }
        white_herb_check(foe);break;
    case S8B_GUARDSWAP: case S8B_POWERSWAP:
        for(int i=mv==S8B_POWERSWAP?0:1;i<4;i+=2) { int t=me->st[i];me->st[i]=foe->st[i];foe->st[i]=t; }
        white_herb_check(me);white_herb_check(foe);break;
    case S8B_GUARDSPLIT: case S8B_POWERSPLIT:
        for(int i=mv==S8B_POWERSPLIT?1:2;i<=4;i+=2) me->stats[i]=foe->stats[i]=(me->stats[i]+foe->stats[i])/2;
        break;
    case S8B_SPEEDSWAP: { int t=me->stats[5];me->stats[5]=foe->stats[5];foe->stats[5]=t;break; }
    case S8B_POWERTRICK: { int t=me->stats[1];me->stats[1]=me->stats[2];me->stats[2]=t;me->power_trick^=1;break; }
    default: success=s8_change_boost(target,me,delta);break;
    }
    if(success) return;
fail:
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);
}

static int s8_attack_accuracy(Mon *a,Mon *b,int mv,Rng *r) {
    if(s8_control_accuracy(a,b,move_def(mv))) return 1;
    if(s8_ohko(mv)) {
        int chance=s8_ohko_chance(a,b,mv,50,50);
        return chance>=0 && (int)(rng_dbl(r)*100)<chance;
    }
    return s8_multihit_accuracy(a,b,move_def(mv),r,1);
}
// ---------- Stage 8c-10b resolution (no policy reads queue state) ----------
static void execute_move(Mon *me,Mon *foe,int mv,Rng *r);
static void s8_run_instruct(Mon *me,Mon *foe,int mv,Rng *r);
static int s8_semi_miss(const Mon *me,const Mon *foe,int mv) {
    int charge=s8_semi(foe);
    if(s8_boost_any((Mon *)me,(Mon *)foe,S8AE_INVULNERABILITY,move_def(mv),1,0)==0) return 0;
    if(charge<0 || CUR_FUTURE_IMPACT || me->control[S8C_LOCKON] || (mv==S8S_TOXIC && has_type(me,T_POISON))) return 0;
    const char *target=S8_MOVES[mv].target;
    if(!strcmp(target,"self") || !strcmp(target,"all") || !strcmp(target,"allySide") || !strcmp(target,"foeSide") || !strcmp(target,"allyTeam")) return 0;
    for(size_t i=0;i<sizeof(S8_SEMI_EXCEPTIONS)/sizeof(*S8_SEMI_EXCEPTIONS);i++)
        if(S8_SEMI_EXCEPTIONS[i].charge==charge && S8_SEMI_EXCEPTIONS[i].attack==mv && !S8_SEMI_EXCEPTIONS[i].phase) return 0;
    return 1;
}
// ---------- Stage 8c-11a: shared current-type and ability mutation ----------
static int s8_type_mutable(const Mon *m) {
    if(m->terastallized) return 0;
    if(!m->spec_v3) return 1;
    const char *base=S8_SPECIES[S8_SPECIES[m->table_species_id].base_id].id;
    return strcmp(base,"arceus") && strcmp(base,"silvally");
}
static int s8_set_types(Mon *m,int first,int second,int added) {
    if(!s8_type_mutable(m)) return 0;
    if(first==-1) { first=second;second=-1; }
    m->type1=first;m->type2=second;m->added_type=added;
    if(OBS && OBS->version==3) obs_emit(EV3_TYPES,m,first,second,added);
    m->state_features|=MON_POST_V1_STATE;return 1;
}
static void s8_assign_ability(Mon *m,int id) {
    if(m->spec_v3 && !m->ability_changed && m->table_species_id==S8_METAGROSS && m->ability==AB_LM)
        m->weight_hg=S8_SPECIES[m->table_species_id].weight_hg;
    m->table_ability_id=id;m->ability_changed=1;
    m->ability=id<0?AB_NONE:s8_ability_projection(id);
    m->state_features|=MON_POST_V1_STATE;
    s8_ability_new_state(m);
}
static int s8_set_ability(Mon *m,int id) {
    if(m->hp<=0 || (s8_ability_flags(m)&S8TA_CANTSUPPRESS) ||
       (id>=0 && (S8_ABILITY_OPERATION_FLAGS[id]&S8TA_CANTSUPPRESS))) return 0;
    if(s8_hit_has_item(m,"abilityshield")) return 0;
    s8_ability_end(m);s8_assign_ability(m,id);
    s8_ability_record(m,S8AE_REVEAL,S8A_PUBLIC);s8_ability_start(m);return 1;
}
// Keep the predecessor's trap calculation intact; item priority -10 follows it.
#define s8_switch_traps s8_switch_traps_before_items
static void obs3_reveal_sink(int kind,const Mon *holder,const Mon *target,int value,int detail,int silent) {
#ifdef ENGINE_REVEAL_EVENT_SINK
    ENGINE_REVEAL_EVENT_SINK(kind,holder,target,value,detail,silent);
#endif
    obs3_reveal_message(kind,holder,target,value,detail,silent);
}
#undef ENGINE_REVEAL_EVENT_SINK
#define ENGINE_REVEAL_EVENT_SINK obs3_reveal_sink
#include "stage8/ability_switch_impl_8d4c.h"
#undef ENGINE_REVEAL_EVENT_SINK
#undef s8_switch_traps
static void s8_switch_traps(Mon *m) {
    s8_switch_traps_before_items(m);
    if(field_member(m) && !m->item_used && held_item(m)==ITEM_SHEDSHELL) {
        m->switch_trapped=m->maybe_trapped=0;
    }
}
#include "stage8/ability_form_impl_8d4c.h"
static int s8_type_modify(Mon *me,const Mon *foe,int mv,Rng *r) {
    const S8TypeAbilityMove *row=s8_type_ability_move(mv);
    if(!row || row->kind!=S8TA_ATTACK) return 0;
    int tied=0;
    MoveDef active=*move_def(mv);
    const char *form=me->spec_v3?S8_SPECIES[me->table_species_id].id:"";
    if(mv==S8TA_M_AURAWHEEL) active.type=!strcmp(form,"morpekohangry")?T_DARK:T_ELEC;
    if(mv==S8TA_M_RAGINGBULL) {
        if(!strcmp(form,"taurospaldeacombat")) active.type=T_FIGHT;
        if(!strcmp(form,"taurospaldeablaze")) active.type=T_FIRE;
        if(!strcmp(form,"taurospaldeaaqua")) active.type=T_WATER;
    }
    if(mv==S8TA_M_WEATHERBALL && current_weather(me)) {
        int w=current_weather(me);
        active.type=w==WEATHER_SUN?T_FIRE:w==WEATHER_RAIN?T_WATER:w==WEATHER_SAND?T_ROCK:T_ICE;
        active.power*=2;
    }
    if(mv==S8TA_M_TERRAINPULSE && field_member(me) && FIELD.terrain && is_grounded(me)) {
        active.type=FIELD.terrain==TERRAIN_ELECTRIC?T_ELEC:FIELD.terrain==TERRAIN_GRASSY?T_GRASS:FIELD.terrain==TERRAIN_MISTY?T_FAIRY:FIELD.terrain==TERRAIN_PSYCHIC?T_PSY:active.type;
        active.power*=2;
    }
    if(mv==S8TA_M_SHELLSIDEARM) {
        // getStat(false,true) keeps stored defenses but swaps their stage names
        // under Wonder Room. Damage itself swaps raw defenses separately.
        int wonder=field_member(me) && FIELD.wonder_room;
        int attack=damage_stat(me->stats[1],s8_property_rank(me,0,me->st[0])),spa=damage_stat(me->stats[3],s8_property_rank(me,2,me->st[2]));
        int defense=damage_stat(foe->stats[2],s8_property_rank(foe,wonder?3:1,foe->st[wonder?3:1]));
        int spd=damage_stat(foe->stats[4],s8_property_rank(foe,wonder?1:3,foe->st[wonder?1:3]));
        int physical=(int)((int64_t)22*active.power*attack/defense/50);
        int special=(int)((int64_t)22*active.power*spa/spd/50);
        tied=physical==special;
        if(physical>special || (tied && r && rng_dbl(r)<0.5)) {
            active.cat=CAT_PHYS;active.contact=1;
        }
    }
    S8_CONTROL_OVERRIDE=active;S8_CONTROL_OVERRIDE_ID=mv;
    return tied;
}
static int s8_type_eligible(const Mon *me,int mv) {
    int required=mv==S8TA_M_BURNUP?T_FIRE:mv==S8TA_M_DOUBLESHOCK?T_ELEC:-1;
    int fail=required>=0 && !has_type(me,required);
    if(mv==S8TA_M_AURAWHEEL) {
        const char *form=me->spec_v3?S8_SPECIES[S8_SPECIES[me->table_species_id].base_id].id:"";
        fail=strcmp(form,"morpeko")!=0;
    }
    return !fail;
}
static int s8_type_try(Mon *me,int mv) {
    if(s8_type_eligible(me,mv)) return 0;
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
}
static int s8_type_status(Mon *me,Mon *foe,int mv,Rng *r) {
    const S8TypeAbilityMove *row=s8_type_ability_move(mv);
    if(!row || row->kind==S8TA_ATTACK) return 0;
    int own=s8_current_ability(me),other=s8_current_ability(foe);
    unsigned sf=s8_ability_flags(me),tf=s8_ability_flags(foe);
    if(s8_is_protected(foe,mv) && (S8_MOVES[mv].flags&S8_FLAG_PROTECT)) {
        attempt_result(me,ATTEMPT_NOT_FAIL);obs_emit(EV_HIT,me,HIT_PROTECT,0,0);return 1;
    }
    // TryImmunity precedes accuracy; move-local TryHit follows it.
    if((S8_MOVES[mv].flags&S8_FLAG_POWDER) && has_type(foe,T_GRASS)) goto fail;
    if(row->kind==S8TA_WORRY && (other==S8TA_AB_INSOMNIA || (other>=0 && other==S8TA_AB_TRUANT))) goto fail;
    if(!s8_boost_accuracy(me,foe,move_def(mv),r,0)) goto fail;
    if(row->kind==S8TA_ENTRAIN && (own==other || (tf&S8TA_CANTSUPPRESS) || (other>=0 && other==S8TA_AB_TRUANT) || (sf&S8TA_NOENTRAIN))) goto fail;
    if(row->kind==S8TA_ROLE && (own==other || (tf&S8TA_FAILROLEPLAY) || (sf&S8TA_CANTSUPPRESS))) goto fail;
    if(row->kind==S8TA_SIMPLE && ((tf&S8TA_CANTSUPPRESS) || other==S8TA_AB_SIMPLE || (other>=0 && other==S8TA_AB_TRUANT))) goto fail;
    if((row->kind==S8TA_WORRY || row->kind==S8TA_SUPPRESS) && (tf&S8TA_CANTSUPPRESS)) goto fail;
    if(s8_sub_blocks(foe,mv) && !(S8_MOVES[mv].flags&S8_FLAG_BYPASSSUB)) goto fail;
    switch(row->kind) {
    case S8TA_ADD:
        if(has_type(foe,row->type)) goto fail;
        foe->added_type=row->type+1;foe->state_features|=MON_POST_V1_STATE;
        if(OBS && OBS->version==3) obs_emit(EV3_TYPES,foe,foe->type1,foe->type2,foe->added_type);
        break;
    case S8TA_SET: {
        int t1=s8_effective_type(foe,0),t2=s8_effective_type(foe,1);
        int same=((t1==row->type && t2==-1) || (t2==row->type && t1==-1)) && !foe->added_type;
        if(same || !s8_set_types(foe,row->type,-1,0)) goto fail;
        break;
    }
    case S8TA_REFLECT: {
        int t1=s8_effective_type(foe,0),t2=s8_effective_type(foe,1);
        if(t1==-2) t1=-1;
        if(t2==-2) t2=-1;
        if(t1==-1 && t2==-1) { if(!foe->added_type) goto fail;t1=T_NORMAL; }
        if(!s8_set_types(me,t1,t2,foe->added_type)) goto fail;
        break;
    }
    case S8TA_SUPPRESS:
        if(foe->ability_suppressed) goto fail;
        s8_ability_suppress(foe,1);
        s8_ability_record(foe,S8AE_SUPPRESS,S8A_PUBLIC);break;
    case S8TA_ROLE: if(!s8_set_ability(me,other)) goto fail;break;
    case S8TA_ENTRAIN: if(!s8_set_ability(foe,own)) goto fail;break;
    case S8TA_SIMPLE: if(!s8_set_ability(foe,S8TA_AB_SIMPLE)) goto fail;break;
    case S8TA_WORRY:
        if(!s8_set_ability(foe,S8TA_AB_INSOMNIA)) goto fail;
        break; // Insomnia cures through its Update event.
    case S8TA_SWAP:
        if(!s8_swap_abilities(me,foe)) goto fail;
        break;
    default: goto fail;
    }
    return 1;
fail:
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
}
static void s8_type_hit(Mon *me,Mon *foe,int mv,int sub,Rng *r) {
    const S8TypeAbilityMove *row=s8_type_ability_move(mv);
    if(!row) return;
    if(mv==S8TA_M_BURNUP || mv==S8TA_M_DOUBLESHOCK) {
        int lost=mv==S8TA_M_BURNUP?T_FIRE:T_ELEC;
        int t1=s8_effective_type(me,0),t2=s8_effective_type(me,1);
        s8_set_types(me,t1==lost?-2:t1,t2==lost?-2:t2,0);
    }
    if(row->chance && !(S8_PROPERTY && S8_PROPERTY->sheer) && rng_dbl(r)<row->chance/100.0) {
        if(mv==S8TA_M_AURAWHEEL && me->hp>0) {
            const int delta[7]={0,0,0,0,1,0,0};s8_change_boost(me,me,delta);
        }
        if(mv==S8TA_M_SHELLSIDEARM && !sub && foe->hp>0) inflict_status(foe,STATUS_PSN,r);
    }
}

static int s8_turn_prepare(Mon *me,Mon *foe,int mv,Rng *r) {
    const S8TurnMove *row=s8_turn_move(mv);
    if(!row) return 0;
    switch(row->kind) {
    case S8U_CHARGE:
        if(s8_charge_release(me,mv)) { obs2_cancel_charge(me);me->charging=me->charge_move=0;return 0; }
        if(row->spa) {
            const int delta[7]={0,0,1,0,0,0,0};s8_change_boost(me,me,delta);
        }
        if((row->skip_weather==1 && current_weather(me)==WEATHER_SUN) ||
           (row->skip_weather==2 && current_weather(me)==WEATHER_RAIN)) return 0;
        me->charging=1;me->charge_move=mv+1;me->state_features|=MON_POST_V1_STATE;
        obs2_emit(EV_CHARGE_START,me,mv,0,0);attempt_result(me,ATTEMPT_NOT_FAIL);return 1;
    case S8U_FIRST: if(FIELD.turn>1) goto fail;break;
    case S8U_RESORT: {
        int knows=0;
        if(me->nmoves<2) goto fail;
        for(int i=0;i<me->nmoves;i++) {
            if(me->moves[i]==mv) knows=1;
            else if(!(me->used_mask&(1<<i))) goto fail;
        }
        if(!knows) goto fail;
        break;
    }
    case S8U_SUCKER: case S8U_UPPER: {
        int seat=foe==FIELD.seat[1],selected=FIELD.committed[seat];
        if(!field_member(foe) || FIELD.consumed[seat] || selected<0 || move_def(selected)->cat==CAT_STATUS) goto fail;
        if(row->kind==S8U_SUCKER && foe->recharge) goto fail;
        if(row->kind==S8U_UPPER && FIELD.committed_priority[seat]<=0) goto fail;
        break;
    }
    case S8U_SINGLE:
        // Quash reaches Hit in singles, so it still checks protection,
        // semi-invulnerability, accuracy and Substitute before its failure.
        if(mv==S8U_M_QUASH || mv==S8U_M_AFTERYOU) return 0;
        goto fail;
    case S8U_ALLYSWITCH:
        if(me->ally_timer && rng_dbl(r)>=1.0/pow(3.0,me->ally_streak)) {
            me->ally_timer=me->ally_streak=0;goto fail;
        }
        me->ally_timer=2;if(me->ally_streak<6) me->ally_streak++;
        attempt_result(me,ATTEMPT_NOT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
    default: break;
    }
    return 0;
fail:
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
}
static void s8_beak_contact(Mon *me,Mon *foe,int mv,int sub,Rng *r) {
    if(!foe->beak_hot || sub || !move_def(mv)->contact) return;
    Mon *saved=S8_EFFECT_SOURCE;S8_EFFECT_SOURCE=foe;
    inflict_status(me,STATUS_BRN,r);S8_EFFECT_SOURCE=saved;
}
static void s8_turn_effect(Mon *me,Mon *foe,int mv,int sub,Rng *r) {
    const S8TurnMove *row=s8_turn_move(mv);
    if((S8_PROPERTY && S8_PROPERTY->sheer) || !row || !row->chance || !s8_guard_secondary(foe,0) || rng_dbl(r)>=row->chance/100.0 || sub || foe->hp<=0) return;
    if(row->effect==1) inflict_status(foe,STATUS_PAR,r);
    if(row->effect==2 && !CUR_SECOND && !foe->focusing) s8_guard_flinch(foe);
    (void)me;
}
static int s8_turn_status(Mon *me,Mon *foe,int mv,Rng *r) {
    const S8TurnMove *row=s8_turn_move(mv);
    if(!row || move_def(mv)->cat!=CAT_STATUS) return 0;
    if(row->kind==S8U_COPY) {
        int call=FIELD.last_used-1;
        if(FIELD.last_used<0) goto fail; // Struggle has failcopycat
        if(call<0) return 1; // pinned onHit has an undefined return (no prior move)
        if(S8_MOVES[call].flags&S8_FLAG_FAILCOPYCAT) goto fail;
        S8_CALL_DEPTH++;
        obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,call},1);
        execute_move(me,foe,call,r);
        S8_CALL_DEPTH--;return 1;
    }
    if(s8_is_protected(foe,mv)) { attempt_result(me,ATTEMPT_NOT_FAIL);obs_emit(EV_HIT,me,HIT_PROTECT,0,0);return 1; }
    if(!s8_boost_accuracy(me,foe,move_def(mv),r,0)) goto fail;
    if(row->kind==S8U_INSTRUCT) {
        int call=foe->action_move-1,slot=call>=0?find_move(foe,call):-1;
        if(call<0 || (S8_MOVES[call].flags&(S8_FLAG_FAILINSTRUCT|S8_FLAG_CHARGE|S8_FLAG_RECHARGE)) ||
           foe->beak_hot || foe->focusing || (slot>=0 && foe->pp[slot]<=0)) goto fail;
        FIELD.instructed[foe==FIELD.seat[1]]=call+1;return 1;
    }
    // After You and Quash: explicit singles onHit failure, after Substitute.
    if(s8_sub_blocks(foe,mv) && !(S8_MOVES[mv].flags&S8_FLAG_BYPASSSUB)) goto fail;
fail:
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
}

// ---------- Stage 8c-11b item effects and no-reserve resolution ----------
static void s8_item_eat_effect(Mon *m,int item) {
    if(m->hp<=0) return;
    switch(S8_ITEM_OPERATIONS[item].eat) {
    case S8IE_QUARTER: s8_heal_hp(m,s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,(m->maxhp+(FIELD.item_ability_context?0:2))/4,item));break;
    case S8IE_TEN: s8_heal_hp(m,s8_ability_event(m,S8AE_ITEM_HEAL,m,m,NULL,10,item));break;
    case S8IE_SLEEP: if(m->sleep) clear_major_status(m);break;
    case S8IE_STATUS:
        clear_major_status(m);
        if(m->confusion) obs2_emit(EV_CONFUSION_END,m,0,0,0);
        m->confusion=0;break;
    case S8IE_SELECTIVE_STATUS: s8_selective_status_cure(m,s8_status_item(item));break;
    case S8IE_PP: {
        int slot=-1;
        for(int i=0;i<m->nmoves;i++) if(!m->pp[i]) { slot=i;break; }
        if(slot<0) for(int i=0;i<m->nmoves;i++) if(m->pp[i]<s8_move_max_pp(m,i)) { slot=i;break; }
        if(slot>=0) {
            int max=s8_move_max_pp(m,slot);
            int added=s8_has_ability(m,S8I_RIPEN)?20:10;
            m->pp[slot]=m->pp[slot]+added<max?m->pp[slot]+added:max;
            obs_emit(EV_LEPPA,m,m->moves[slot],0,0);
        }
        break;
    }
    default: break; // the admitted resist berries have an empty onEat
    }
}
// Observe the existing Pressure deduction before its existing Leppa check.
// The wrapper preserves the original call order, results and RNG consumption.
static int OBS3_PRESSURE_PP[4];
static void obs3_pressure_leppa(Mon *m,int slot) {
    if(OBS && OBS->version==3 && slot>=0 && slot<4)
        obs_emit(EV3_PP,m,m->moves[slot],OBS3_PRESSURE_PP[slot]-m->pp[slot],0);
    leppa_check(m,slot);
}
#define s8_items_pressure s8_items_pressure_before_obs3
#define leppa_check obs3_pressure_leppa
#include "stage8/ability_item_impl_8d4b.h"
#undef leppa_check
#undef s8_items_pressure
static void s8_items_pressure(Mon *me,Mon *foe,int mv) {
    if(OBS && OBS->version==3) for(int i=0;i<me->nmoves;i++) OBS3_PRESSURE_PP[i]=me->pp[i];
    s8_items_pressure_before_obs3(me,foe,mv);
}

static void s8_item_force_eat(Mon *m) {
    if(m->hp<=0 || !S8_ITEM_OPERATIONS[m->item].berry) return;
    int item=m->item;
    // Forced eatItem bypasses suppression/TryEatItem, preserving raw identity.
    s8_item_eat_effect(m,item);s8_item_spend(m,1);
}
static int s8_item_prepare(Mon *me,Mon *foe,int mv) {
    const S8ItemSwitchMove *row=s8_item_switch_move(mv);
    if(!row) return 0;
    if(row->kind==S8IS_POLTER && !foe->item) goto fail;
    if(row->kind==S8IS_STUFF && !S8_ITEM_OPERATIONS[me->item].berry) goto fail;
    if(row->kind==S8IS_FLING) {
        if(!held_item(me) || !s8_item_takeable(me) || !S8_ITEM_OPERATIONS[me->item].power) goto fail;
        // The volatile removes the item on Update, after damage/secondaries.
        me->fling_item=me->item;
        if(S8_ITEM_OPERATIONS[me->item].berry && s8_has_ability(me,S8I_CUDCHEW))
            s8_ability_event(me,S8AE_EAT_ITEM,me,me,move_def(mv),1,me->item);
    }
    return 0;
fail:
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
}
static void s8_fling_update(Mon *m) {
    if(!m->fling_item) return;
    s8_item_spend(m,1);m->fling_item=0;
}
static int s8_item_status(Mon *me,Mon *foe,int mv,Rng *r) {
    const S8ItemSwitchMove *row=s8_item_switch_move(mv);
    if(!row || move_def(mv)->cat!=CAT_STATUS) return 0;
    if(mv==S8IS_M_ROAR || mv==S8IS_M_WHIRLWIND) {
        if(s8_can_switch(foe)) {
            int ok=s8_drag_out(me,foe,move_def(mv));attempt_result(me,ok?ATTEMPT_SUCCESS:ATTEMPT_FAIL);return 1;
        }
    }
    switch(row->kind) {
    case S8IS_NOTFAIL: goto notfail; // canSwitch is false before any cost/volatile
    case S8IS_RECYCLE: {
        if(me->item || !me->last_item) goto fail;
        int item=me->last_item,table=me->last_item_table;
        me->last_item=0;me->last_item_table=-1;s8_item_set(me,item,table);return 1;
    }
    case S8IS_STUFF: {
        const int delta[7]={0,2,0,0,0,0,0};
        if(!s8_change_boost(me,me,delta)) goto notfail;
        s8_item_force_eat(me);return 1;
    }
    case S8IS_TEA: {
        Mon *ms[]={FIELD.seat[0],FIELD.seat[1]};int found=0;
        for(int i=0;i<2;i++) if(ms[i]->hp>0 && S8_ITEM_OPERATIONS[ms[i]->item].berry &&
           (s8_semi(ms[i])<0 || (ms[i]!=me && me->control[S8C_LOCKON]))) { found=1;s8_item_force_eat(ms[i]); }
        if(!found) goto notfail;
        return 1;
    }
    case S8IS_SNOW:
        if(!s8_set_weather(me,WEATHER_SNOW)) goto fail;
        return 1;
    case S8IS_FAIL:
        if(mv==S8IS_M_REVIVALBLESSING) goto fail;
        break;
    default: break;
    }
    if((S8_MOVES[mv].flags&S8_FLAG_PROTECT) && s8_is_protected(foe,mv)) {
        attempt_result(me,ATTEMPT_NOT_FAIL);obs_emit(EV_HIT,me,HIT_PROTECT,0,0);return 1;
    }
    if(!s8_boost_accuracy(me,foe,move_def(mv),r,0)) goto fail;
    if(s8_sub_blocks(foe,mv) && !(S8_MOVES[mv].flags&S8_FLAG_BYPASSSUB)) goto fail;
    if(row->kind==S8IS_FAIL) goto fail;
    if(row->kind==S8IS_PARTING) {
        const int delta[7]={-1,0,-1,0,0,0,0};
        if(!s8_change_boost(foe,me,delta)) goto fail;
    } else if(row->kind==S8IS_SWAP) {
        if(!s8_item_exchange(me,foe)) goto fail;
    } else if(row->kind==S8IS_GAS) {
        // The pinned onHit logs failure but returns undefined (move success).
        if(s8_item_take(foe,me,move_def(mv))) { obs_emit(EV_TRANSFER,foe,ITEM_NONE,1,0); }
    }
    return 1;
notfail:
    attempt_result(me,ATTEMPT_NOT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
fail:
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
}
static void s8_item_onhit(Mon *me,Mon *foe,int mv,int sub) {
    const S8ItemSwitchMove *row=s8_item_switch_move(mv);
    if(!row || sub) return;
    if(row->kind==S8IS_EAT && me->hp>0 && S8_ITEM_OPERATIONS[foe->item].berry && s8_item_takeable(foe)) {
        int item=s8_item_take(foe,me,move_def(mv));
        if(!item) return;
        obs_emit(EV_TRANSFER,foe,ITEM_NONE,1,0);
        s8_item_eat_effect(me,item);s8_ability_event(me,S8AE_EAT_ITEM,me,me,move_def(mv),1,item);me->ate_berry=1; // stealeat sets ateBerry, not lastItem
    }
    if(row->kind==S8IS_FLING && me->fling_item) {
        const S8ItemOperation *item=&S8_ITEM_OPERATIONS[me->fling_item];
        if(item->berry) {
            s8_item_eat_effect(foe,me->fling_item);
            s8_ability_event(foe,S8AE_EAT_ITEM,me,foe,move_def(mv),1,me->fling_item);foe->ate_berry=1;
        }
        else if(item->fling_effect==S8IE_HERB && foe->hp>0) {
            for(int i=0;i<7;i++) if(*s8_boost_stage(foe,i)<0) *s8_boost_stage(foe,i)=0;
        }
        else if(item->fling_effect==S8IE_MENTAL && foe->hp>0) s8_mental_cure(foe);
        else if(item->fling_effect==S8IE_PARALYZE && foe->hp>0) {
            Mon *saved=S8_EFFECT_SOURCE;S8_EFFECT_SOURCE=me;
            inflict_status(foe,STATUS_PAR,BATTLE_RNG);S8_EFFECT_SOURCE=saved;
        }
    }
}
static void s8_item_secondary(Mon *me,Mon *foe,int mv,int sub,Rng *r) {
    if(s8_guard_secondary(foe,0) && mv==S8IS_M_FLING && me->fling_item && S8_ITEM_OPERATIONS[me->fling_item].fling_effect==S8IE_FLINCH) {
        (void)rng_dbl(r); // secondaries() draws even with an undefined chance
        if(!sub && foe->hp>0 && !CUR_SECOND && !foe->focusing) s8_guard_flinch(foe);
    }
}
static void s8_item_afterhit(Mon *me,Mon *foe,int mv,int sub) {
    const S8ItemSwitchMove *row=s8_item_switch_move(mv);
    if(!row || sub || me->hp<=0 || !s8_item_takeable(foe)) return;
    if(row->kind==S8IS_STEAL && !me->item && !s8_stone_matches(me,s8_item_table(foe))) {
        int item=foe->item,table=s8_item_table(foe);
        if(!s8_item_take(foe,me,move_def(mv))) return;
        s8_item_set(me,item,table);obs_emit(EV_TRANSFER,foe,ITEM_NONE,1,0);
    } else if(row->kind==S8IS_KNOCK && s8_item_take(foe,foe,move_def(mv))) { obs_emit(EV_TRANSFER,foe,ITEM_NONE,1,0); }
}

// ---------- Stage 8c-11c resolution: field callbacks and special secondaries ----------
static void s8_special_clear_terrain(Mon *source) {
    if(!FIELD.terrain) return;
    FIELD.terrain=FIELD.terrain_turns=0;obs2_emit(EV_TERRAIN_END,source,0,0,0);
}
static int s8_special_clear_hazards(Mon *m) {
    int changed=m->spikes || m->toxic_spikes || m->stealth_rock || m->sticky_web || m->gmax_steelsurge;
    m->spikes=m->toxic_spikes=m->stealth_rock=m->sticky_web=m->gmax_steelsurge=0;
    return changed;
}
static void s8_special_spin(Mon *m) {
    m->leech_seed=m->partial_trap=m->partial_move=0;s8_special_clear_hazards(m);
}
static int s8_special_deduct(Mon *m,int amount) {
    int slot=find_move(m,m->last_move);
    if(m->last_move<0 || slot<0 || m->pp[slot]<=0) return 0;
    int loss=m->pp[slot]<amount?m->pp[slot]:amount;m->pp[slot]-=loss;
    if(OBS && OBS->version==3) obs_emit(EV3_PP,m,m->moves[slot],loss,0);
    return loss;
}
static int s8_special_try(Mon *me,int mv) {
    if((mv==S8X_M_BELCH && !me->ate_berry) || (mv==S8X_M_STEELROLLER && !FIELD.terrain)) {
        attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
    }
    return 0;
}
static int s8_special_status(Mon *me,Mon *foe,int mv,Rng *r) {
    if(!s8_special_move(mv) || move_def(mv)->cat!=CAT_STATUS) return 0;
    int success=0;
    if(mv==S8X_M_COURTCHANGE) {
        // Transfer the complete side condition, including remaining timer/layers.
#define SWAP_SIDE(x) do { int t=me->x;success|=!!(t || foe->x);me->x=foe->x;foe->x=t; } while(0)
        SWAP_SIDE(reflect);SWAP_SIDE(light_screen);SWAP_SIDE(aurora_veil);SWAP_SIDE(safeguard);
        SWAP_SIDE(tailwind);SWAP_SIDE(spikes);SWAP_SIDE(toxic_spikes);SWAP_SIDE(stealth_rock);SWAP_SIDE(sticky_web);
#undef SWAP_SIDE
        if(OBS && OBS->version==3) {
            obs_emit(EV3_COURT,me,0,0,0);
            // Transfer evidence with the condition; the next announcement
            // pass must not restart its public elapsed clock or 5/8 bounds.
            for(int i=0;i<5;i++) {
                int tmp=OBS->published_field[O3_SIDE+i];
                OBS->published_field[O3_SIDE+i]=OBS->published_field[O3_SIDE+7+i];
                OBS->published_field[O3_SIDE+7+i]=tmp;
            }
            for(int i=0;i<4;i++) {
                int tmp=OBS->published_hazard[0][i];OBS->published_hazard[0][i]=OBS->published_hazard[1][i];OBS->published_hazard[1][i]=tmp;
            }
        }
    } else if(mv==S8X_M_TIDYUP) {
        success=!!(me->sub_hp || foe->sub_hp);
        if(me->sub_hp) obs_emit(EV_SUB,me,0,0,0);
        if(foe->sub_hp) obs_emit(EV_SUB,foe,0,0,0);
        me->sub_hp=foe->sub_hp=0;
        success=s8_special_clear_hazards(me) || success;
        success=s8_special_clear_hazards(foe) || success;
        const int delta[7]={1,0,0,0,1,0,0};success=s8_change_boost(me,me,delta) || success;
    } else {
        if(s8_is_protected(foe,mv)) { attempt_result(me,ATTEMPT_NOT_FAIL);obs_emit(EV_HIT,me,HIT_PROTECT,0,0);return 1; }
        if(!s8_boost_accuracy(me,foe,move_def(mv),r,0)) goto fail;
        if(mv==S8X_M_SPITE) success=s8_special_deduct(foe,4)>0;
        else if(mv==S8X_M_DEFOG) {
            // bypasssub allows removal; its explicit onHit check still blocks the drop.
            const int delta[7]={0,0,0,0,0,0,-1};
            if(!foe->sub_hp) success=s8_change_boost(foe,me,delta);
            if(foe->reflect) obs2_emit(EV_SCREEN_END,foe,0,0,0);
            if(foe->light_screen) obs2_emit(EV_SCREEN_END,foe,1,0,0);
            foe->reflect=foe->light_screen=foe->aurora_veil=foe->safeguard=0;
            success=s8_special_clear_hazards(foe) || success;
            success=s8_special_clear_hazards(me) || success;
            s8_special_clear_terrain(me); // deliberately not part of callback success
        }
    }
    if(success) return 1;
fail:
    attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return 1;
}
static void s8_special_onhit(Mon *me,Mon *foe,int mv,int sub) {
    (void)foe;
    if(mv==S8X_M_STEELROLLER || (sub && mv==S8X_M_ICESPINNER && me->hp>0)) s8_special_clear_terrain(me);
    if(sub && (mv==S8X_M_MORTALSPIN || s8_rapidspin_move(mv)) && me->hp>0 && !(S8_PROPERTY && S8_PROPERTY->sheer)) s8_special_spin(me);
}
static void s8_special_secondary(Mon *me,Mon *foe,int mv,int sub,Rng *r) {
    if(s8_rapidspin_move(mv)) {
        const S8StatEffect speed={mv,S8_STAT_SECONDARY,100,1,{0,0,0,0,1,0}};
        s8_apply_stat_effect(me,foe,&speed,!sub,r);return;
    }
    const S8SpecialMove *row=s8_special_move(mv);
    if(S8_PROPERTY && S8_PROPERTY->parental && S8_PROPERTY->hit<2 && !strcmp(S8_MOVES[mv].id,"secretpower")) return;
    if((S8_PROPERTY && S8_PROPERTY->sheer) || !row || !row->chance || !s8_guard_secondary(foe,0) || (int)(rng_dbl(r)*100)>=row->chance || sub || foe->hp<=0) return;
    int status=0;
    switch(row->effect) {
    case S8X_CONFUSE_RAISED: if(foe->stats_raised_turn) inflict_confusion(foe,r);break;
    case S8X_BURN_RAISED: if(foe->stats_raised_turn) inflict_status(foe,STATUS_BRN,r);break;
    case S8X_RANDOM_DIRE: {
        static const int choices[]={STATUS_PSN,STATUS_PAR,0};status=choices[(int)(rng_dbl(r)*3)];
        if(!status) { inflict_sleep(foe,r);return; }break;
    }
    case S8X_RANDOM_TRI: {
        static const int choices[]={STATUS_BRN,STATUS_PAR,STATUS_FRZ};status=choices[(int)(rng_dbl(r)*3)];break;
    }
    case S8X_PP: s8_special_deduct(foe,3);break;
    case S8X_CONFUSE: inflict_confusion(foe,r);break;
    case S8X_PSN: status=STATUS_PSN;break;
    case S8X_PAR: status=STATUS_PAR;break;
    case S8X_FRZ: status=STATUS_FRZ;break;
    default: break; // Sparkling Aria volatile is resolved in AfterMove below.
    }
    if(status) inflict_status(foe,status,r);
}
static void s8_special_afterhit(Mon *me,Mon *foe,int mv,int sub) {
    (void)foe;
    if(sub || me->hp<=0) return;
    if(mv==S8X_M_ICESPINNER) s8_special_clear_terrain(me);
    if((mv==S8X_M_MORTALSPIN || s8_rapidspin_move(mv)) && !(S8_PROPERTY && S8_PROPERTY->sheer)) s8_special_spin(me);
}
static void s8_special_after(Mon *foe,int mv) {
    if(!HIT_LANDED || (S8_PROPERTY && S8_PROPERTY->sheer)) return;
    // AfterMove precedes faint-queue resolution, even after Life Orb reaches 0 HP.
    if(mv==S8X_M_SPARKLINGARIA && s8_guard_secondary(foe,0) && foe->hp>0 && foe->status==STATUS_BRN) clear_major_status(foe);
}

// TryPrimaryHit precedes Substitute and damage, after accuracy/immunity.
static void s8_normal_gem_hit(Mon *source,Mon *target,const MoveDef *move,int pledge_combo) {
    if(source==target || move->cat==CAT_STATUS || pledge_combo ||
       move->type!=T_NORMAL || source->item_used || held_item(source)!=ITEM_NORMALGEM) return;
    if(field_member(source)) s8_item_enable(FIELD.seat[0],FIELD.seat[1]);
    s8_item_spend(source,1);source->gem=1;source->state_features|=MON_POST_V1_STATE;
}
// The caller has checked protection, immunity and the first accuracy roll.
// Keep a mutable plan seam for 8d Skill Link / item Loaded Dice; neither is
// admitted here. No recursive execute_move: PP/use/accuracy are move-level.
static void s8_execute_multihit(Mon *me, Mon *foe, const S8Multihit *definition, Rng *r) {
    S8Multihit plan=*definition;
    int mv=plan.move, count=plan.max_hits, total=0, body_total=0, last_body=0, final_damage=0, completed=0;
    const MoveDef *m=move_def(mv);
    if(mv==S8R_M_UPROAR) {
        // Move-local TryHit follows protection, type immunity and accuracy.
        Mon *ms[]={me,foe};
        for(int i=0;i<2;i++) if(ms[i]->sleep) { ms[i]->sleep=0;obs_emit(EV_SLEEP,ms[i],0,0,0); }
    }
    if(s8_has_ability(me,S8SC_BATTLEBOND) && !me->transformed &&
       !strcmp(s8_switch_species(me),"greninjaash") && mv==S8V_WATERSHURIKEN)
        plan.min_hits=plan.max_hits=count=3;
    if(s8_has_ability(me,S8D_SKILLLINK)) plan.min_hits=plan.max_hits;
    if(plan.min_hits!=plan.max_hits) {
        // champions gen=9 samples 20 equal slots, weights 7/7/3/3.
        // PRNG.sample -> random(length) -> floor(uniform*length).
        int slot=(int)(rng_dbl(r)*(sizeof(S8_MULTIHIT_SAMPLE)/sizeof(*S8_MULTIHIT_SAMPLE)));
        count=S8_MULTIHIT_SAMPLE[slot];
    }
    for(int hit=1;hit<=count && (me->hp>0 || mv==S8V_MISTYEXPLOSION) && foe->hp>0;hit++) {
        if(S8_PROPERTY) S8_PROPERTY->hit=hit;
        if(hit>1 && plan.multiaccuracy && !s8_has_ability(me,S8D_SKILLLINK) && !s8_multihit_accuracy(me,foe,m,r,0)) break;
        s8_ability_phase(me,foe,S8AE_BEFORE_HIT,mv,hit);
        s8_normal_gem_hit(me,foe,m,S8_PROPERTY && S8_PROPERTY->pledge_combo);
        int sub=s8_sub_blocks(foe,mv) && !s8_property_sound(mv);
        int fixed=s8_variable_supported(mv)?s8_fixed_damage(me,foe,mv):-1;
        int crit=0, dmg=fixed;
        if(fixed<0) crit=rng_dbl(r)<damage_crit_probability(me,foe,mv);
        if(OBS && OBS->version==3) obs_emit(EV3_HIT_BEGIN,me,hit,0,0);
        if(crit) obs_emit(EV_HIT,me,HIT_CRIT,0,0);
        double eff=damage_effectiveness(foe,mv);
        obs_emit(EV_HIT,me,HIT_EFFECT,eff<1?0:eff==1?1:2,0);
        // Resist berries protect the body, not a Substitute (legacy eligibility
        // remains frozen in calc_move_damage).
        int berry=fixed<0 && !sub && damage_berry(foe,m,eff);
        if(berry) { berry=s8_items_try_eat(foe);if(berry) s8_item_spend(foe,1); }
        if(fixed<0) {
            S8_HIT_INDEX=hit;
            S8_POWER_PROC=mv==S8V_FICKLEBEAM && (int)(rng_dbl(r)*10)<3;
            int roll=85+rng_int(r,16);
            LIVE_DAMAGE=1;
            S8_ITEM_DAMAGE_EXEC=1;
            dmg=damage_value(me,foe,mv,crit,roll,&DAMAGE_NEUTRAL,berry);
            S8_ITEM_DAMAGE_EXEC=0;
            LIVE_DAMAGE=0;
        }
        S8_POWER_PROC=0; S8_HIT_INDEX=1;
        HIT_LANDED=1;
        int dealt;
        if(sub) {
            dealt=dmg<foe->sub_hp?dmg:foe->sub_hp;
            foe->sub_hp-=dealt;
            obs_emit(EV_HIT,me,HIT_SUB,foe->sub_hp>0,0);
            if(!foe->sub_hp) obs_emit(EV_SUB,foe,0,0,0);
        } else {
            int before=foe->hp;
            s8_hit_before(me,foe,m,definition->max_hits>1);
            dmg=s8_ability_event(foe,S8AE_HP_DAMAGE,me,foe,m,dmg,0);
            foe->hp=dmg<before?before-dmg:0;
            if(!foe->hp) {
                if(foe->endured_now) foe->hp=1;
                else if(held_item(foe)==ITEM_SASH && !foe->item_used && before>=foe->maxhp) {
                    foe->hp=1; s8_item_spend(foe,1);
                } else if(held_item(foe)==ITEM_FBAND && rng_dbl(r)<0.10) { foe->hp=1; obs_item(foe,0); }
            }
            dealt=before-foe->hp;
            body_total+=dealt;
            if(foe->focusing) foe->focus_lost=1;
            queue_faint_from(foe,FAINT_DAMAGE,me,m); obs_hp(foe,before,HP_HIT);
        }
        s8_ability_phase(me,foe,S8AE_AFTER_HIT,mv,hit);
        total+=dealt; completed=hit; last_body=!sub; final_damage=dealt;
        const S8TrapMove *trap=s8_trap_move(mv);
        s8_beak_contact(me,foe,mv,sub,r);
        s8_special_onhit(me,foe,mv,sub);
        s8_item_onhit(me,foe,mv,sub);
        s8_type_hit(me,foe,mv,sub,r);
        s8_turn_effect(me,foe,mv,sub,r);
        s8_trap_hit(me,foe,trap,sub,r);
        s8_control_hit(me,foe,s8_control_move(mv),sub,r);
        s8_rampage_hit(me,foe,s8_rampage_move(mv),sub,r);
        if((trap || s8_control_move(mv) || s8_rampage_move(mv) || s8_protect_move(mv) || s8_turn_move(mv) || s8_type_ability_move(mv) || s8_item_switch_move(mv)) && !sub) status_damaging_hit(foe,m->type,m->cat,0);
        const S8HealMove *heal=s8_heal_move(mv);
        if(heal) {
            // Battle.spreadDamage rounds body drain; Substitute uses ceil.
            int amount=((int64_t)dealt*heal->num+(sub?heal->den-1:heal->den/2))/heal->den;
            s8_property_drain(me,foe,amount,0);
            s8_apply_status_effects(me,foe,&heal->status,!sub,r);
            if(!sub) status_damaging_hit(foe,m->type,m->cat,0);
        }
        if(mv==S8B_CLEARSMOG && !sub) {
            for(int i=0;i<7;i++) *s8_boost_stage(foe,i)=0;
        }
        if(!sub) s8_hit_onhit(me,foe,m,crit);
        if(s8_variable_supported(mv) || (S8_PROPERTY && S8_PROPERTY->parental)) apply_move_effect(me,foe,mv,!sub,r);
        s8_special_secondary(me,foe,mv,sub,r);
        if(mv==MV_PN && !sub && foe->hp>0 && !(S8_PROPERTY && S8_PROPERTY->sheer) && s8_guard_secondary(foe,0) && s8_guard_volatile(foe,S8G_HEALBLOCK)) { foe->healblock=2;obs_emit(EV_BLOCK,foe,1,0,0); }
        s8_property_stench(me,foe,mv,!sub,r);
        // King's Rock is a secondary on every hit, even into Substitute/KO;
        // eligibility is tested after the draw, just like secondaries().
        if(held_item(me)==ITEM_KINGS && s8_guard_secondary(foe,0) && !s8_property_flinch(mv) && rng_dbl(r)<0.10 && !sub && foe->hp>0 && !CUR_SECOND && !foe->focusing)
            s8_guard_flinch(foe);
        s8_item_secondary(me,foe,mv,sub,r);
        if(!sub) {
            if(s8_special_move(mv) || s8_rapidspin_move(mv)) status_damaging_hit(foe,m->type,m->cat,0);
            if((foe->hp>0 || FIELD.item_context) && held_item(foe)==ITEM_BALLOON && !foe->item_used) { s8_item_spend(foe,0); }
            // DamagingHit runs per body hit, including the target's KO hit.
            s8_damaging_hit(foe,m->cat,dealt);
            s8_hit_damaging(me,foe,m,dealt);
            if(m->contact && held_item(foe)==ITEM_HELMET && me->hp>0 && s8_resist_loss(me,1,S8R_OTHER)) {
                int before=me->hp, loss=me->maxhp/6;
                if(loss<1) loss=1;
                me->hp=loss<before?before-loss:0;
                queue_faint(me,FAINT_RECOIL); obs_item(foe,0); obs_hp(me,before,HP_RECOIL);
                s8_ability_event(me,S8AE_EMERGENCY_EXIT,me,me,NULL,before,0);
            }
        }
        s8_special_afterhit(me,foe,mv,sub);
        s8_item_afterhit(me,foe,mv,sub);
        if(FIELD.item_context && sub && held_item(foe)==ITEM_BALLOON) s8_item_spend(foe,0);
        s8_fling_update(me);
        if(trap && !sub && trap->kind==S8T_EDGE && me->hp>0 && !(S8_PROPERTY && S8_PROPERTY->sheer)) s8_add_hazard(foe,trap);
        // Each hit's Update can heal before the following hit.
        s8_ability_phase(me,foe,S8AE_UPDATE,mv,hit);
        berry_hp_check(foe); berry_hp_check(me);
#ifdef ENGINE_MULTIHIT_SINK
        ENGINE_MULTIHIT_SINK(hit,dmg,dealt,sub,crit);
#endif
    }
    if(last_body) s8_got_attacked(foe,final_damage,completed);
    LAST_DIRECT_DAMAGE=total;
    const S8HealMove *heal=s8_heal_move(mv);
    if(last_body && heal && heal->status.thaws_target && foe->hp>0 && foe->status==STATUS_FRZ)
        clear_major_status(foe);
    s8_hit_finish(me,foe,m);
    if(HIT_LANDED) {
        const S8StatEffect boost={mv,S8_STAT_SELFBOOST,100,1,
            {plan.self_delta[0],plan.self_delta[1],plan.self_delta[2],plan.self_delta[3],plan.self_delta[4],plan.self_delta[5]}};
        int has_boost=0;
        for(int i=0;i<6;i++) if(plan.self_delta[i]) has_boost=1;
        if(has_boost) s8_apply_stat_effect(me,foe,&boost,1,r);
        if(S8_PROPERTY && S8_PROPERTY->parental) s8_apply_self_boost(me,foe,mv,r);
        // The move's AfterMoveSecondarySelf precedes the held-item event.
        if(mv==S8X_M_FELLSTINGER && me->hp>0 && foe->hp<=0) {
            const int delta[7]={3,0,0,0,0,0,0};s8_change_boost(me,me,delta);
        }
        s8_ability_event(me,S8AE_AFTER_SECONDARY_SELF,me,foe,m,1,1);
        // Life Orb / Shell Bell run once after the loop and selfBoost.
        // Substitute returns a nonnumeric result to hitStepMoveHitLoop, so
        // its damage is excluded from move.totalDamage for Shell Bell.
        if(me->hp>0 && held_item(me)==ITEM_LIFEORB && s8_resist_loss(me,1,S8R_OTHER)) {
            int before=me->hp, loss=me->maxhp/10;
            if(loss<1) loss=1;
            me->hp=loss<before?before-loss:0;
            obs_item(me,0); obs_hp(me,before,HP_RECOIL);
        } else if(held_item(me)==ITEM_SHELLBELL) post_hit_attacker(me,body_total);
        berry_hp_check(me);
    }
}
// All defense moves share the existing Protect/Endure streak and draw.
// Side guards increment it without invoking StallMove or drawing success RNG.
static void s8_execute_protect(Mon *me,int mv,Rng *r) {
    const S8ProtectMove *row=s8_protect_move(mv);
    int side=row && (row->kind==S8P_QUICKGUARD || row->kind==S8P_WIDEGUARD);
    if((row || mv==MV_END) && CUR_SECOND) {
        attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return;
    }
    int streak=me->protect_streak;
    if((row || mv==MV_END || FIELD.guard_context) && streak>6) streak=6;
    if(side || (row && !streak) || rng_dbl(r)<pow(1.0/3.0,streak)) {
        if(mv==MV_END) { me->endured_now=1;me->state_features|=MON_POST_V1_STATE; }
        else if(side) {
            int *guard=row->kind==S8P_QUICKGUARD?&me->quick_guard:&me->wide_guard;
            if(*guard) { attempt_result(me,ATTEMPT_FAIL);return; }
            *guard=1;
        } else { me->protected_now=1;me->shield_now=row?row->kind:0; }
        me->protect_streak++;
        if(FIELD.guard_context) me->guard_stall=2;
        obs_emit(EV_PROTECT,me,1,0,0);
    } else {
        me->protect_streak=me->guard_stall=0;
        attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_PROTECT,me,0,0,0);
    }
}
static int s8_guard_hit(Mon *me,Mon *foe,int mv,Rng *r) {
    if(CUR_FUTURE_IMPACT || !(S8_MOVES[mv].flags&S8_FLAG_PROTECT)) return 0;
    const char *target=S8_MOVES[mv].target;
    int spread=!strcmp(target,"allAdjacent") || !strcmp(target,"allAdjacentFoes");
    if(move_def(mv)->cat==CAT_STATUS && strcmp(target,"normal") && !spread) return 0;
    int side=(foe->quick_guard && (S8_PROPERTY?S8_PROPERTY->priority:(!S8_CALL_DEPTH?s8_move_priority(me,mv):0))>0) || (foe->wide_guard && spread);
    if(!side && !(foe->shield_now && s8_is_protected(foe,mv))) return 0;
    if(side && !s8_ability_event(me,S8AE_HIT_PROTECT,me,foe,move_def(mv),1,0)) return 0;
    // Protect-like TryHit deletes a fresh lockedmove without running End.
    if(me->rampage_duration==2) me->rampage_duration=me->rampage_true=me->rampage_move=0;
    if(!side && move_def(mv)->contact) {
        if(foe->shield_now==S8P_BANEFULBUNKER) {
            Mon *source=S8_EFFECT_SOURCE;S8_EFFECT_SOURCE=foe;
            inflict_status(me,STATUS_PSN,r);S8_EFFECT_SOURCE=source;
        } else if(foe->shield_now==S8P_KINGSSHIELD) {
            const S8StatEffect drop={mv,S8_STAT_SELFBOOST,100,0,{-1,0,0,0,0,0}};
            s8_apply_stat_effect(foe,me,&drop,1,r);
        } else if(foe->shield_now==S8P_SPIKYSHIELD && s8_resist_loss(me,1,S8R_OTHER)) {
            int before=me->hp,loss=me->maxhp/8;if(loss<1) loss=1;
            me->hp=loss<before?before-loss:0;
            queue_faint(me,FAINT_RECOIL);obs_hp(me,before,HP_RECOIL);berry_hp_check(me);
        }
    }
    attempt_result(me,ATTEMPT_NOT_FAIL);obs_emit(EV_HIT,me,HIT_PROTECT,0,0);return 1;
}
static void s8_break_protect(Mon *foe,int mv) {
    if(mv!=S8P_M_FEINT && mv!=S8U_M_PHANTOMFORCE) return;
    if(foe->protected_now || foe->quick_guard || foe->wide_guard) {
        foe->protected_now=foe->shield_now=foe->quick_guard=foe->wide_guard=0;
        foe->protect_streak=foe->guard_stall=0;
    }
}
static int s8_boost_bounce(Mon *me,Mon *target,int mv,Rng *r,S8AbilityEventId event) {
    if(!s8_has_ability(target,S8Q_MAGICBOUNCE) ||
       s8_ability_event(target,event,me,target,move_def(mv),1,1)!=-1) return 0;
    int saved=S8_BOUNCED;S8_BOUNCED=1;
    s8_ability_record(target,event,S8A_PUBLIC);
    // useMove consumes no PP, and the reflected active move cannot bounce again.
    S8_CALL_DEPTH++;execute_move(target,me,mv,r);S8_CALL_DEPTH--;
    S8_BOUNCED=saved;attempt_result(me,ATTEMPT_NOT_FAIL);return 1;
}
static int s8_boost_try_hit(Mon *me,Mon *target,int mv,Rng *r) {
    return s8_boost_bounce(me,target,mv,r,S8AE_TRY_HIT);
}
static int s8_boost_try_side(Mon *me,Mon *target,int mv,Rng *r) {
    return s8_boost_bounce(me,target,mv,r,S8AE_TRY_HIT_SIDE);
}

static void execute_move_core(Mon *me, Mon *foe, int mv, Rng *r) {
    const MoveDef *M = move_def(mv);
    attempt_result(me,ATTEMPT_SUCCESS); LAST_DIRECT_DAMAGE=0;
    if(mv==MV_RND && field_member(me)) {
        int other=foe==FIELD.seat[1];
        if(FIELD.committed[other]==MV_RND && !FIELD.acted[other]) {
            foe->round_chained=1; foe->state_features|=MON_POST_V1_STATE;
        }
    }
    const S8HealMove *heal_move=s8_heal_move(mv);
    const S8StatefulHeal *stateful=s8_stateful_heal(mv);
    if(((heal_move && heal_move->heal_flag) || (stateful && stateful->heal_flag) ||
       mv==S8IS_M_HEALINGWISH || mv==S8IS_M_REVIVALBLESSING) && me->healblock) {
        attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);return;
    }
    const S8StatusMove *status_move=s8_resolved_status_move(mv);
    status_modify_move(me,(status_move && status_move->defrost) || mv==S8TA_M_BURNUP);
    obs_emit_action(EV_EXEC,me,(MoveAction){mv==S8D_STRUGGLE?ACTION_STRUGGLE:ACTION_MOVE,mv},0);
    if(mv!=MV_PRO && mv!=MV_END && !s8_stalling_move(mv)) me->protect_streak=me->guard_stall=0;
    if(s8_special_try(me,mv)) return;
    if(s8_item_prepare(me,foe,mv)) return;
    if(s8_type_try(me,mv)) return;
    if(s8_turn_prepare(me,foe,mv,r)) return;
    s8_ability_event(me,S8AE_PREPARE_HIT,me,foe,M,1,0);
    s8_ability_phase(me,foe,S8AE_TARGET,mv,0);
    foe=s8_resist_select_target(me,foe,M);
    if(S8_PROPERTY) S8_PROPERTY->target=foe;
    s8_ability_phase(me,foe,S8AE_ACCURACY,mv,0);
    if(s8_semi_miss(me,foe,mv)) { attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_MISS,0,0);return; }
    if(s8_guard_hit(me,foe,mv,r)) return;
    const char *ability_target=S8_MOVES[mv].target;
    int pokemon_target=strcmp(ability_target,"all") && strcmp(ability_target,"foeSide") &&
        strcmp(ability_target,"allySide") && strcmp(ability_target,"allyTeam");
    const S8BoostMove *ability_boost=s8_boost_move(mv);
    if(!strcmp(ability_target,"adjacentAlly")) pokemon_target=0;
    Mon *recipient=(!strcmp(ability_target,"self") || !strcmp(ability_target,"allies") ||
        !strcmp(ability_target,"adjacentAllyOrSelf") ||
        (M->cat==CAT_STATUS && ability_boost && ability_boost->target==0))?me:foe;
    if(pokemon_target && !(recipient!=me && s8_is_protected(recipient,mv) &&
       (S8_MOVES[mv].flags&S8_FLAG_PROTECT)) &&
       s8_boost_try_hit(me,recipient,mv,r)) return;
    if(pokemon_target && !(recipient!=me && s8_is_protected(recipient,mv) &&
       (S8_MOVES[mv].flags&S8_FLAG_PROTECT)) &&
       !s8_ability_event(recipient,S8AE_TRY_HIT,me,recipient,M,1,0)) {
        attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_IMMUNE,0,0);return;
    }
    if(pokemon_target && s8_prankster_immune(me,recipient)) {
        attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_IMMUNE,0,0);return;
    }
    if(!pokemon_target && strcmp(ability_target,"all")) {
        Mon *side=!strcmp(ability_target,"foeSide")?foe:me;
        if(s8_boost_try_side(me,side,mv,r)) return;
        s8_ability_event(side,S8AE_TRY_HIT_SIDE,me,side,M,1,0);
    }
    if(mv==S8SC_TRANSFORM) {
        int ok=s8_transform_into(me,foe);
        attempt_result(me,ok?ATTEMPT_SUCCESS:ATTEMPT_FAIL);
        if(!ok) obs_emit(EV_HIT,me,HIT_FAIL,0,0);
        return;
    }
    if(s8_special_status(me,foe,mv,r)) return;
    if(s8_item_status(me,foe,mv,r)) return;
    if(s8_type_status(me,foe,mv,r)) return;
    if(s8_turn_status(me,foe,mv,r)) return;
    if(s8_stalling_move(mv)) { s8_execute_protect(me,mv,r);return; }
    if(mv==MV_FS && !CUR_FUTURE_IMPACT) {
        if(foe->future_turns) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); return; }
        foe->future_turns=3; foe->state_features|=MON_POST_V1_STATE;
        obs2_emit(EV_FUTURE_SET,foe,MV_FS,0,0);
        attempt_result(me,ATTEMPT_NOT_FAIL); return;
    }
    if(mv==MV_ROLL && (!field_member(me) || !FIELD.terrain)) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); return; }
    if(mv==MV_MB) {
        if(me->charging) { obs2_cancel_charge(me); me->charging=0; }
        else if(s8_boost_modern(me,foe)) {
            const int delta[7]={0,0,1,0,0,0,0};s8_change_boost(me,me,delta);
            me->charging=1;me->state_features|=MON_POST_V1_STATE;
            obs2_emit(EV_CHARGE_START,me,MV_MB,0,0);attempt_result(me,ATTEMPT_NOT_FAIL);return;
        } else { me->charging=1; if(me->st[2]<6) me->stats_raised_turn=1; me->st[2]=clamp_stage(me->st[2]+1); me->state_features|=MON_POST_V1_STATE;
            obs_emit(EV_STAGE,me,2,1,0); obs2_emit(EV_CHARGE_START,me,MV_MB,0,0); attempt_result(me,ATTEMPT_NOT_FAIL); return; }
    }
    const S8TrapMove *trap=s8_trap_move(mv);
    if(trap && M->cat==CAT_STATUS && (trap->kind!=S8T_GHOST_CURSE || has_type(me,T_GHOST))) {
        s8_execute_trap_status(me,foe,trap,r);return;
    }
    if(s8_control_move(mv) && M->cat==CAT_STATUS) { s8_execute_control(me,foe,s8_control_move(mv),r); return; }
    if(s8_field_move(mv)) { s8_execute_field(me,foe,s8_field_move(mv)); return; }
    if(stateful) { s8_execute_stateful_heal(me,stateful); return; }
    if(s8_primary_status(mv)) { s8_execute_primary_status(me,foe,mv,r); return; }
    if(heal_move && heal_move->kind!=S8H_DRAIN) { s8_execute_heal(me,foe,heal_move,r);return; }
    if(s8_boost_move(mv) && M->cat==CAT_STATUS) { s8_execute_boost(me,foe,mv,r); return; }
    if(mv==S8B_SPITUP && !me->stockpile) { attempt_result(me,ATTEMPT_FAIL); return; }
    switch (mv) {
    case MV_SUN: case MV_RAIN: case MV_SAND: {
        if(!field_member(me)) field_bind(me,foe);
        int weather=mv==MV_SUN?WEATHER_SUN:mv==MV_RAIN?WEATHER_RAIN:WEATHER_SAND;
        if(!s8_set_weather(me,weather)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        me->state_features|=MON_POST_V1_STATE; foe->state_features|=MON_POST_V1_STATE;
        return;
    }
    case MV_GRAV:
        if(!field_member(me)) field_bind(me,foe);
        if(FIELD.gravity) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        FIELD.gravity=5; obs2_emit(EV_GRAVITY_START,me,0,0,0);
        if(me->magnet_rise) obs2_emit(EV_RISE_END,me,0,0,0);
        if(foe->magnet_rise) obs2_emit(EV_RISE_END,foe,0,0,0);
        s8_cancel_air_charge(me);s8_cancel_air_charge(foe);
        me->magnet_rise=foe->magnet_rise=0;
        me->state_features|=MON_POST_V1_STATE; foe->state_features|=MON_POST_V1_STATE;
        return;
    case MV_MR:
        if(current_gravity(me) || me->magnet_rise) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        me->magnet_rise=5; me->state_features|=MON_POST_V1_STATE; obs2_emit(EV_RISE_START,me,0,0,0); return;
    case MV_REF: case MV_LS: case MV_SR: {
        Mon *target=mv==MV_SR?foe:me;
        int *timer=mv==MV_REF?&target->reflect:mv==MV_LS?&target->light_screen:&target->stealth_rock;
        if(*timer) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        *timer=mv==MV_SR?1:s8_duration(me,S8D_SCREEN,0); target->state_features|=MON_POST_V1_STATE;
        obs2_emit(mv==MV_SR?EV_ROCKS:EV_SCREEN_START,target,mv==MV_LS,0,0);
        // Stealth Rock switch-in: floor(maxHP * 2^clamp(typeMod,-6,6) / 8),
        // minimum 1 via damage(). No switching exists; no switch hook is installed.
        return;
    }
    // self-target status
    case MV_AGI: if(s8_boost_modern(me,foe)) { const int delta[7]={0,0,0,0,2,0,0};if(!s8_change_boost(me,me,delta)) attempt_result(me,ATTEMPT_FAIL);return; } if(me->st[4]<6) me->stats_raised_turn=1; if(me->st[4]==6) attempt_result(me,ATTEMPT_FAIL); me->st[4] = clamp_stage(me->st[4] + 2); obs_emit(EV_STAGE,me,4,2,0); return;
    case MV_CP: if(s8_boost_modern(me,foe)) { const int delta[7]={0,1,0,1,0,0,0};if(!s8_change_boost(me,me,delta)) attempt_result(me,ATTEMPT_FAIL);return; } if(me->st[1]<6 || me->st[3]<6) me->stats_raised_turn=1; if(me->st[1]==6 && me->st[3]==6) attempt_result(me,ATTEMPT_FAIL); me->st[1] = clamp_stage(me->st[1] + 1); me->st[3] = clamp_stage(me->st[3] + 1); obs_emit(EV_STAGE,me,1,1,0); obs_emit(EV_STAGE,me,3,1,0); return;
    case MV_ID: if(s8_boost_modern(me,foe)) { const int delta[7]={0,2,0,0,0,0,0};if(!s8_change_boost(me,me,delta)) attempt_result(me,ATTEMPT_FAIL);return; } if(me->st[1]<6) me->stats_raised_turn=1; if(me->st[1]==6) attempt_result(me,ATTEMPT_FAIL); me->st[1] = clamp_stage(me->st[1] + 2); obs_emit(EV_STAGE,me,1,2,0); return;
    case MV_REST:
        if (me->healblock > 0) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }                    // fails under heal block
        if (me->hp >= me->maxhp) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }                  // fails at full HP
        if(s8_status_prevented(me,1,0)) { attempt_result(me,ATTEMPT_FAIL); return; }
        if(s8_has_ability(me,S8G_INSOMNIA) || s8_has_ability(me,S8G_VITALSPIRIT)) {
            s8_ability_record(me,S8AE_SET_STATUS,S8A_PUBLIC);attempt_result(me,ATTEMPT_FAIL);return;
        }
        if(!s8_ability_event(me,S8AE_SET_STATUS,me,me,M,1,0)) { attempt_result(me,ATTEMPT_FAIL);return; }
        clear_major_status(me); // Rest replaces the major status, not confusion
        { int before=me->hp; me->hp = me->maxhp; obs_hp(me,before,HP_HEAL); } me->sleep = 2; obs_emit(EV_SLEEP,me,1,0,0);
        // Chesto / Lum Berry cure the Rest sleep immediately.
        if ((held_item(me) == ITEM_CHESTO || held_item(me) == ITEM_LUM) && !me->item_used && s8_items_try_eat(me)) { me->sleep = 0; s8_item_spend(me,1); obs_emit(EV_SLEEP,me,0,0,0); }
        return;
    case MV_PRO: case MV_END: s8_execute_protect(me,mv,r);return;
    case MV_SUB: {
        if (me->sub_hp > 0) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        int cost = me->maxhp / 4;
        if (me->hp <= cost) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        int before=me->hp; me->hp -= cost; me->sub_hp = cost; me->partial_trap=me->partial_move=0; obs_hp(me,before,HP_COST); obs_emit(EV_SUB,me,cost>0,0,0); berry_hp_check(me); return; }
    // foe-target status
    case MV_PU:
        if (s8_is_protected(foe,mv) && !CUR_FUTURE_IMPACT) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
        for (int i = 0; i < 5; i++) me->st[i] = foe->st[i];
        me->acc = foe->acc;
        if(me->evasion || foe->evasion) { me->evasion=foe->evasion; me->state_features|=MON_POST_V1_STATE; }
        obs_emit(EV_COPY,me,0,0,0); white_herb_check(me); return;
    case MV_SWG: {
        if(s8_is_protected(foe,mv)) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
        if(!accuracy_check(me,foe,M,r)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_MISS,0,0)); return; }
        if(s8_sub_blocks(foe,mv)) { obs_emit(EV_HIT,me,HIT_SUB,0,0); return; }
        // spreadMoveHit: boosts first, volatile second, combine the results.
        int before=foe->st[0];
        if(s8_boost_modern(me,foe)) { const int delta[7]={2,0,0,0,0,0,0};s8_change_boost(foe,me,delta); }
        else {
            foe->st[0]=clamp_stage(before+2);
            if(foe->st[0]>before) foe->stats_raised_turn=1;
            if(foe->st[0]!=before) obs_emit(EV_STAGE,foe,0,foe->st[0]-before,0);
        }
        int confused=inflict_confusion(foe,r);
        s8_ability_event(foe,S8AE_HIT,me,foe,M,1,0);
        if(before==foe->st[0] && !confused) (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0));
        return;
    }
    case MV_SF:
        if (s8_is_protected(foe,mv) && !CUR_FUTURE_IMPACT) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
        if (s8_sub_blocks(foe,mv)) { obs_emit(EV_HIT,me,HIT_SUB,0,0); return; }
        if (!accuracy_check(me, foe, M, r)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_MISS,0,0)); return; }
        if(s8_boost_modern(me,foe)) { const int delta[7]={0,0,0,0,-2,0,0};if(!s8_change_boost(foe,me,delta)) attempt_result(me,ATTEMPT_FAIL);return; }
        if (!s8_ability_boost(foe,me,-2)) { s8_ability_record(foe,S8AE_TRY_BOOST,S8A_PUBLIC); attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_ABILITY,foe,AB_CB,0,0); return; }                 // Clear Body blocks foe-caused drops
        if(foe->st[4]==-6) attempt_result(me,ATTEMPT_FAIL);
        if(foe->st[4]>-6) foe->stats_lowered_turn=1;
        foe->st[4] = clamp_stage(foe->st[4] - 2); obs_emit(EV_STAGE,foe,4,-2,0); white_herb_check(foe); return;
    case MV_TRK:
        if (s8_is_protected(foe,mv) && !CUR_FUTURE_IMPACT) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
        if (s8_sub_blocks(foe,mv)) { obs_emit(EV_HIT,me,HIT_SUB,0,0); return; }
        if (!accuracy_check(me, foe, M, r)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_MISS,0,0)); return; }
        if (trick_stone_blocked(me, foe)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        if (me->item == ITEM_NONE && foe->item == ITEM_NONE) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        if(FIELD.item_context) {
            if(!s8_item_exchange(me,foe)) attempt_result(me,ATTEMPT_FAIL);
            return;
        }
        { int t = me->item; me->item = foe->item; foe->item = t; }
        { int t = me->item_used; me->item_used = foe->item_used; foe->item_used = t; }
        obs_emit(EV_TRANSFER,me,me->item,me->item_used?2:1,0);
        obs_emit(EV_TRANSFER,foe,foe->item,foe->item_used?2:1,0);
        if (me->item != ITEM_SCARF) me->choice_lock = -1;
        if (foe->item != ITEM_SCARF) foe->choice_lock = -1;
        // Update newly acquired Lum for G3 states; sleeping-only legacy Trick
        // remains frozen under D4, just like its historical berry timing.
        if(me->status || me->confusion) status_berry_check(me);
        if(foe->status || foe->confusion) status_berry_check(foe);
        return;
    default: break;
    }
    if((mv==S8V_COUNTER && !me->counter_valid) || (mv==S8V_MIRRORCOAT && !me->coat_valid) ||
       ((mv==S8V_COMEUPPANCE || mv==S8V_METALBURST) && !me->attacked_turn)) {
        attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); return;
    }
    if(mv==S8V_MEMENTO) {
        if(s8_is_protected(foe,mv)) { attempt_result(me,ATTEMPT_NOT_FAIL); obs_emit(EV_HIT,me,HIT_PROTECT,0,0); return; }
        if(!s8_attack_accuracy(me,foe,mv,r)) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_MISS,0,0); return; }
        if(s8_sub_blocks(foe,mv)) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_SUB,0,0); return; }
        const S8StatEffect drop={mv,S8_STAT_SELFBOOST,100,0,{-2,0,-2,0,0,0}};
        if(!s8_boost_modern(me,foe) && !s8_ability_boost(foe,me,-2)) { s8_ability_record(foe,S8AE_TRY_BOOST,S8A_PUBLIC); obs_emit(EV_ABILITY,foe,AB_CB,0,0); }
        s8_apply_stat_effect(me,foe,&drop,1,r);
        int before=me->hp; me->hp=0; queue_faint(me,FAINT_SELF_DESTRUCT); obs_hp(me,before,HP_COST); return;
    }
    // damaging moves
    if(mv==MV_SN && !me->sleep && !CUR_SLEEP_ACTION) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
    if (s8_is_protected(foe,mv) && !CUR_FUTURE_IMPACT) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
    if(mv>=MV_FP && damage_effectiveness(foe,mv)==0) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_IMMUNE,0,0); return; }
    if(mv==S8V_ENDEAVOR && me->hp>=foe->hp) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); return; }
    if(s8_ohko(mv) && s8_ohko_chance(me,foe,mv,50,50)<0) {
        attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_IMMUNE,0,0); return;
    }
    const S8Multihit *multihit=s8_multihit(mv);
    S8Multihit item_plan={mv,1,1,0,{0}};
    if(!multihit && s8_items_context(me,foe)) multihit=&item_plan;
    S8Multihit bond_plan={mv,2,2,0,{0}};
    if(S8_PROPERTY && S8_PROPERTY->parental) multihit=&bond_plan;
    if (!(s8_boost_move(mv)?s8_boost_accuracy(me,foe,M,r,mv==S8B_DARKESTLARIAT || mv==S8B_SACREDSWORD):s8_variable_supported(mv)?s8_attack_accuracy(me,foe,mv,r):multihit?s8_multihit_accuracy(me,foe,M,r,1):accuracy_check(me,foe,M,r))) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_MISS,0,0)); return; }
    // All Ground immunities use the shared groundedness query.
    if (M->type == T_GROUND && !is_grounded(foe)) { if(!OBS || OBS->version<2 || (held_item(foe)==ITEM_BALLOON && !foe->item_used)) obs_item(foe,0); (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_IMMUNE,0,0)); return; }
    if(damage_effectiveness(foe,mv)==0) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_IMMUNE,0,0)); return; }
    s8_break_protect(foe,mv); // after immunity/accuracy, before Substitute
    if(mv==S8TA_M_RAGINGBULL) {
        if(foe->reflect) obs2_emit(EV_SCREEN_END,foe,0,0,0);
        if(foe->light_screen) obs2_emit(EV_SCREEN_END,foe,1,0,0);
        foe->reflect=foe->light_screen=foe->aurora_veil=0;
    }
    if(S8_PROPERTY && S8_PROPERTY->parental && (mv==MV_BB || mv==MV_PF)) {
        if(foe->reflect) obs2_emit(EV_SCREEN_END,foe,0,0,0);
        if(foe->light_screen) obs2_emit(EV_SCREEN_END,foe,1,0,0);
        foe->reflect=foe->light_screen=foe->aurora_veil=0;
    }
    if(multihit) { s8_execute_multihit(me,foe,multihit,r); return; }
    // Move-local onTryHit: after protection/accuracy/immunity, before Substitute.
    if(mv==MV_BB || mv==MV_PF) {
        if(foe->reflect) obs2_emit(EV_SCREEN_END,foe,0,0,0);
        if(foe->light_screen) obs2_emit(EV_SCREEN_END,foe,1,0,0);
        foe->reflect=foe->light_screen=foe->aurora_veil=0;
    }
    s8_ability_phase(me,foe,S8AE_BEFORE_HIT,mv,1);
    s8_normal_gem_hit(me,foe,M,S8_PROPERTY && S8_PROPERTY->pledge_combo);
    int before_hp = foe->hp;
    int dmg = mv<MV_COUNT && S8_CONTROL_OVERRIDE_ID!=mv ? calc_damage(me,foe,M,r) : calc_move_damage(me,foe,mv,r);
    HIT_LANDED=1;
    if (s8_sub_blocks(foe,mv) && !s8_property_sound(mv)) {
        LAST_DIRECT_DAMAGE=dmg<foe->sub_hp?dmg:foe->sub_hp;
        foe->sub_hp -= dmg;
        if (foe->sub_hp < 0) foe->sub_hp = 0;
        obs_emit(EV_HIT,me,HIT_SUB,foe->sub_hp>0,0);
        if(!foe->sub_hp) obs_emit(EV_SUB,foe,0,0,0);
        s8_ability_phase(me,foe,S8AE_AFTER_HIT,mv,1);
        apply_move_effect(me,foe,mv,0,r);
        s8_property_stench(me,foe,mv,0,r);                  // self effects survive Substitute
        g5_after_damage(me,foe,mv,LAST_DIRECT_DAMAGE);
        s8_apply_self_boost(me,foe,mv,r);
        post_hit_attacker(me, mv>=MV_FP?LAST_DIRECT_DAMAGE:dmg);                        // Life Orb / Shell Bell apply vs substitutes too
        return;                                            // target effects blocked
    }
    s8_hit_before(me,foe,M,0);
    dmg=s8_ability_event(foe,S8AE_HP_DAMAGE,me,foe,M,dmg,0);
    foe->hp -= dmg;
    // Focus Sash (from full HP) / Focus Band (10%): survive a KO hit at 1 HP.
    if (foe->hp <= 0) {
        if(foe->endured_now) foe->hp=1;
        else if (held_item(foe) == ITEM_SASH && !foe->item_used && before_hp >= foe->maxhp) { foe->hp = 1; s8_item_spend(foe,1); }
        else if (held_item(foe) == ITEM_FBAND && rng_dbl(r) < 0.10) { foe->hp = 1; obs_item(foe,0); }
    }
    s8_ability_phase(me,foe,S8AE_AFTER_HIT,mv,1);
    LAST_DIRECT_DAMAGE=before_hp-(foe->hp>0?foe->hp:0);
    s8_damaging_hit(foe,M->cat,LAST_DIRECT_DAMAGE);
    s8_got_attacked(foe,LAST_DIRECT_DAMAGE,1);
    if(foe->focusing) foe->focus_lost=1;
    if(mv>=MV_FP || s8_hit_implemented(s8_current_ability(foe)) || s8_switch_implemented(s8_current_ability(me)))
        queue_faint_from(foe,FAINT_DAMAGE,me,M);
    obs_hp(foe,before_hp,HP_HIT);
    if(!status_move) status_damaging_hit(foe,M->type,M->cat,0);
    s8_beak_contact(me,foe,mv,0,r);
    s8_hit_onhit(me,foe,M,S8_HIT_CONTEXT->crit);
    apply_move_effect(me,foe,mv,1,r);
    s8_property_stench(me,foe,mv,1,r);
    // Inherited Fire DamagingHit follows secondaries. A frozen target cannot
    // receive the same hit's burn. Preserve the frozen legacy-68 ordering.
    if(status_move) status_damaging_hit(foe,M->type,M->cat,0);
    if (mv == MV_PN && foe->hp > 0 && !(S8_PROPERTY && S8_PROPERTY->sheer) && s8_guard_secondary(foe,0) && s8_guard_volatile(foe,S8G_HEALBLOCK)) { foe->healblock = 2; obs_emit(EV_BLOCK,foe,1,0,0); }
    white_herb_check(foe);
    if (foe->hp > 0) {
        if (held_item(me) == ITEM_KINGS && s8_guard_secondary(foe,0) && (S8_PROPERTY && (S8_PROPERTY->sheer || S8_PROPERTY->stench) ? !s8_property_flinch(mv) : mv>=MV_COUNT ? !s8_native_flinch(mv) : MOVE_EFFECTS[mv].kind!=EFFECT_FLINCH) && rng_dbl(r) < 0.10 && (mv<V1_MOVE_COUNT || !CUR_SECOND) && !foe->focusing) s8_guard_flinch(foe);   // King's Rock
        if (held_item(foe) == ITEM_BALLOON && !foe->item_used && dmg > 0) { s8_item_spend(foe,0); }  // balloon pops
    }
    s8_hit_damaging(me,foe,M,LAST_DIRECT_DAMAGE);
    if(foe->hp>0) berry_hp_check(foe);
    g5_after_damage(me,foe,mv,LAST_DIRECT_DAMAGE);
    // Scald / Scorching Sands thaw in AfterMoveSecondary, after recoil and
    // before the attacker's AfterMoveSecondarySelf. Never through Substitute.
    if(status_move && status_move->thaws_target && foe->hp>0 && foe->status==STATUS_FRZ)
        clear_major_status(foe);
    s8_hit_finish(me,foe,M);
    s8_apply_self_boost(me,foe,mv,r);
    post_hit_attacker(me, mv>=MV_FP?LAST_DIRECT_DAMAGE:dmg);
    if (M->contact && held_item(foe) == ITEM_HELMET && me->hp > 0 && s8_resist_loss(me,1,S8R_OTHER)) {
        int before=me->hp; me->hp -= me->maxhp / 6; obs_item(foe,0); obs_hp(me,before,HP_RECOIL);                           // Rocky Helmet recoil
        s8_ability_event(me,S8AE_EMERGENCY_EXIT,me,me,NULL,before,0);
        berry_hp_check(me);
    }
}

static void execute_move(Mon *me, Mon *foe, int mv, Rng *r) {
    S8PropertyContext property={.user=me,.target=foe,.move=mv},*saved_property=S8_PROPERTY;
    if(CUR_FUTURE_IMPACT) { property.prankster=0;property.priority=0; }
    else if(saved_property && (S8_CALL_DEPTH || S8_BOUNCED)) {
        property.prankster=S8_BOUNCED?0:saved_property->prankster;property.priority=saved_property->priority;
    } else if(field_member(me) && FIELD.move_queued[me==FIELD.seat[1]]) {
        property.prankster=FIELD.prankster_boosted[me==FIELD.seat[1]];
        property.priority=FIELD.committed_priority[me==FIELD.seat[1]];
    } else {
        property.prankster=move_def(mv)->cat==CAT_STATUS && s8_has_ability(me,S8V3_PRANKSTER);
        property.priority=s8_move_priority(me,mv);
    }
    S8_PROPERTY=&property;
    Rng *saved_power_rng=S8_POWER_RNG;S8_POWER_RNG=r;
    S8HitContext hit_context={.rng=r},*saved_hit_context=S8_HIT_CONTEXT;
    int sleep_caller=S8_SLEEP_CALLER && (!saved_hit_context || saved_hit_context->user==me);
    hit_context.smart_target=s8_hit_smart_target(mv);
    hit_context.user=me;hit_context.used_move=CUR_FUTURE_IMPACT?0:sleep_caller?S8_SLEEP_CALLER:mv+1;
    if(S8_CALL_DEPTH && saved_hit_context && saved_hit_context->user==me)
        hit_context.used_move=saved_hit_context->used_move;
    S8_HIT_CONTEXT=&hit_context;
    if(s8_boost_move(mv) || s8_primary_status(mv) || s8_heal_move(mv) || s8_stateful_heal(mv) || s8_field_move(mv) || s8_trap_move(mv) || s8_control_move(mv) || s8_rampage_move(mv) || s8_protect_move(mv) || s8_turn_move(mv) || s8_type_ability_move(mv) || s8_item_switch_move(mv) || s8_special_move(mv) || s8_rapidspin_move(mv)) {
        me->boost_stats_active=foe->boost_stats_active=1;
        me->state_features|=MON_POST_V1_STATE;
    }
    if(mv>=MV_FP && !field_member(me)) field_bind(me,foe);
    if(mv>=MV_COUNT && s8_special_move(mv)) S8_SPECIAL_ACTIVE=1;
    if(s8_item_switch_move(mv) || s8_special_move(mv)) s8_item_enable(me,foe);
    if(s8_stalling_move(mv) && field_member(me)) FIELD.guard_context=1;
    if(s8_trap_move(mv) && field_member(me)) FIELD.trap_context=1;
    if((s8_control_move(mv) || s8_rampage_move(mv)) && field_member(me)) FIELD.control_context=1;
    if(s8_turn_move(mv)) FIELD.turn_context=FIELD.control_context=1;
    if(FIELD.turn_context && !S8_CALL_DEPTH && !CUR_SLEEP_ACTION && !CUR_FUTURE_IMPACT) me->action_move=mv+1;
    int saved_serial=FIELD.move_serial;
    int saved_id=S8_CONTROL_OVERRIDE_ID; MoveDef saved_override=S8_CONTROL_OVERRIDE;
    s8_type_modify(me,foe,mv,r);
    if(me->control[S8C_ELECTRIFY] && !CUR_FUTURE_IMPACT) {
        S8_CONTROL_OVERRIDE=*move_def(mv); S8_CONTROL_OVERRIDE.type=T_ELEC; S8_CONTROL_OVERRIDE_ID=mv;
    }
    s8_ability_event(me,S8AE_MODIFY_TYPE,me,foe,move_def(mv),1,0);
    s8_ability_event(me,S8AE_MODIFY_MOVE,me,foe,move_def(mv),1,1);
    if(field_member(me) && !S8_CALL_DEPTH) FIELD.acted[me==FIELD.seat[1]]=1;
    int damp_blocked=!s8_boost_any(me,foe,S8AE_TRY_MOVE,move_def(mv),1,0);
    if(!damp_blocked && (mv==MV_SD || mv==MV_EXP || mv==S8V_MISTYEXPLOSION)) {
        int before=me->hp; me->hp=0; me->state_features|=MON_POST_V1_STATE;
        queue_faint(me,FAINT_SELF_DESTRUCT); obs_hp(me,before,HP_COST);
    }
    HIT_LANDED=G5_COST_DONE=LAST_DIRECT_DAMAGE=0;
    if(mv>=MV_COUNT && (S8_MOVES[mv].flags&S8_FLAG_GRAVITY) && current_gravity(me)) {
        attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0);
        S8_CONTROL_OVERRIDE_ID=saved_id; S8_CONTROL_OVERRIDE=saved_override;S8_POWER_RNG=saved_power_rng;S8_HIT_CONTEXT=saved_hit_context;S8_PROPERTY=saved_property; return;
    }
    obs2_emit(EV_ATTACK_START,me,mv,CUR_FUTURE_IMPACT,0);
    Mon *saved_source=S8_EFFECT_SOURCE; S8_EFFECT_SOURCE=me;
    const MoveDef *saved_status_effect=S8_STATUS_EFFECT;S8_STATUS_EFFECT=move_def(mv);
    // Psychic Terrain is TryHit, before protection/accuracy; only opposing targets.
    int opposing=move_def(mv)->cat!=CAT_STATUS || (mv<MV_COUNT?
        mv==MV_PU || mv==MV_SWG || mv==MV_SF || mv==MV_TRK:
        !strcmp(S8_MOVES[mv].target,"normal") || !strcmp(S8_MOVES[mv].target,"allAdjacentFoes"));
    s8_items_pressure(me,foe,mv);
    if(damp_blocked || !s8_order_try_move(me,foe,mv)) {
        attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);
    } else if(me->throatchop && s8_move_sound(mv)) {
        // ModifyMove also blocks called sound moves after the caller spent PP.
        attempt_result(me,ATTEMPT_FAIL);obs_emit(EV_HIT,me,HIT_FAIL,0,0);
    } else if(!CUR_FUTURE_IMPACT && field_member(foe) && FIELD.terrain==TERRAIN_PSYCHIC &&
       property.priority>0 && opposing && is_grounded(foe) && s8_semi(foe)<0) {
        attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0);
    } else execute_move_core(me,foe,mv,r);
    S8_EFFECT_SOURCE=saved_source;S8_STATUS_EFFECT=saved_status_effect;
    s8_fling_update(me); // also on miss, protection and immunity
    if(FIELD.item_context) s8_field_item_update(me,foe);
    if(HIT_LANDED && hit_context.has_last && (mv==S8IS_M_CIRCLETHROW || mv==S8IS_M_DRAGONTAIL)) s8_drag_out(me,foe,move_def(mv));
    s8_special_after(foe,mv);
    if(hit_context.has_last) s8_ability_event(foe,S8AE_EMERGENCY_EXIT,me,foe,NULL,hit_context.last_hp+hit_context.total,0);
    s8_ability_phase(me,foe,S8AE_AFTER_MOVE,mv,HIT_LANDED);
    s8_ability_phase(me,foe,S8AE_UPDATE,mv,0);
    if(!CUR_FUTURE_IMPACT) s8_rampage_after(me,r);
    if(mv==S8B_SPITUP) s8_consume_stockpile(me); // AfterMove, even after failure
    g5_after_damage(me,foe,mv,LAST_DIRECT_DAMAGE);
    if(mv>=MV_FP) queue_faint(me,FAINT_RECOIL);
    if(!CUR_FUTURE_IMPACT && mv!=S8C_M_CHARGE && move_def(mv)->type==T_ELEC) me->control[S8C_CHARGE]=0;
    if(!CUR_FUTURE_IMPACT && move_def(mv)->cat!=CAT_STATUS && HIT_LANDED) s8_control_destiny(me,foe);
    s8_control_links_update(me,foe);
    S8_CONTROL_OVERRIDE_ID=saved_id; S8_CONTROL_OVERRIDE=saved_override;
    s8_trap_links_update(me,foe);
    obs3_publish();
    obs2_emit(EV_ATTACK_END,me,0,0,0);
    if(mv==S8U_M_BEAKBLAST) me->beak_hot=0;
    if(FIELD.turn_context && !CUR_FUTURE_IMPACT) {
        if(FIELD.move_serial==saved_serial) { FIELD.last_used=mv+1;FIELD.move_serial++; }
        // Instruct's prioritized action is drained only after the caller's
        // AfterMove and faint handling. It does not consume the original queue.
        if(!S8_CALL_DEPTH && me->hp>0 && foe->hp>0) {
            int pending=FIELD.instructed[foe==FIELD.seat[1]];
            FIELD.instructed[foe==FIELD.seat[1]]=0;
            if(pending) s8_run_instruct(foe,me,pending-1,r);
        }
    }
    s8_after_faint();
    s8_process_switches();
    S8_POWER_RNG=saved_power_rng;
    S8_HIT_CONTEXT=saved_hit_context;S8_PROPERTY=saved_property;
}
static void future_residual(Mon *a,Mon *b,Rng *r) {
    Mon *ms[]={a,b}; int due[2]={0,0};
    for(int i=0;i<2;i++) if(ms[i]->future_turns) {
        obs2_emit(EV_FUTURE_TICK,ms[i],0,0,0);
        if(!--ms[i]->future_turns) due[i]=1;
    }
    if(!r) return;
    int first=0;
    // Slot-condition residual handlers use holder Speed. Only simultaneous
    // impacts need a new tie-break; an isolated reservation adds no order draw.
    if(due[0] && due[1]) {
        double sa=s8_action_speed(a), sb=s8_action_speed(b);
        if(sb>sa || (sb==sa && rng_dbl(r)<.5)) first=1;
    }
    for(int k=0;k<2;k++) {
        int i=k?1-first:first;
        if(!due[i]) continue;
        if(a->hp<=0 || b->hp<=0) break; // checkWin after each impact in 1v1
        Mon *target=ms[i], *source=ms[1-i];
        int saved=source->attempt_this;
        obs2_emit(EV_FUTURE_IMPACT,target,MV_FS,0,0);
        CUR_FUTURE_IMPACT=1; execute_move(source,target,MV_FS,r); CUR_FUTURE_IMPACT=0;
        if(!HIT_LANDED && held_item(source)==ITEM_LIFEORB) { post_hit_attacker(source,1); queue_faint(source,FAINT_RECOIL); }
        source->attempt_this=saved;
    }
}

// Sleep Talk can call Snore (no nosleeptalk/charge flag). Called moves spend
// only Sleep Talk's PP. Scope the sleep context without changing Mon's timer.
static void execute_sleep_move(Mon *me, Mon *foe, int mv, Rng *r) {
    int saved_caller=S8_SLEEP_CALLER;S8_SLEEP_CALLER=MV_ST+1;
    CUR_SLEEP_ACTION=1;
    execute_move(me,foe,mv,r);
    CUR_SLEEP_ACTION=0;
    S8_SLEEP_CALLER=saved_caller;
}
// A prioritized Instruct action runs the normal gates and PP deduction but
// leaves the original choice/consumed flags intact. No new policy invocation.
static void s8_run_instruct(Mon *me,Mon *foe,int mv,Rng *r) {
    int second=CUR_SECOND,slot=find_move(me,mv),saved_acted=FIELD.acted[me==FIELD.seat[1]];
    CUR_SECOND=FIELD.consumed[foe==FIELD.seat[1]];
    s8_control_begin_action(me);
    if(me->recharge) { me->recharge=0;attempt_result(me,ATTEMPT_SKIP_NOT_FAIL);goto done; }
    if(!freeze_before_move(me,s8_selected_defrost(me,slot),0,r)) { attempt_result(me,ATTEMPT_SKIP_FAIL);goto done; }
    if(me->flinched) { s8_hit_flinch(me);me->flinched=0;attempt_result(me,ATTEMPT_SKIP_FAIL);goto done; }
    if(me->healblock) me->healblock--;
    if((S8_MOVES[mv].flags&S8_FLAG_GRAVITY) && current_gravity(me)) { attempt_result(me,ATTEMPT_SKIP_FAIL);goto done; }
    s8_earlybird_before_move(me);
    if(me->sleep) {
        if(!s8_has_ability(me,S8V3_EARLYBIRD)) { me->sleep--;obs2_emit(EV_SLEEP_TICK,me,0,0,0); }
        if(!me->sleep) obs_emit(EV_SLEEP,me,0,0,0);
        // Preserve the engine's existing sleep opportunity convention.
        int st=find_move(me,MV_ST);
        if(st>=0 && me->pp[st]>0 && s8_control_before_move(me,foe,MV_ST,r)) {
            int other[4],n=0;s8_spend_pp(me,st);leppa_check(me,st);
            for(int i=0;i<me->nmoves;i++) if(i!=st && g5_sleep_callable(me->moves[i])) other[n++]=i;
            if(n) { int call=me->moves[other[rng_int(r,n)]];execute_sleep_move(me,foe,call,r);note_move_used(me,call); }
        } else attempt_result(me,ATTEMPT_SKIP_FAIL);
        goto done;
    }
    if(!s8_control_before_move(me,foe,mv,r)) goto done;
    if(slot<0 || me->pp[slot]<=0) { attempt_result(me,ATTEMPT_FAIL);goto done; }
    obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,mv},0);
    if(!s8_rampage_locked(me) && !me->charging) s8_spend_pp(me,slot);
    leppa_check(me,slot);execute_move(me,foe,mv,r);note_move_used(me,mv);
done:
    FIELD.acted[me==FIELD.seat[1]]=saved_acted;
    CUR_SECOND=second;
}

// A committed Snore takes precedence over the legacy automatic Sleep Talk.
// Called before the old sleep branch; other old sleep choices are untouched.
static int selected_snore(Mon *me, Mon *foe, int slot, Rng *r) {
    s8_earlybird_before_move(me);
    if(me->sleep<=0 || slot<0 || me->moves[slot]!=MV_SN) return 0;
    if(!s8_has_ability(me,S8V3_EARLYBIRD)) { obs2_emit(EV_SLEEP_TICK,me,0,0,0); me->sleep--; }
    if(!me->sleep) obs_emit(EV_SLEEP,me,0,0,0);
    if(!s8_control_before_move(me,foe,MV_SN,r)) return 1;
    obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,MV_SN},0);
    s8_spend_pp(me,slot); leppa_check(me,slot);
    CUR_SLEEP_ACTION=1;execute_move(me,foe,MV_SN,r);CUR_SLEEP_ACTION=0;
    note_move_used(me,MV_SN);
    return 2;
}

// Struggle (わるあがき), ported from sim5's formula.
static void do_struggle(Mon *me, Mon *foe, Rng *r) {
    // New move properties need the same actual-hit pipeline even for Struggle.
    // Preserve the historical kernel for the pre-8d-1d compatibility domain.
    if(s8_items_context(me,foe) || s8_form_implemented(s8_current_ability(me)) || s8_form_implemented(s8_current_ability(foe)) ||
       s8_boost_modern(me,foe) || (s8_property_implemented(s8_current_ability(me)) && s8_has_ability(me,s8_current_ability(me))) ||
       (s8_property_implemented(s8_current_ability(foe)) && s8_has_ability(foe,s8_current_ability(foe)))) {
        int saved_id=S8_CONTROL_OVERRIDE_ID;MoveDef saved=S8_CONTROL_OVERRIDE;
        S8_CONTROL_OVERRIDE=(MoveDef){S8_MOVES[S8D_STRUGGLE].code,-1,CAT_PHYS,50,101,0,1,1};
        S8_CONTROL_OVERRIDE_ID=S8D_STRUGGLE;
        obs_emit_action(EV_USE,me,(MoveAction){ACTION_STRUGGLE,0},0);
        execute_move(me,foe,S8D_STRUGGLE,r);
        int before=me->hp,loss=(me->maxhp+2)/4;
        if(loss<1) loss=1;
        if(HIT_LANDED && before>0) { me->hp=loss<before?before-loss:0;queue_faint(me,FAINT_RECOIL);obs_hp(me,before,HP_RECOIL); }
        S8_CONTROL_OVERRIDE_ID=saved_id;S8_CONTROL_OVERRIDE=saved;
        return;
    }
    S8HitContext hit_context={.rng=r},*saved_hit_context=S8_HIT_CONTEXT;
    S8_HIT_CONTEXT=&hit_context;
    if(FIELD.turn_context) { me->action_move=0; FIELD.last_used=-1;FIELD.move_serial++; }
    if(me->uproar) me->uproar_struggle=1;
    attempt_result(me,ATTEMPT_SUCCESS);
    obs_emit_action(EV_USE,me,(MoveAction){ACTION_STRUGGLE,0},0); obs_emit_action(EV_NOTE,me,(MoveAction){ACTION_STRUGGLE,0},0);
    int raw_def=field_member(foe) && FIELD.wonder_room?foe->stats[4]:foe->stats[2];
    double d = raw_def * smult(foe->st[1]);
    const MoveDef struggle={"struggle",-1,CAT_PHYS,50,101,0,1,1};
    int defmod=s8_ability_event(foe,S8AE_DEF,me,foe,&struggle,4096,0);
    if(current_weather(foe)==WEATHER_SNOW && has_type(foe,T_ICE)) defmod=damage_chain(6144,defmod);
    if(defmod!=4096) d=item_mod((int)d,defmod);
    if(d<1) d=1;
    long base = (long)floor(22.0 * 50 * me->stats[1] / d);
    base = base / 50 + 2;
    int roll = 85 + rng_int(r, 16);
    int dmg = (int)floor(base * roll / 100.0);
    S8AbilityEvent se={S8AE_DAMAGE,me,foe,&struggle,4096,0};
    S8AbilityCall sc[12];int sn=0;
    const Mon *sh[7]={foe,me};int shn=2;
    int ss=s8_resist_side(foe);
    if(ss>=0) for(int i=0;i<FIELD.power_ally_count[ss];i++) sh[shn++]=FIELD.power_allies[ss][i];
    for(int k=0;k<shn;k++) if(sh[k] && sh[k]->hp>0)
        for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
            const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
            int scope=s8_resist_scope(row);
            if(row->event!=S8AE_DAMAGE || !s8_resist_implemented(row->ability) ||
               (scope==1?sh[k]!=foe:scope!=3) || !s8_has_ability(sh[k],row->ability)) continue;
            sc[sn++]=(S8AbilityCall){sh[k],row,eff_speed(sh[k]),0};
        }
    s8_ability_run_queue(sc,sn,&se,r);
    if(se.value!=4096) { dmg=item_mod(dmg,se.value);if(dmg<1) dmg=1; }
    if(foe->aurora_veil) { dmg=item_mod(dmg,2048); if(dmg<1) dmg=1; }
    if(foe->control[S8C_GLAIVERUSH]) dmg=item_mod(dmg,8192);
    // Preserve Struggle's legacy kernel; apply the new burn stage only when active.
    if(me->status==STATUS_BRN) { dmg=item_mod(dmg,2048); if(dmg<1) dmg=1; }
    if (foe->sub_hp > 0) { foe->sub_hp -= dmg; if (foe->sub_hp < 0) foe->sub_hp = 0; obs_emit(EV_HIT,me,HIT_SUB,foe->sub_hp>0,0); if(!foe->sub_hp) obs_emit(EV_SUB,foe,0,0,0); }
    else { int before=foe->hp;s8_hit_before(me,foe,&struggle,0); foe->hp -= dmg; if(foe->endured_now && foe->hp<1) foe->hp=1; if(foe->focusing) foe->focus_lost=1; obs_hp(foe,before,HP_HIT);
        int dealt=before-(foe->hp>0?foe->hp:0); s8_damaging_hit(foe,CAT_PHYS,dealt); s8_got_attacked(foe,dealt,1);
        if(s8_hit_implemented(s8_current_ability(foe))) queue_faint(foe,FAINT_DAMAGE);
        s8_hit_onhit(me,foe,&struggle,0);s8_hit_damaging(me,foe,&struggle,dealt); }
    int before=me->hp; me->hp -= dmg / 4; obs_hp(me,before,HP_RECOIL);
    s8_hit_finish(me,foe,&struggle);
    if(FIELD.control_context) { queue_faint(foe,FAINT_DAMAGE);queue_faint(me,FAINT_RECOIL);s8_control_destiny(me,foe); }
    s8_rampage_after(me,r);
    S8_HIT_CONTEXT=saved_hit_context;
}

// ---------- Damage estimate (for the generic policy) ----------
// Exact discrete expectation of the same integer chain as calc_damage. No RNG,
// observer access or item mutation. Crit probabilities and all 16 rolls match.
static int est_damage_core(Mon *atk, Mon *def, int mvi) {
    const MoveDef *mv=move_def(mvi);
    if(mv->cat==CAT_STATUS || (mv->power<=0 && mvi!=S8IS_M_FLING) || (mvi==MV_SN && !atk->sleep)) return 0;
    if(mv->type==T_GROUND && def->item==ITEM_BALLOON && !def->item_used) return 0;
    if(!s8_property_ignored(def) && !(FIELD.ignore_breakable && s8_resist_breakable(s8_current_ability(def))) &&
       s8_has_ability(def,s8_current_ability(def)) &&
       s8_resist_matches(def,atk,mv,s8_current_ability(def))) return 0;
    double eff=damage_effectiveness(def,mvi);
    if(eff<=0) return 0;
    int berry=damage_berry(def,mv,eff), sum[2]={0,0};
    for(int crit=0;crit<2;crit++) for(int roll=85;roll<=100;roll++)
        sum[crit]+=damage_value(atk,def,mvi,crit,roll,&DAMAGE_NEUTRAL,berry);
    double cp=damage_crit_probability(atk,def,mvi);
    int estimate=(int)(((1-cp)*sum[0]+cp*sum[1])/16);
    return estimate<1?1:estimate;
}

// Generic estimates use the same active type/power/category without spending RNG.
// Shell Side Arm ties average both category expectations, without a random draw.
static int est_damage_before_property(Mon *atk,Mon *def,int mv) {
    if(s8_item_switch_move(mv)) {
        if((mv==S8IS_M_FLING && (!held_item(atk) || !s8_item_takeable(atk))) ||
           (mv==S8IS_M_POLTERGEIST && !def->item)) return 0;
        int saved=LIVE_DAMAGE;LIVE_DAMAGE=1;
        int value=est_damage_core(atk,def,mv);LIVE_DAMAGE=saved;return value;
    }
    if(!s8_type_ability_move(mv)) return est_damage_core(atk,def,mv);
    if(!s8_type_eligible(atk,mv)) return 0;
    int saved_id=S8_CONTROL_OVERRIDE_ID,saved_live=LIVE_DAMAGE;
    MoveDef saved=S8_CONTROL_OVERRIDE;
    int tied=s8_type_modify(atk,def,mv,NULL);
    if(atk->control[S8C_ELECTRIFY]) {
        S8_CONTROL_OVERRIDE=*move_def(mv);S8_CONTROL_OVERRIDE.type=T_ELEC;S8_CONTROL_OVERRIDE_ID=mv;
    }
    LIVE_DAMAGE=1;
    int value=est_damage_core(atk,def,mv);
    if(tied) {
        S8_CONTROL_OVERRIDE.cat=CAT_PHYS;S8_CONTROL_OVERRIDE.contact=1;
        value=(value+est_damage_core(atk,def,mv))/2;
    }
    LIVE_DAMAGE=saved_live;S8_CONTROL_OVERRIDE_ID=saved_id;S8_CONTROL_OVERRIDE=saved;
    return value;
}

static int est_damage(Mon *atk,Mon *def,int mv) {
    if(!s8_property_implemented(s8_current_ability(atk)) && !s8_property_implemented(s8_current_ability(def)))
        return est_damage_before_property(atk,def,mv);
    if(!s8_type_eligible(atk,mv)) return 0;
    if((mv==S8IS_M_FLING && (!held_item(atk) || !s8_item_takeable(atk))) ||
       (mv==S8IS_M_POLTERGEIST && !def->item)) return 0;
    int saved_id=S8_CONTROL_OVERRIDE_ID,saved_live=LIVE_DAMAGE;
    MoveDef saved=S8_CONTROL_OVERRIDE;
    S8PropertyContext p={.user=atk,.target=def,.move=mv},*saved_p=S8_PROPERTY;S8_PROPERTY=&p;
    int tied=s8_type_modify(atk,def,mv,NULL);
    if(atk->control[S8C_ELECTRIFY]) {
        S8_CONTROL_OVERRIDE=*move_def(mv);S8_CONTROL_OVERRIDE.type=T_ELEC;S8_CONTROL_OVERRIDE_ID=mv;
    }
    s8_ability_event(atk,S8AE_MODIFY_TYPE,atk,def,move_def(mv),1,0);
    s8_ability_event(atk,S8AE_MODIFY_MOVE,atk,def,move_def(mv),1,0);
    LIVE_DAMAGE=1;
    int value=est_damage_core(atk,def,mv);
    if(tied) {
        S8_CONTROL_OVERRIDE.cat=CAT_PHYS;
        S8_CONTROL_OVERRIDE.contact=!s8_has_ability(atk,S8D_LONGREACH);
        value=(value+est_damage_core(atk,def,mv))/2;
    }
    LIVE_DAMAGE=saved_live;S8_CONTROL_OVERRIDE_ID=saved_id;S8_CONTROL_OVERRIDE=saved;S8_PROPERTY=saved_p;
    return value;
}

// ---------- Observable heuristic v2 (obsh2, revision 2) ----------
// Pure functions of the frozen MGO1 vector, mask and public move rules only.
// The stat prior below is NOT a reconstructed foe: equal-weight public-rule
// hypotheses (0/16/32 points, hindering/neutral/boosting nature). No build IDs,
// true-stat estimates, observer access, persistent memory or RNG are used.
static int h2_slot(const Observation *o, const ActionMask *mask, int mv) {
    for(int i=0;i<4;i++) if(mask->legal[i] && o->v[48+4*i] && o->v[49+4*i]==mv) return i;
    return -1;
}
static int h2_revealed(const Observation *o, int mv) {
    for(int i=0;i<4;i++) {
        int j=79+8*i;
        if(o->v[j] && o->v[j+1]==mv && (!o->v[j+5] || o->v[j+7]>0)) return 1;
    }
    return 0;
}
static double h2_damage(const Observation *o, int id, int outgoing) {
    const int32_t *v=o->v; const MoveDef *m=&MOVES[id];
    if(m->cat==CAT_STATUS) return 0;
    int physical=m->cat==CAT_PHYS, ai=id==MV_BP?2:physical?1:3, di=physical?2:4;
    int as=ai-1, ds=di-1;
    int item=outgoing?v[42]:(v[111]?v[112]:ITEM_NONE);
    int mega=v[outgoing?4:14], type1=v[outgoing?5:15], type2=v[outgoing?6:16];
    int defitem=outgoing?(v[111]?v[112]:ITEM_NONE):v[42];
    int defused=outgoing?v[113]==2:v[43];
    if(m->type==T_GROUND && defitem==ITEM_BALLOON && !defused) return 0;
    double eff=eff_vs(m->type,v[outgoing?15:5])*eff_vs(m->type,v[outgoing?16:6]);
    double stab=(m->type==type1 || m->type==type2)?1.5:1.0;
    int power=m->power;
    if(m->contact && mega) power=(int)(power*1.3);
    if((physical && item==ITEM_MUSCLE) || (!physical && item==ITEM_WISE)) power=item_mod(power,4506);
    if((item==ITEM_TAG && m->type==T_GHOST) || (item==ITEM_SAND && m->type==T_GROUND) ||
       (item==ITEM_SPOON && m->type==T_PSY) || (item==ITEM_BBELT && m->type==T_FIGHT)) power=item_mod(power,4915);
    int astage=v[(outgoing?31:66)+as], dstage=v[(outgoing?66:31)+ds];
    int cs=(id==MV_SCL)+(item==ITEM_LENS);
    double cp=cs==2?0.5:cs==1?0.125:1.0/24.0, total=0;
    // Average over public stat hypotheses; never select a hypothesis by foe truth.
    for(int points=0;points<=32;points+=16) for(int nature=9;nature<=11;nature++) {
        double a=outgoing?v[25+ai]:floor((v[17+ai]+20+points)*nature/10.0);
        double d=outgoing?floor((v[17+di]+20+points)*nature/10.0):v[25+di];
        double expected=0;
        for(int crit=0;crit<2;crit++) {
            double aa=a*smult(crit && astage<0?0:astage);
            double dd=d*smult(crit && dstage>0?0:dstage);
            int b=(int)floor(22.0*power*aa/dd)/50+2;
            b=(int)floor(b*stab); b=(int)floor(b*eff);
            if(eff>1 && item==ITEM_BELT) b=item_mod(b,4915);
            if(eff>1 && !defused && ((defitem==ITEM_SHUCA && m->type==T_GROUND) ||
               (defitem==ITEM_CHOPLE && m->type==T_FIGHT) || (defitem==ITEM_KASIB && m->type==T_GHOST) ||
               (defitem==ITEM_PAYAPA && m->type==T_PSY))) b/=2;
            if(crit) b=(int)floor(b*1.5);
            if(item==ITEM_LIFEORB) b=item_mod(b,5324);
            int last=v[outgoing?46:118]-1, streak=v[outgoing?47:119];
            if(item==ITEM_METRO && last==id) b=item_mod(b,4096+819*(streak>5?5:streak));
            expected+=(crit?cp:1-cp)*fmax(1,b*0.925);
        }
        // Outgoing is measured in bar points; incoming in own exact HP.
        total+=outgoing?100*expected/(v[17]+75+16):expected;
    }
    double accuracy=fmin(1,m->acc*acc_mult(v[outgoing?36:71])/100.0);
    if(item==ITEM_WIDE) accuracy=fmin(1,accuracy*1.1);
    if(defitem==ITEM_POWDER) accuracy*=0.9;
    return total/9*accuracy;
}
static double h2_pressure(const Observation *o, int id) {
    const int32_t *v=o->v; double prior=h2_damage(o,id,1);
    // A recent clean hit is bar-endpoint evidence, not numeric outgoing damage.
    // Reject costs, healing, recoil, crits and defensive setup/copy contexts.
    int foe=v[160]?v[161]:V1_MOVE_COUNT;
    int clean_foe=!v[160] || foe==MV_PRO || foe==MV_AGI || foe==MV_SF ||
        (foe<V1_MOVE_COUNT && MOVES[foe].cat!=CAT_STATUS);
    if(v[154] && !v[156] && v[155]==id && !v[159] && v[173]>0 && !v[174] && clean_foe &&
       !(v[111] && v[112]==ITEM_LIFEORB) &&
       !(v[42]==ITEM_HELMET && foe<V1_MOVE_COUNT && MOVES[foe].contact))
        return 0.5*prior+0.5*v[173];
    return prior;
}
static int obsh2(const Observation *o, const ActionMask *mask) {
    const int32_t *v=o->v;
    int first=-1; for(int i=0;i<5;i++) if(mask->legal[i]) { first=i; break; }
    if(first<0 || first==4) return first; // Terminal is never queried; Struggle is action 4.
    int slot[V1_MOVE_COUNT]; for(int m=0;m<V1_MOVE_COUNT;m++) slot[m]=h2_slot(o,mask,m);
    if(v[37]) { // Frozen engine: nominal priority orders the automatic Sleep Talk.
        if(slot[MV_PRO]>=0) return slot[MV_PRO];
        if(slot[MV_ST]>=0) return slot[MV_ST];
        return first;
    }
    double hp=v[23], maxhp=v[24], frac=hp/maxhp;
    int phys=0, spec=0, attacks=0;
    double threat=0;
    for(int m=0;m<V1_MOVE_COUNT;m++) if(h2_revealed(o,m) && MOVES[m].cat!=CAT_STATUS) {
        if(MOVES[m].cat==CAT_PHYS) phys=1; else spec=1;
        threat=fmax(threat,h2_damage(o,m,0)); attacks++;
    }
    if(!attacks) threat=0.25*maxhp;
    // Own HP-loss includes possible recoil/cost: conservative survival evidence.
    if(v[168]>0 && !(v[154] && v[155]==MV_SUB)) threat=fmax(threat,v[168]);
    int best=-1; double bestscore=-1, besthit=0;
    for(int i=0;i<4;i++) if(mask->legal[i]) {
        int m=v[49+4*i]; if(MOVES[m].cat==CAT_STATUS) continue;
        double d=h2_pressure(o,m), score=d;
        if(MOVES[m].contact && v[111] && v[112]==ITEM_HELMET) score-=100.0/6;
        if(m==MV_PN && !v[73] && !v[74] && (h2_revealed(o,MV_REST) ||
           (v[111] && v[112]==ITEM_LEFTOVERS))) score+=12;
        if(m==MV_BD && v[114]!=(1<<AB_CB) && v[70]>=0 && v[152]>0) score+=5;
        if(m==MV_MS && v[114]!=(1<<AB_CB) && v[71]>-3 && frac>0.5) score+=6;
        if(score>bestscore) { bestscore=score; best=i; besthit=d; }
    }
    // Cash a plausible KO before spending a turn on setup, unless moving second
    // would be fatal. Rounded bars and priors make this a heuristic, not certainty.
    int second=v[150] && v[142]==2;
    if(best>=0 && !v[74] && besthit>=v[64]+2 && (!second || hp>threat*1.15)) return best;
    int rest=slot[MV_REST];
    // Heal block decrements before execution; counter 1 permits Rest this turn.
    if(rest>=0 && v[38]<=1 && hp<maxhp &&
       (frac<=0.42 || (frac<0.78 && hp<=threat*1.65))) return rest;
    if(slot[MV_PU]>=0) {
        double benefit=0;
        for(int i=0;i<5;i++) {
            double w=i==1 || i==3?1:i==4?0.5:0;
            if(i==0) for(int m=0;m<V1_MOVE_COUNT;m++) if(slot[m]>=0 && MOVES[m].cat==CAT_PHYS && m!=MV_BP) w=1;
            if(i==2) for(int m=0;m<V1_MOVE_COUNT;m++) if(slot[m]>=0 && MOVES[m].cat==CAT_SPEC) w=1;
            benefit+=w*(v[66+i]-v[31+i]);
        }
        benefit+=0.75*(v[71]-v[36]);
        if(benefit>=2 && (hp>threat*1.25 || v[39]>0)) return slot[MV_PU];
    }
    if(slot[MV_TRK]>=0 && !v[4] && !v[14] && v[42]!=ITEM_STONE && !v[74] &&
       v[111] && v[112]!=ITEM_STONE && v[112]!=v[42] &&
       (v[42]==ITEM_NONE || v[42]==ITEM_IRONBALL ||
        (v[42]==ITEM_SCARF && (h2_revealed(o,MV_REST) || h2_revealed(o,MV_CP)))) &&
       (v[112]==ITEM_HELMET || v[112]==ITEM_LEFTOVERS || v[112]==ITEM_LIFEORB || v[42]==ITEM_SCARF))
        return slot[MV_TRK];
    // Two qualified losses without a self-first observation reduce (but never
    // eliminate) tie ambiguity. Counts are evidence, not an exact Speed fact.
    // Reject priorities/claws and the immediately changing order contexts.
    int stable=second && v[143] && v[145] && v[151]==0 && v[152]>=2;
    if(stable) for(int a=0;a<2;a++) {
        int m=v[a?146:144];
        if(m==MV_AGI || m==MV_SF || m==MV_BD || m==MV_PU || m==MV_TRK) stable=0;
    }
    if(stable && hp>threat*1.25 && v[64]>25) {
        if(slot[MV_AGI]>=0 && v[35]<=0) return slot[MV_AGI];
        if(slot[MV_SF]>=0 && v[35]==0 && v[70]>=0 && !v[74] && v[114]!=(1<<AB_CB) &&
           v[50+4*slot[MV_SF]]>=MOVES[MV_SF].pp-1) return slot[MV_SF];
    }
    if(hp>threat*1.6 && frac>0.55) {
        int bp=slot[MV_BP]>=0;
        if(slot[MV_CP]>=0 && ((v[32]<2 && (phys || !attacks || bp)) ||
           (v[34]<2 && (spec || !attacks)))) return slot[MV_CP];
        if(slot[MV_ID]>=0 && v[32]<2 && (phys || !attacks || bp)) return slot[MV_ID];
    }
    if(slot[MV_SUB]>=0 && !v[39] && frac>=0.8 &&
       (v[72] || h2_revealed(o,MV_SF) || h2_revealed(o,MV_MS) || h2_revealed(o,MV_TRK))) return slot[MV_SUB];
    if(slot[MV_PRO]>=0 && !v[40]) {
        // Protect needs a concrete recovery/timer benefit. A blind opening
        // scout reveals a move but no Speed and concedes a free setup turn.
        if((v[42]==ITEM_LEFTOVERS && v[38]<=1 && frac<0.9 && hp>threat) ||
           (v[38]>1 && rest>=0 && frac<0.55)) return slot[MV_PRO];
    }
    if(best>=0) return best;
    // No attacks left: use productive recovery/setup before legal no-ops.
    if(rest>=0 && v[38]<=1 && hp<maxhp) return rest;
    if(slot[MV_CP]>=0 && (v[32]<6 || v[34]<6)) return slot[MV_CP];
    if(slot[MV_ID]>=0 && v[32]<6) return slot[MV_ID];
    if(slot[MV_AGI]>=0 && v[35]<6) return slot[MV_AGI];
    if(slot[MV_PRO]>=0 && !v[40]) return slot[MV_PRO];
    return first;
}

// ---------- obsh3: pure MGO2 heuristic, no battle context or randomness ----------
static int h3_slot(const Observation2 *o,const ActionMask *mask,int mv) {
    for(int i=0;i<4;i++) if(mask->legal[i] && o->v[48+4*i] && o->v[49+4*i]==mv) return i;
    return -1;
}
static int h3_revealed(const Observation2 *o,int mv) {
    for(int i=0;i<4;i++) { int j=79+8*i; if(o->v[j] && o->v[j+1]==mv && o->v[j+7]>0) return 1; }
    return 0;
}
static double h3_damage(const Observation2 *o,int id,int outgoing) {
    const int32_t *v=o->v; const MoveDef *m=&MOVES[id];
    if(m->cat==CAT_STATUS || (id==MV_ROLL && !v[197]) || (id==MV_SN && !(outgoing?v[37]:v[72]))) return 0;
    int phys=m->cat==CAT_PHYS,ai=id==MV_BP?2:phys?1:3,di=phys?2:4;
    if(MOVE_TRAITS[id].defensive_stat) di=MOVE_TRAITS[id].defensive_stat;
    int astage=v[(outgoing?31:66)+ai-1],dstage=v[(outgoing?66:31)+di-1];
    int item=outgoing?v[42]:(v[111]?v[112]:ITEM_NONE), defitem=outgoing?(v[111]?v[112]:ITEM_NONE):v[42];
    int defused=outgoing?v[113]==2:v[43],mega=v[outgoing?4:14];
    int t1=v[outgoing?5:15],t2=v[outgoing?6:16],d1=v[outgoing?15:5],d2=v[outgoing?16:6];
    int burned=(outgoing?v[182]:v[189])==STATUS_BRN;
    int ground=outgoing?v[210]:v[201] || v[42]==ITEM_IRONBALL?GROUND_YES:
        v[205] || (v[42]==ITEM_BALLOON && !v[43])?GROUND_NO:GROUND_YES;
    if(m->type==T_GROUND && ground==GROUND_NO) return 0;
    double eff=(m->type==T_GROUND && d1==T_FLY?1:eff_vs(m->type,d1))*(m->type==T_GROUND && d2==T_FLY?1:eff_vs(m->type,d2));
    if(eff<=0) return 0;
    int power=m->power;
    if(id==MV_GK) power=120; // all legal public Metagross weight hypotheses
    if(id==MV_HP) power=outgoing?v[64]:(int)(100LL*v[23]/v[24]);
    if(id==MV_TAN) power=(outgoing?(v[186]==ATTEMPT_FAIL || v[186]==ATTEMPT_SKIP_FAIL):
        (v[221]==PUBLIC_FAIL || v[221]==PUBLIC_SKIP_FAIL))?150:75;
    if(id==MV_GYRO) {
        double ours=v[30]*smult(v[35]),theirs=(v[22]+20+16)*smult(v[70]);
        if(v[42]==ITEM_SCARF) ours*=1.5;
        if(v[42]==ITEM_IRONBALL) ours*=.5;
        if(v[111] && v[112]==ITEM_SCARF) theirs*=1.5;
        if(v[111] && v[112]==ITEM_IRONBALL) theirs*=.5;
        if(v[182]==STATUS_PAR) ours=floor(ours)/2;
        if(v[189]==STATUS_PAR) theirs=floor(theirs)/2;
        power=(int)(25*(outgoing?theirs:ours)/fmax(1,outgoing?ours:theirs))+1;
        if(power>150) power=150;
    }
    if(power<1) power=1;
    int mods=4096;
    if(m->contact && mega) mods=damage_chain(mods,5325);
    if((phys && item==ITEM_MUSCLE) || (!phys && item==ITEM_WISE)) mods=damage_chain(mods,4505);
    if((item==ITEM_TAG && m->type==T_GHOST) || (item==ITEM_SAND && m->type==T_GROUND) ||
       (item==ITEM_SPOON && m->type==T_PSY) || (item==ITEM_BBELT && m->type==T_FIGHT)) mods=damage_chain(mods,4915);
    int attacker_ground=outgoing?(v[201] || v[42]==ITEM_IRONBALL || (!v[205] && !(v[42]==ITEM_BALLOON && !v[43]))):v[210]==GROUND_YES;
    if(v[197]==TERRAIN_PSYCHIC && attacker_ground && m->type==T_PSY) {
        mods=damage_chain(mods,5325); if(id==MV_EF) mods=damage_chain(mods,6144);
    }
    if(id==MV_FAC && (outgoing?v[182]:v[189]) && !(outgoing?v[37]:v[72])) mods=damage_chain(mods,8192);
    power=item_mod(power,mods); if(power<1) power=1;
    int screen=v[(outgoing?206:202)+(phys?0:1)]>0;
    int weather=4096;
    if((v[195]==WEATHER_SUN && m->type==T_FIRE) || (v[195]==WEATHER_RAIN && m->type==T_WATER)) weather=6144;
    if((v[195]==WEATHER_SUN && m->type==T_WATER) || (v[195]==WEATHER_RAIN && m->type==T_FIRE)) weather=2048;
    int cs=MOVE_TRAITS[id].crit_bonus+(item==ITEM_LENS);
    static const double cpv[4]={1.0/24,1.0/8,.5,1}; double cp=cpv[cs>3?3:cs],total=0;
    // Equal public-rule hypotheses, as obsh2: 0/16/32 points x nature 0.9/1/1.1.
    // This is a prior, never stat inversion or selection by the actual build.
    for(int points=0;points<=32;points+=16) for(int nature=9;nature<=11;nature++) {
        int aa=outgoing?v[25+ai]:(v[17+ai]+20+points)*nature/10;
        int dd=outgoing?(v[17+di]+20+points)*nature/10:v[25+di];
        for(int crit=0;crit<2;crit++) {
            int attack=damage_stat(aa,crit && astage<0?0:astage),defense=damage_stat(dd,crit && dstage>0?0:dstage);
            if(di==4 && v[195]==WEATHER_SAND && (d1==T_ROCK || d2==T_ROCK)) defense=item_mod(defense,6144);
            int base=(int)(22LL*power*attack/defense/50)+2;
            int final=screen && !crit?2048:4096;
            if(item==ITEM_LIFEORB) final=damage_chain(final,5324);
            if(eff>1 && item==ITEM_BELT) final=damage_chain(final,4915);
            if(item==ITEM_METRO && v[outgoing?46:118]==id+1) {
                static const int met[6]={4096,4915,5734,6553,7372,8192};
                int streak=v[outgoing?47:119]; final=damage_chain(final,met[streak>5?5:streak]);
            }
            if(!defused && eff>1 && ((defitem==ITEM_SHUCA && m->type==T_GROUND) ||
               (defitem==ITEM_CHOPLE && m->type==T_FIGHT) || (defitem==ITEM_KASIB && m->type==T_GHOST) ||
               (defitem==ITEM_PAYAPA && m->type==T_PSY))) final=damage_chain(final,2048);
            DamageHooks hooks={weather,4096,burned && phys && id!=MV_FAC};
            double sum=0;
            for(int roll=85;roll<=100;roll++) sum+=damage_modifiers(base,crit,roll,m->type==t1 || m->type==t2,eff,&hooks,final);
            total+=(crit?cp:1-cp)*sum/16;
        }
    }
    double accuracy=m->acc>100?1:fmin(1,m->acc*acc_mult(v[outgoing?36:71])*(v[201]?6840.0/4096:1)/100);
    if(item==ITEM_WIDE) accuracy=fmin(1,accuracy*1.1);
    if(defitem==ITEM_POWDER) accuracy*=.9;
    double prior=total/9*accuracy;
    return outgoing?100*prior/(v[17]+75+16):prior;
}
static double h3_pressure(const Observation2 *o,int mv) {
    double prior=h3_damage(o,mv,1);
    // Blend only in the same unmodified public context. No hidden-stat fit.
    for(int i=0;i<4;i++) {
        int j=226+12*i;
        if(o->v[j] && o->v[j+1]==mv && o->v[j+8] && !o->v[182] && !o->v[195] && !o->v[197] && !o->v[206] && !o->v[207]) {
            int zero=1; for(int t=0;t<4;t++) if(o->v[31+t] || o->v[66+t]) zero=0;
            if(zero && MOVES[mv].power>0 && mv!=MV_TAN && mv!=MV_RND)
                return .5*prior+.5*(o->v[j+9]+o->v[j+10])/200.0;
        }
    }
    return prior;
}
static int obsh3(const Observation2 *o,const ActionMask *mask) {
    const int32_t *v=o->v; int first=obsdemo2(o,mask);
    if(first<0 || first==4) return first;
    int slot[V2_MOVE_COUNT]; for(int m=0;m<V2_MOVE_COUNT;m++) slot[m]=h3_slot(o,mask,m);
    if(v[37]) {
        if(slot[MV_PRO]>=0) return slot[MV_PRO];
        if(slot[MV_SN]>=0) return slot[MV_SN];
        if(slot[MV_ST]>=0) return slot[MV_ST];
        return first;
    }
    double hp=v[23],maxhp=v[24],frac=hp/maxhp,threat=0;
    int phys=0,spec=0,attacks=0;
    for(int i=0;i<4;i++) if(v[79+8*i]) {
        int m=v[80+8*i];
        if(MOVES[m].cat!=CAT_STATUS) {
            attacks++; if(MOVES[m].cat==CAT_PHYS) phys=1; else spec=1;
            threat=fmax(threat,h3_damage(o,m,0));
        }
        int j=274+12*i; if(v[j] && v[j+2]) threat=fmax(threat,v[j+4]*maxhp/10000);
    }
    if(!attacks) threat=.25*maxhp;
    int best=-1; double score=-1,besthit=0;
    for(int i=0;i<4;i++) if(mask->legal[i]) {
        int m=v[49+4*i]; if(MOVES[m].cat==CAT_STATUS) continue;
        double d=h3_pressure(o,m),s=d;
        if(m==MV_FS) s=v[213]?0:d*.6;
        if(m==MV_MB && !v[188]) s*=.6;
        if(m==MV_GI || m==MV_HB) s*=.65;
        if(m==MV_SD || m==MV_EXP) s-=100; // self-faint loses simultaneous KO
        if(m==MV_BEAM) s-=50;
        if(m==MV_DE) s-=d*.33;
        if(MOVES[m].contact && v[111] && v[112]==ITEM_HELMET) s-=100.0/6;
        if(m==MV_PN && !v[73] && !v[74] && h3_revealed(o,MV_REST)) s+=12;
        if((m==MV_BB || m==MV_PF) && (v[206] || v[207])) s+=10;
        if(m==MV_TP && !v[189]) s+=3;
        if(m==MV_IP && !v[189] && v[195]!=WEATHER_SUN) s+=3;
        if(m==MV_FP && attacks) s*=.4;
        if(s>score) { score=s; best=i; besthit=d; }
    }
    int second=v[150] && v[142]==2;
    if(best>=0 && !v[74] && besthit>=v[64]+2 && (!second || hp>threat*1.15) &&
       v[49+4*best]!=MV_FS && v[49+4*best]!=MV_SD && v[49+4*best]!=MV_EXP &&
       (v[49+4*best]!=MV_MB || v[188])) return best;
    if(slot[MV_REST]>=0 && v[38]<=1 && hp<maxhp && (frac<=.42 || (frac<.78 && hp<=threat*1.65))) return slot[MV_REST];
    if(slot[MV_PU]>=0) {
        int benefit=0; for(int i=0;i<5;i++) benefit+=v[66+i]-v[31+i];
        if(benefit>=3 && hp>threat*1.25) return slot[MV_PU];
    }
    if(hp>threat*1.6 && frac>.55) {
        if(slot[MV_REF]>=0 && !v[202] && (phys || !attacks)) return slot[MV_REF];
        if(slot[MV_LS]>=0 && !v[203] && spec) return slot[MV_LS];
        if(slot[MV_MR]>=0 && !v[205] && !v[201] && v[42]!=ITEM_IRONBALL &&
           (h3_revealed(o,MV_EQ) || h3_revealed(o,MV_TAN))) return slot[MV_MR];
        if(slot[MV_CP]>=0 && ((v[32]<2 && (phys || !attacks || slot[MV_BP]>=0)) || (v[34]<2 && spec))) return slot[MV_CP];
        if(slot[MV_ID]>=0 && v[32]<2 && (phys || !attacks || slot[MV_BP]>=0)) return slot[MV_ID];
        if(slot[MV_AGI]>=0 && v[35]<=0 && v[150] && v[151]==0 && v[152]>=2) return slot[MV_AGI];
    }
    if(slot[MV_PRO]>=0 && !v[40] && ((v[42]==ITEM_LEFTOVERS && frac<.9 && hp>threat) || (v[38]>1 && slot[MV_REST]>=0))) return slot[MV_PRO];
    if(slot[MV_SUB]>=0 && !v[39] && frac>=.8 && (v[72] || h3_revealed(o,MV_SWG))) return slot[MV_SUB];
    if(best>=0) return best;
    if(slot[MV_REST]>=0 && v[38]<=1 && hp<maxhp) return slot[MV_REST];
    if(slot[MV_SUN]>=0 && !v[195] && h3_revealed(o,MV_IP)) return slot[MV_SUN];
    if(slot[MV_SWG]>=0 && !v[193] && !v[74]) return slot[MV_SWG];
    if(slot[MV_SR]>=0 && !v[208]) return slot[MV_SR];
    if(slot[MV_CP]>=0 && (v[32]<6 || v[34]<6)) return slot[MV_CP];
    if(slot[MV_ID]>=0 && v[32]<6) return slot[MV_ID];
    return first;
}

// ---------- Stage 8e-3: obsh4 ----------
enum { H4_SELF_SLOTS=76, H4_SELF_VOL=92, H4_FOE_VOL=242, H4_RECORD_WORDS=26,
       H4_ABILITIES=912, H4_ITEMS=920, H4_MOVES=924, H4_STATS=944 };

typedef struct {
    int type[3], stat[6][3], rank[7];
    int ability, item, item_id, form, species, status, sleep, sub, block, rise, last, streak, attempt;
    double hp;
} H4Side;

typedef struct {
    const int32_t *v;
    H4Side mon[2];
    int weather, terrain, gravity, trick, magic, wonder;
    int screen[2][3], tailwind[2];
} H4Position;

typedef struct { double mean, hit, ko; } H4Damage;
typedef struct { H4Damage damage; int move, categories; } H4Threat;

static int h4_bit(const int32_t *words,int bit) {
    return ((uint32_t)words[bit/32]>>(bit%32))&1;
}
static int h4_singleton(const int32_t *words,int count) {
    int found=-1;
    for(int i=0;i<count;i++) if(h4_bit(words,i)) {
        if(found>=0) return -1;
        found=i;
    }
    return found;
}
static int h4_duration(const int32_t *v,int id) {
    return v[O3_FIELD+2+5*id];
}
static const int32_t *h4_record(const int32_t *v,int side,int index) {
    return v+O3_RECORDS+(side*OBS3_RECORDS+index)*H4_RECORD_WORDS;
}
static int h4_revealed(const H4Position *p,int move) {
    for(int i=0;i<OBS3_RECORDS;i++) {
        const int32_t *r=h4_record(p->v,1,i);
        if(r[0] && r[1]==move && (!r[7] || r[9]>0)) return 1;
    }
    return 0;
}
static int h4_ability_is(const H4Side *s,const char *key) {
    return s->ability>=0 && !strcmp(S8_ABILITIES[s->ability].id,key);
}
static int h4_item_is(const H4Side *s,int item) {
    return s->item==item;
}
static int h4_has_type(const H4Side *s,int type) {
    return s->type[0]==type || s->type[1]==type || s->type[2]==type;
}
static int h4_stat_base(int form,int stat,int up,int down,int points) {
    int value=(2*S8_SPECIES[form].base_stats[stat]+31)/2+(stat?5:60)+points;
    return value*(stat && stat==up?110:stat && stat==down?90:100)/100;
}
static int s8_item_projection(int table_id);

static H4Position h4_position(const Observation3 *o) {
    const int32_t *v=o->v;
    H4Position p={.v=v,.weather=v[O3_FIELD],.terrain=v[O3_FIELD+1],
        .gravity=h4_duration(v,O3_GRAVITY),.trick=h4_duration(v,O3_TRICK),
        .magic=h4_duration(v,O3_MAGIC),.wonder=h4_duration(v,O3_WONDER)};
    H4Side *a=&p.mon[0],*b=&p.mon[1];
    a->species=v[17];a->form=v[18];b->species=v[193];b->form=v[194];
    a->ability=v[27]?-1:v[26];
    b->ability=v[898]?-1:v[896]?v[897]:v[H4_ABILITIES]?h4_singleton(v+H4_ABILITIES+1,S8_ABILITIES_COUNT):-1;
    a->item=v[31]==2?-1:v[30];
    b->item=v[900]?(v[902]==2?-1:v[901]):-1;
    if(!v[900] && v[H4_ITEMS]) {
        int bit=h4_singleton(v+H4_ITEMS+1,S8_NON_STONE_ITEMS_COUNT+3);
        if(bit>=0 && bit<S8_NON_STONE_ITEMS_COUNT) b->item=bit;
        // Unrevealed stones have no effect in the current-form damage model.
    }
    for(int i=0;i<3;i++) { a->type[i]=v[19+i]; b->type[i]=v[195+i]; }
    for(int i=0;i<6;i++) {
        for(int j=0;j<3;j++) a->stat[i][j]=v[50+i];
        int low=v[H4_STATS]?v[H4_STATS+1+2*i]:h4_stat_base(b->form,i,0,i,0);
        int high=v[H4_STATS]?v[H4_STATS+2+2*i]:h4_stat_base(b->form,i,i,0,32);
        b->stat[i][0]=low;b->stat[i][1]=(low+high)/2;b->stat[i][2]=high;
    }
    a->hp=100.0*v[48]/fmax(1,v[49]);b->hp=v[224];
    a->status=v[63];b->status=v[233];
    a->sleep=v[64];b->sleep=(b->status==7); // MGO3 public sleep status
    a->sub=v[69]>0;b->sub=v[H4_FOE_VOL+2*O3_SUB];
    a->block=v[68];b->block=v[H4_FOE_VOL+2*O3_BLOCK];
    a->rise=v[H4_SELF_VOL+O3_RISE];b->rise=v[H4_FOE_VOL+2*O3_RISE];
    a->last=v[73];a->streak=v[74];a->attempt=v[75];
    b->last=v[239];b->streak=v[240];b->attempt=v[241];
    for(int i=0;i<7;i++) { a->rank[i]=v[56+i]; b->rank[i]=v[226+i]; }
    for(int side=0;side<2;side++) {
        H4Side *s=&p.mon[side];
        s->item_id=p.magic || s->ability==S8I_KLUTZ?-1:s->item;
        s->item=s8_item_projection(s->item_id);
        for(int i=0;i<3;i++) p.screen[side][i]=h4_duration(v,O3_SIDE+side*7+i);
        p.tailwind[side]=h4_duration(v,O3_SIDE+side*7+4);
    }
    return p;
}
static int h4_grounded(const H4Position *p,const H4Side *s) {
    if(p->gravity || h4_item_is(s,ITEM_IRONBALL)) return 1;
    return !s->rise && !h4_has_type(s,T_FLY) && !h4_item_is(s,ITEM_BALLOON) && !h4_ability_is(s,"levitate");
}
static double h4_speed(const H4Position *p,int side,int sample) {
    const H4Side *s=&p->mon[side];
    double speed=s->stat[5][sample]*smult(s->rank[4]);
    if(h4_item_is(s,ITEM_SCARF)) speed*=1.5;
    if(h4_item_is(s,ITEM_IRONBALL)) speed*=.5;
    if(s->status==STATUS_PAR && s->ability!=S8V3_QUICKFEET) speed=floor(speed)/2;
    if(s->status && s->ability==S8V3_QUICKFEET) speed*=1.5;
    if(p->tailwind[side]) speed*=2;
    return p->trick?-speed:speed;
}
static double h4_first(const H4Position *p,int move,int response) {
    if(response<0) return 1;
    int priority=S8_MOVES[move].priority-S8_MOVES[response].priority;
    if(priority) return priority>0;
    double speed=h4_speed(p,0,1), first=0;
    for(int i=0;i<3;i++) {
        double other=h4_speed(p,1,i);
        first+=speed>other?1:speed==other?.5:0;
    }
    return first/3;
}
static double h4_accuracy(const H4Position *p,int side,int move) {
    const H4Side *a=&p->mon[side],*b=&p->mon[1-side];
    int acc=S8_MOVES[move].accuracy;
    if(acc>100) return 1;
    double accuracy=acc*acc_mult(clamp_stage(a->rank[5]-b->rank[6]))/100;
    if(p->gravity) accuracy*=6840.0/4096;
    if(h4_item_is(a,ITEM_WIDE)) accuracy*=1.1;
    if(h4_item_is(b,ITEM_POWDER)) accuracy*=.9;
    return fmin(1,accuracy);
}
static int h4_weight(const H4Side *s) {
    int weight=S8_SPECIES[s->form].weight_hg;
    if(s->ability==S8TA_AB_LIGHTMETAL) weight/=2;
    if(s->ability==S8I_HEAVYMETAL) weight*=2;
    return weight>0?weight:1;
}
static int h4_power(const H4Position *p,int side,int move) {
    const H4Side *a=&p->mon[side],*b=&p->mon[1-side];
    int power=S8_MOVES[move].power;
    switch(move) {
    case MV_GK: case S8V_LOWKICK: return grass_knot_power(h4_weight(b));
    case MV_HP: return (int)fmax(1,b->hp);
    case MV_TAN: case S8V_TEMPERFLARE: {
        int failed=side?(a->attempt==PUBLIC_FAIL || a->attempt==PUBLIC_SKIP_FAIL):
                        (a->attempt==ATTEMPT_FAIL || a->attempt==ATTEMPT_SKIP_FAIL);
        return failed?power*2:power;
    }
    case MV_GYRO:
        return (int)fmin(150,1+25*fabs(h4_speed(p,1-side,1))/fmax(1,fabs(h4_speed(p,side,1))));
    case S8V_ERUPTION: case S8V_WATERSPOUT: return (int)fmax(1,power*a->hp/100);
    case S8V_FLAIL: case S8V_REVERSAL: {
        int ratio=(int)(48*a->hp/100);
        return ratio<2?200:ratio<5?150:ratio<10?100:ratio<17?80:ratio<33?40:20;
    }
    case S8V_HEATCRASH: case S8V_HEAVYSLAM: {
        int ratio=h4_weight(a)/h4_weight(b);
        return ratio>=5?120:ratio==4?100:ratio==3?80:ratio==2?60:40;
    }
    case S8V_HEX: case S8V_INFERNALPARADE: return b->status || b->sleep?power*2:power;
    case S8V_ACROBATICS: return a->item==ITEM_NONE && (side==0 || p->v[900])?power*2:power;
    case S8V_POWERTRIP: case S8V_STOREDPOWER:
        for(int i=0;i<7;i++) if(a->rank[i]>0) power+=20*a->rank[i];
        return power;
    case S8V_ELECTROBALL: {
        static const int powers[]={40,60,80,120,150};
        int ratio=(int)(fabs(h4_speed(p,side,1))/fmax(1,fabs(h4_speed(p,1-side,1))));
        return powers[ratio>4?4:ratio];
    }
    case S8V_BEATUP: return 5+S8_SPECIES[a->species].base_stats[1]/10;
    default: return power;
    }
}
static double h4_effectiveness(const H4Position *p,int side,const S8Move *m) {
    const H4Side *a=&p->mon[side],*b=&p->mon[1-side];
    int type=m->type, ab=b->ability;
    if(m->flags&S8_FLAG_SOUND && ab==S8R_SOUNDPROOF) return 0;
    if(m->flags&S8_FLAG_BULLET && ab==S8R_BULLETPROOF) return 0;
    if(type==T_GROUND && (!h4_grounded(p,b) || ab==S8R_EARTHEATER)) return 0;
    if(type==T_FIRE && ab==S8R_FLASHFIRE) return 0;
    if(type==T_WATER && (ab==S8R_WATERABSORB || ab==S8R_DRYSKIN || h4_ability_is(b,"stormdrain"))) return 0;
    if(type==T_ELEC && (ab==S8R_LIGHTNINGROD || ab==S8R_VOLTABSORB || ab==S8R_MOTORDRIVE)) return 0;
    if(type==T_GRASS && ab==S8R_SAPSIPPER) return 0;
    if(m->priority>0 && p->terrain==TERRAIN_PSYCHIC && h4_grounded(p,b)) return 0;
    double eff=1;
    for(int i=0;i<3;i++) {
        int def=b->type[i];
        if((type==T_GROUND && def==T_FLY) ||
           ((type==T_NORMAL || type==T_FIGHT) && def==T_GHOST && h4_ability_is(a,"scrappy"))) continue;
        eff*=eff_vs(type,def);
    }
    return eff;
}
static H4Damage h4_damage(const H4Position *p,int side,int move) {
    H4Damage out={0};
    const S8Move *m=&S8_MOVES[move];
    const H4Side *a=&p->mon[side],*b=&p->mon[1-side];
    if(m->category==CAT_STATUS || (move==MV_ROLL && !p->terrain) || (move==MV_SN && !a->sleep)) return out;
    double eff=move==S8_STRUGGLE?1:h4_effectiveness(p,side,m);
    if(eff<=0) return out;
    double accuracy=h4_accuracy(p,side,move);
    int power=h4_power(p,side,move);
    int phys=m->category==CAT_PHYS,ai=move==MV_BP?2:phys?1:3,di=phys?2:4;
    if(move<MV_COUNT && MOVE_TRAITS[move].defensive_stat) di=MOVE_TRAITS[move].defensive_stat;
    int ds=p->wonder?(di==2?4:2):di;
    int guards=p->screen[1-side][phys?0:1] || p->screen[1-side][2];
    int weather=p->weather, type=m->type, power_mod=4096, final=4096;
    if(a->ability==S8W_CLOUDNINE || b->ability==S8W_CLOUDNINE) weather=WEATHER_NONE;
    if(a->ability==S8TA_AB_TOUGHCLAWS && m->contact) power_mod=damage_chain(power_mod,5325);
    if(h4_item_is(a,phys?ITEM_MUSCLE:ITEM_WISE)) power_mod=damage_chain(power_mod,4505);
    for(size_t i=0;i<sizeof(S8_TYPE_BOOST_ITEMS)/sizeof(*S8_TYPE_BOOST_ITEMS);i++)
        if(S8_TYPE_BOOST_ITEMS[i].type==type && h4_item_is(a,S8_TYPE_BOOST_ITEMS[i].item))
            power_mod=damage_chain(power_mod,4915);
    if(h4_grounded(p,a)) {
        if((p->terrain==TERRAIN_PSYCHIC && type==T_PSY) || (p->terrain==TERRAIN_GRASSY && type==T_GRASS) ||
           (p->terrain==TERRAIN_ELECTRIC && type==T_ELEC)) power_mod=damage_chain(power_mod,5325);
        if(move==MV_EF && p->terrain==TERRAIN_PSYCHIC) power_mod=damage_chain(power_mod,6144);
    }
    if(h4_grounded(p,b) && ((p->terrain==TERRAIN_MISTY && type==T_DRAGON) ||
       (p->terrain==TERRAIN_GRASSY && (move==MV_EQ || move==MV_BD)))) power_mod=damage_chain(power_mod,2048);
    if(move==MV_FAC && a->status && !b->sub) power_mod=damage_chain(power_mod,8192);
    power=item_mod(power,power_mod);
    if(h4_item_is(a,ITEM_LIFEORB)) final=damage_chain(final,5324);
    if(eff>1 && h4_item_is(a,ITEM_BELT)) final=damage_chain(final,4915);
    int resist=(eff>1 && ((b->item==ITEM_SHUCA && type==T_GROUND) ||
        (b->item==ITEM_CHOPLE && type==T_FIGHT) || (b->item==ITEM_KASIB && type==T_GHOST) ||
        (b->item==ITEM_PAYAPA && type==T_PSY)));
    for(size_t i=0;i<sizeof(S8_RESIST_BERRIES)/sizeof(*S8_RESIST_BERRIES);i++) {
        const S8ResistBerry *row=&S8_RESIST_BERRIES[i];
        if(row->item==b->item && row->type==type && (eff>1 || !row->super_effective)) resist=1;
    }
    if(resist && !b->block) final=damage_chain(final,2048);
    if((b->ability==S8R_FILTER || b->ability==S8R_SOLIDROCK) && eff>1) final=damage_chain(final,3072);
    if(b->ability==S8R_MULTISCALE && b->hp==100) final=damage_chain(final,2048);
    if(h4_item_is(a,ITEM_METRO) && a->last==move+1) {
        static const int met[]={4096,4915,5734,6553,7372,8192};
        final=damage_chain(final,met[a->streak>5?5:a->streak]);
    }
    int wm=4096;
    if((weather==WEATHER_SUN && type==T_FIRE) || (weather==WEATHER_RAIN && type==T_WATER)) wm=6144;
    if((weather==WEATHER_SUN && type==T_WATER) || (weather==WEATHER_RAIN && type==T_FIRE)) wm=2048;
    int stab=move!=S8_STRUGGLE && h4_has_type(a,type)?(h4_ability_is(a,"adaptability")?8192:6144):0;
    int cs=(m->crit_ratio>0?m->crit_ratio-1:0)+h4_item_is(a,ITEM_LENS);
    static const double rates[]={1.0/24,1.0/8,.5,1};
    double cp=m->will_crit?1:rates[cs>3?3:cs];
    int burned=phys && a->status==STATUS_BRN && move!=MV_FAC && a->ability!=S8P_GUTS;
    double hits=move==S8V_WATERSHURIKEN?3.1:1;
    if(move>=MV_COUNT) for(size_t i=0;i<sizeof(S8_MULTIHITS)/sizeof(*S8_MULTIHITS);i++) {
        const S8Multihit *multi=&S8_MULTIHITS[i];
        if(multi->move==move) hits=multi->min_hits==2 && multi->max_hits==5?3.1:multi->min_hits;
    }
    if(move==S8V_TRIPLEAXEL) hits=1+2*accuracy+3*accuracy*accuracy;
    out.hit=accuracy;
    for(int sample=0;sample<3;sample++) {
        int maxhp=b->stat[0][sample];
        if(power<=0 || move==MV_END || s8_ohko(move)) {
            double dmg=0, rate=accuracy;
            switch(move) {
            case S8V_NIGHTSHADE: case S8V_SEISMICTOSS: dmg=5000.0/maxhp;break;
            case S8V_SUPERFANG: dmg=b->hp/2;break;
            case MV_END: case S8V_ENDEAVOR:
                dmg=fmax(0,b->hp-a->hp*a->stat[0][sample]/maxhp);break;
            case S8V_FINALGAMBIT: dmg=a->hp*a->stat[0][sample]/maxhp;break;
            default:
                if(s8_ohko(move) && (move!=S8V_SHEERCOLD || !h4_has_type(b,T_ICE))) {
                    dmg=b->hp;rate=move==S8V_SHEERCOLD && !h4_has_type(a,T_ICE)?.2:.3;
                }
                break;
            }
            out.mean+=rate*dmg/3;
            if(dmg>=b->hp) out.ko+=rate/3;
            continue;
        }
        for(int crit=0;crit<2;crit++) {
            double chance=accuracy*(crit?cp:1-cp)/48;
            if(chance==0) continue;
            int ar=a->rank[ai-1], dr=b->rank[ds-1];
            if(crit && ar<0) ar=0;
            if(crit && dr>0) dr=0;
            int attack=damage_stat(a->stat[ai][sample],ar), defense=damage_stat(b->stat[ds][sample],dr);
            if(phys && a->status && a->ability==S8P_GUTS) attack=item_mod(attack,6144);
            if((di==2 && weather==WEATHER_SNOW && h4_has_type(b,T_ICE)) ||
               (di==4 && weather==WEATHER_SAND && h4_has_type(b,T_ROCK))) defense=item_mod(defense,6144);
            int base=(int)(22LL*power*attack/defense/50)+2;
            DamageHooks hooks={wm,4096,burned};
            int modifier=guards && !crit && move!=MV_BB && move!=MV_PF?damage_chain(final,2048):final;
            for(int roll=85;roll<=100;roll++) {
                double dmg=100.0*hits*damage_modifiers(base,crit,roll,stab,eff,&hooks,modifier)/maxhp;
                out.mean+=chance*dmg;
                if(dmg>=b->hp) out.ko+=chance;
            }
        }
    }
    if(b->sub && !(m->flags&S8_FLAG_BYPASSSUB) && !h4_ability_is(a,"infiltrator")) out.ko=0;
    if(b->hp==100 && (h4_item_is(b,ITEM_SASH) || h4_ability_is(b,"sturdy")) && hits==1) out.ko=0;
    return out;
}
static int h4_delay(const H4Position *p,int side,int move) {
    int charging=side?p->v[H4_FOE_VOL+2*O3_CHARGE]:p->v[H4_SELF_VOL+O3_CHARGE];
    if(move==MV_FS) return 2;
    if(move==MV_MB) return !charging;
    const S8TurnMove *turn=s8_turn_move(move);
    return turn && turn->kind==S8U_CHARGE && !charging &&
           (!turn->skip_weather || turn->skip_weather!=p->weather);
}

// Revealed threats take precedence. Before an attack is known, the mean of
// compatible learnset attacks supplies pressure without assuming a moveset.
static H4Threat h4_threat(const H4Position *p) {
    H4Threat danger={.move=-1};
    for(int i=0;i<OBS3_RECORDS;i++) {
        const int32_t *r=h4_record(p->v,1,i);
        if(!r[0] || (r[7] && r[9]<=0) || S8_MOVES[r[1]].category==CAT_STATUS) continue;
        int move=r[1];
        H4Damage d=h4_damage(p,1,move);
        if(h4_delay(p,1,move)) d.ko=0;
        if(d.hit>0) danger.categories|=1<<S8_MOVES[move].category;
        if(d.mean>danger.damage.mean) { danger.damage=d;danger.move=move; }
    }
    if(danger.move>=0 || !p->v[H4_MOVES]) return danger;
    int count=0;
    const S8Species *species=&S8_SPECIES[p->v[203]?p->mon[1].form:p->mon[1].species];
    for(int i=0;i<species->move_count;i++) {
        int move=S8_LEARNSETS[species->move_offset+i];
        if(!h4_bit(p->v+H4_MOVES+1,move) || S8_MOVES[move].category==CAT_STATUS) continue;
        H4Damage d=h4_damage(p,1,move);
        danger.damage.mean+=d.mean;
        danger.damage.ko+=h4_delay(p,1,move)?0:d.ko;
        count++;
        if(d.mean>0 && (danger.move<0 || S8_MOVES[move].priority>S8_MOVES[danger.move].priority))
            danger.move=move;
    }
    if(count) { danger.damage.mean/=count;danger.damage.ko/=count; }
    return danger;
}

// Only model the immediate reduction in the next attack. Use confirmed
// blockers and public candidate singletons from the same MGO3 view.
static double h4_attack_drop(const H4Position *p,int move,H4Damage hit,H4Threat threat) {
    const S8StatEffect *effect=s8_stat_effect(move);
    if(!effect || effect->mode!=S8_STAT_SECONDARY || effect->self ||
       (effect->delta[0]>=0 && effect->delta[2]>=0)) return 0;
    for(int i=0;i<6;i++)
        if(effect->delta[i]>0 || (i!=0 && i!=2 && effect->delta[i])) return 0;
    const H4Side *a=&p->mon[0],*b=&p->mon[1];
    double chance=fmax(0,hit.hit-hit.ko)*effect->chance/100;
    if(chance<=0 || threat.damage.mean<=0 || a->ability==S8D_SHEERFORCE ||
       b->ability==S8G_SHIELDDUST || b->ability==S8Q_MIRRORARMOR ||
       (b->ability==S8G_FLOWERVEIL && h4_has_type(b,T_GRASS)) ||
       (b->sub && !(S8_MOVES[move].flags&S8_FLAG_BYPASSSUB) &&
        a->ability!=S8D_INFILTRATOR)) return 0;
    if(b->item_id>=0) {
        const char *item=S8_ITEMS[b->item_id].id;
        if(h4_item_is(b,ITEM_HERB) || !strcmp(item,"clearamulet") ||
           !strcmp(item,"covertcloak")) return 0;
    }
    const S8BoostAbility *ability=s8_boost_ability(b->ability);
    if(ability && (ability->change<0 || ability->react_stat>=0)) return 0;
    H4Position after=*p;
    for(int i=0;i<=2;i+=2) {
        if(ability && (ability->guard_stat==7 || ability->guard_stat==i)) continue;
        int multiplier=ability && ability->change>0?ability->change:1;
        after.mon[1].rank[i]=clamp_stage(b->rank[i]+effect->delta[i]*multiplier);
    }
    if(after.mon[1].rank[0]==b->rank[0] && after.mon[1].rank[2]==b->rank[2]) return 0;
    double before=fmin(a->hp,threat.damage.mean);
    double reduced=fmin(a->hp,h4_threat(&after).damage.mean);
    return chance*fmax(0,before-reduced);
}

static double h4_status_value(const H4Position *p,int move,int status,int sleep,int confusion,
                              H4Threat threat) {
    const H4Side *b=&p->mon[1];
    const S8Move *m=&S8_MOVES[move];
    if((b->sub && !(m->flags&S8_FLAG_BYPASSSUB) && !h4_ability_is(&p->mon[0],"infiltrator")) ||
       h4_duration(p->v,O3_SIDE+7+3) || (p->terrain==TERRAIN_MISTY && h4_grounded(p,b))) return 0;
    if(confusion) return p->v[236]?0:threat.damage.mean/3;
    if(b->status || b->sleep || h4_item_is(b,ITEM_LUM) || b->ability==S8R_GOODASGOLD) return 0;
    if(sleep) {
        if(b->ability==S8TA_AB_INSOMNIA || h4_ability_is(b,"vitalspirit") || h4_item_is(b,ITEM_CHESTO) ||
           (p->terrain==TERRAIN_ELECTRIC && h4_grounded(p,b))) return 0;
        return threat.damage.mean;
    }
    if(status==STATUS_PAR) return h4_has_type(b,T_ELEC) || h4_ability_is(b,"limber")?0:threat.damage.mean/8;
    if(status==STATUS_FRZ) return h4_has_type(b,T_ICE) || p->weather==WEATHER_SUN?0:threat.damage.mean;
    if(status==STATUS_BRN && !h4_has_type(b,T_FIRE) && b->ability!=S8P_GUTS)
        return (b->ability==S8R_MAGICGUARD?0:100.0/16)+(threat.categories==1?threat.damage.mean/2:0);
    if((status==STATUS_PSN || status==STATUS_TOX) && !h4_has_type(b,T_POISON) &&
       !h4_has_type(b,T_STEEL) && b->ability!=S8R_MAGICGUARD) return 100.0/(status==STATUS_TOX?16:8);
    return 0;
}

static double h4_attack_score(const H4Position *p,int move,H4Damage d,H4Threat threat) {
    const S8Move *m=&S8_MOVES[move];
    const H4Side *a=&p->mon[0],*b=&p->mon[1];
    if(d.hit==0 || move==MV_SD || move==MV_EXP || move==S8V_FINALGAMBIT || move==S8V_MISTYEXPLOSION) return -100;
    if(move==MV_FS && h4_duration(p->v,O3_SIDE+7+6)) return -100;
    double score=d.mean;
    if(h4_delay(p,0,move)) score/=2;
    if(m->flags&S8_FLAG_RECHARGE) score*=.5+.5*d.ko;
    if(move==MV_FP) score*=threat.categories?1-threat.damage.hit:1;
    if(move==MV_BEAM) score-=50*d.hit;
    if(m->recoil_num && a->ability!=S8R_ROCKHEAD && a->ability!=S8R_MAGICGUARD)
        score-=fmin(b->hp,d.mean)*b->stat[0][1]/a->stat[0][1]*m->recoil_num/m->recoil_den;
    if(m->contact && h4_item_is(b,ITEM_HELMET)) score-=100*d.hit/6;
    if(h4_item_is(a,ITEM_LIFEORB) && a->ability!=S8R_MAGICGUARD) score-=10*d.hit;
    const S8HealMove *heal=s8_heal_move(move);
    if(heal && heal->kind==S8H_DRAIN && !a->block)
        score+=fmin(100-a->hp,d.mean*b->stat[0][1]/a->stat[0][1]*heal->num/heal->den);
    const S8StatusMove *secondary=s8_status_move(move);
    if(secondary) for(int i=0;i<secondary->count;i++) {
        const S8ExtraEffect *e=&secondary->effects[i];
        if(e->kind==S8_EXTRA_STATUS || e->kind==S8_EXTRA_CONFUSION)
            score+=d.hit*e->chance/100*h4_status_value(p,move,e->status,0,e->kind==S8_EXTRA_CONFUSION,threat);
        if(e->kind==S8_EXTRA_FLINCH && !h4_ability_is(b,"innerfocus"))
            score+=d.hit*e->chance/100*threat.damage.mean*h4_first(p,move,threat.move);
    }
    if(move==MV_IP || move==MV_TP)
        score+=.1*d.hit*h4_status_value(p,move,move==MV_IP?STATUS_FRZ:STATUS_PAR,0,0,threat);
    if(move==MV_PN && !b->block && (!b->sub || (m->flags&S8_FLAG_BYPASSSUB)) && h4_revealed(p,MV_REST))
        score+=fmin(100-b->hp,threat.damage.mean);
    return score;
}

// Compare HP exchanged, including the chance that a faster knockout denies
// the slower action. The same calculation is used on both Mega hypotheses.
static double h4_exchange(const H4Position *p,int move,H4Damage d,H4Threat threat) {
    const H4Side *a=&p->mon[0],*b=&p->mon[1];
    double first=h4_first(p,move,threat.move);
    double taken=fmin(a->hp,threat.damage.mean)*(1-first*d.ko);
    double dealt=fmin(b->hp,d.mean)*(1-(1-first)*threat.damage.ko);
    return dealt-taken+d.ko*100*(1-(1-first)*threat.damage.ko)-100*threat.damage.ko*(1-first*d.ko);
}
enum { OBSH4_STAT_DROP=1, OBSH4_SUBSTITUTE=2, OBSH4_SETUP_RACE=4, OBSH4_SETUP_TEMPO=8,
       OBSH4_OPENING=16,
       OBSH4_SETUP_BASELINE=OBSH4_SUBSTITUTE, OBSH4_DEFAULT=OBSH4_SETUP_BASELINE };

static int h4_race_move(const H4Position *p,int side,int move) {
    const S8Move *m=&S8_MOVES[move];
    return m->power>0 && !m->recoil_num && !(m->flags&S8_FLAG_RECHARGE) &&
        move!=MV_SD && move!=MV_EXP && move!=MV_BEAM && move!=S8V_MISTYEXPLOSION && !s8_ohko(move) &&
        !h4_delay(p,side,move);
}

// Compare five turns of sustained pressure with and without spending the
// first turn on a boost. Wins score their remaining turns plus one.
static int h4_race(const H4Position *p,const H4Position *after,int setup,int attack,
                   int next,int reply,const double damage[4],int cautious) {
    double own=p->mon[0].hp, foe=p->mon[1].hp;
    double own_heal=h4_item_is(&p->mon[0],ITEM_LEFTOVERS) && !p->mon[0].block?6.25:0;
    double foe_heal=h4_item_is(&p->mon[1],ITEM_LEFTOVERS) && !p->mon[1].block?6.25:0;
    for(int t=0;t<5;t++) {
        int boosting=setup>=0 && !t;
        int boosted=setup>=0 && t;
        const H4Position *state=boosted?after:p;
        int move=boosting?setup:boosted?next:attack;
        double first=h4_first(state,move,reply);
        int acts_first=cautious?first>.5:first>=.5;
        double dealt=boosting?0:damage[boosted?1:0];
        double taken=damage[boosted || (boosting && acts_first)?3:2]*(cautious?1.08:1);
        if(acts_first) {
            if((foe-=dealt)<=0) return 5-t;
            if((own-=taken)<=0) return -1;
        } else {
            if((own-=taken)<=0) return -1;
            if((foe-=dealt)<=0) return 5-t;
        }
        own=fmin(100,own+own_heal);
        foe=fmin(100,foe+foe_heal);
    }
    return 0;
}

static int h4_setup_race(const H4Position *p,const H4Position *after,const ActionMask *mask,
                         int move,int attack,const H4Damage *hit,const H4Threat *threat,
                         const H4Threat *after_threat,int prefer_faster) {
    switch(move) {
    case S8B_CALMMIND: case S8B_SWORDSDANCE: case S8B_DRAGONDANCE: case S8B_BULKUP:
    case S8B_NASTYPLOT: case S8B_QUIVERDANCE: break;
    default: return 0;
    }
    const H4Side *a=&p->mon[0],*b=&p->mon[1];
    if(a->sub || b->sub || a->status || b->status || p->weather==WEATHER_SAND ||
       a->ability==S8Q_CONTRARY || a->ability==S8Q_SIMPLE || h4_ability_is(b,"unaware") ||
       threat->move<0 || after_threat->move!=threat->move ||
       !h4_revealed(p,threat->move) || threat->damage.mean<=0 ||
       !h4_race_move(p,1,threat->move) || !h4_race_move(p,0,attack) ||
       !memcmp(a->rank,after->mon[0].rank,sizeof a->rank)) return 0;
    int base_slot=-1,next=-1;
    double best=0;
    for(int i=0;i<4;i++) if(mask->legal[i]) {
        const int32_t *slot=p->v+H4_SELF_SLOTS+4*i;
        if(slot[1]==attack) base_slot=i;
        if(slot[2]<4 || !h4_race_move(after,0,slot[1])) continue;
        double damage=h4_damage(after,0,slot[1]).mean;
        if(damage>best) { best=damage;next=slot[1]; }
    }
    if(base_slot<0 || p->v[H4_SELF_SLOTS+4*base_slot+2]<5 || next<0) return 0;
    double damage[]={hit->mean,best,threat->damage.mean,after_threat->damage.mean};
    int direction=0;
    for(int cautious=0;cautious<2;cautious++) {
        int now=h4_race(p,p,-1,attack,attack,threat->move,damage,cautious);
        int boosted=h4_race(p,after,move,attack,next,threat->move,damage,cautious);
        int comparison=0;
        if(boosted>0 && (now<=0 || (prefer_faster && boosted>now))) comparison=1;
        if(now>0 && (boosted<=0 || (prefer_faster && now>boosted))) comparison=-1;
        if(cautious && comparison!=direction) return 0;
        direction=comparison;
    }
    return direction;
}

static double h4_setup(const H4Position *p,int move,int attack,const H4Damage *hit,const H4Threat *incoming,
                       const ActionMask *mask,int options,double attack_score) {
    H4Threat threat=*incoming;
    const H4Side *a=&p->mon[0],*b=&p->mon[1];
    H4Position after=*p;
    const S8BoostMove *boost=s8_boost_move(move);
    if(move==MV_PU) {
        int benefit=0;
        for(int i=0;i<5;i++) benefit+=b->rank[i]-a->rank[i];
        if(benefit<3 || a->hp<=threat.damage.mean*1.25) return 0;
        memcpy(after.mon[0].rank,b->rank,sizeof(b->rank));
    } else if(boost && boost->target<=1) {
        if(boost->target==1 && (b->sub || b->ability==S8TA_AB_CLEARBODY || h4_ability_is(b,"whitesmoke"))) return 0;
        for(int i=0;i<7;i++) after.mon[boost->target].rank[i]=clamp_stage(after.mon[boost->target].rank[i]+boost->delta[i]);
    } else {
        switch(move) {
        case MV_CP:
            after.mon[0].rank[3]=clamp_stage(a->rank[3]+1);
            after.mon[0].rank[1]=clamp_stage(a->rank[1]+1);break;
        case MV_ID: after.mon[0].rank[1]=clamp_stage(a->rank[1]+2);break;
        case MV_AGI: after.mon[0].rank[4]=clamp_stage(a->rank[4]+2);break;
        case MV_REF: after.screen[0][0]=1;break;
        case MV_LS: after.screen[0][1]=1;break;
        case MV_MR: after.mon[0].rise=1;break;
        case S8F_AURORAVEIL: if(p->weather!=WEATHER_SNOW) return 0;after.screen[0][2]=1;break;
        case S8F_TAILWIND: after.tailwind[0]=1;break;
        case MV_SUN: after.weather=WEATHER_SUN;break;
        case MV_RAIN: after.weather=WEATHER_RAIN;break;
        case MV_SAND: after.weather=WEATHER_SAND;break;
        default: {
            const S8FieldMove *field=s8_field_move(move);
            if(!field || field->kind!=S8F_TERRAIN) return 0;
            after.terrain=field->value;break;
        }}
    }
    if(attack<0) {
        double rank_gain=0;
        for(int i=0;i<7;i++) rank_gain+=after.mon[0].rank[i]-a->rank[i]+b->rank[i]-after.mon[1].rank[i];
        return rank_gain;
    }
    H4Threat reply=h4_threat(&after);
    if(options&OBSH4_SETUP_RACE) {
        int direction=h4_setup_race(p,&after,mask,move,attack,hit,&threat,&reply,
                                    options&OBSH4_SETUP_TEMPO);
        if(direction>0) return attack_score+1;
        if(direction<0) return 0;
    }
    H4Damage improved=h4_damage(&after,0,attack);
    double gain=h4_exchange(&after,attack,improved,reply)-h4_exchange(p,attack,*hit,threat);
    // A setup turn costs one attack. Future value can recur while both sides
    // have enough HP to survive, but immediate knockouts take precedence.
    double rounds=fmin(a->hp/fmax(1,reply.damage.mean),b->hp/fmax(1,improved.mean));
    return gain*fmax(0,rounds-1);
}
static int h4_slot(const H4Position *p,const ActionMask *mask,int move) {
    for(int i=0;i<4;i++) if(mask->legal[i] && p->v[H4_SELF_SLOTS+4*i+1]==move) return i;
    return -1;
}

// Estimate value over two incoming hits; the hit that breaks Substitute
// does not spill excess damage onto its owner.
static double h4_substitute(const H4Position *p,H4Threat threat,double attack_score) {
    const H4Side *a=&p->mon[0],*b=&p->mon[1];
    const int32_t *v=p->v;
    int cost_hp=v[49]/4;
    if(a->sub || b->sub || a->status || v[H4_SELF_VOL+S8C_YAWN] ||
       cost_hp<=0 || v[48]<=cost_hp || threat.move<0 ||
       b->ability==S8D_INFILTRATOR || h4_ability_is(b,"parentalbond") ||
       !h4_revealed(p,threat.move)) return 0;
    double cost=100.0*cost_hp/v[49];
    if(a->hp-cost<50 || a->hp-cost<=threat.damage.mean) return 0;

    // A revealed sound attack or multi-hit attack can get through; a
    // single projected hit is insufficient evidence of useful protection.
    for(int i=0;i<OBS3_RECORDS;i++) {
        const int32_t *r=h4_record(v,1,i);
        if(!r[0] || (r[7] && r[9]<=0) || S8_MOVES[r[1]].category==CAT_STATUS) continue;
        int move=r[1];
        if((S8_MOVES[move].flags&(S8_FLAG_BYPASSSUB|S8_FLAG_SOUND)) ||
           move==S8V_WATERSHURIKEN || move==S8V_TRIPLEAXEL) return 0;
        for(size_t j=0;j<sizeof S8_MULTIHITS/sizeof S8_MULTIHITS[0];j++)
            if(S8_MULTIHITS[j].move==move && S8_MULTIHITS[j].max_hits>1) return 0;
    }
    double hit=threat.damage.hit;
    if(hit<=0 || threat.damage.mean>=cost*hit) return 0;
    double value=2*threat.damage.mean-cost;
    if(value<=0) return 0;

    // Leftovers plus damage and an available Protect are evidence of a
    // recovery turn. A compatible but unseen move receives less weight.
    if(!v[O3_FOE_VISIBLE+14] && b->hp<90 && h4_item_is(b,ITEM_LEFTOVERS) && !b->block) {
        if(h4_revealed(p,MV_PRO)) value+=.65*attack_score;
        else if(v[H4_MOVES] && h4_bit(v+H4_MOVES+1,MV_PRO)) value+=.50*attack_score;
    }
    return value;
}

static int h4_choose(const H4Position *p,const ActionMask *mask,int options) {
    int stat_drop=options&OBSH4_STAT_DROP;
    int first=-1;
    for(int i=0;i<5;i++) if(mask->legal[i]) { first=i;break; }
    if(first<0 || first==4) return first;
    const H4Side *a=&p->mon[0],*b=&p->mon[1];
    if(a->sleep) {
        const int sleep_moves[]={MV_PRO,MV_SN,MV_ST};
        for(size_t i=0;i<sizeof(sleep_moves)/sizeof(*sleep_moves);i++) {
            int slot=h4_slot(p,mask,sleep_moves[i]);
            if(slot>=0) return slot;
        }
        return first;
    }
    H4Threat threat=h4_threat(p);
    H4Damage hit={0};
    int best=-1, attack=-1;
    int best_with_drop=-1;
    double score=0, score_with_drop=0;
    for(int i=0;i<4;i++) if(mask->legal[i]) {
        int move=p->v[H4_SELF_SLOTS+4*i+1];
        if(S8_MOVES[move].category==CAT_STATUS) continue;
        H4Damage d=h4_damage(p,0,move);
        double value=h4_attack_score(p,move,d,threat);
        double drop=0;
        if(value>=0 && !h4_delay(p,0,move)) {
            double first=h4_first(p,move,threat.move);
            value+=100*d.ko*(1-(1-first)*threat.damage.ko)+100*threat.damage.ko*first*d.ko;
            value-=(1-first)*threat.damage.ko*fmax(0,d.mean);
            if(stat_drop) drop=h4_attack_drop(p,move,d,threat);
        }
        if(value>score) { best=i;attack=move;score=value;hit=d; }
        if(stat_drop && value+drop>score_with_drop) { best_with_drop=i;score_with_drop=value+drop; }
    }
    if(best>=0 && !b->sub && hit.mean>=b->hp+2 && !h4_delay(p,0,attack) &&
       (h4_first(p,attack,threat.move)>=.5 || a->hp>threat.damage.mean*1.15)) return best;
    if(stat_drop) { best=best_with_drop;score=score_with_drop; }
    int rest=h4_slot(p,mask,MV_REST), protect=h4_slot(p,mask,MV_PRO);
    if(rest>=0 && a->block<=1 && a->hp<100 && (a->hp<=42 || (a->hp<78 && a->hp<=threat.damage.mean*1.65))) return rest;
    if(protect>=0 && !p->v[70] && ((h4_item_is(a,ITEM_LEFTOVERS) && a->hp<90 && a->hp>threat.damage.mean) ||
       (a->block>1 && rest>=0))) return protect;

    double attack_score=score;
    for(int i=0;i<4;i++) if(mask->legal[i]) {
        int move=p->v[H4_SELF_SLOTS+4*i+1];
        if(S8_MOVES[move].category!=CAT_STATUS) continue;
        double value=0;
        const S8PrimaryStatus *status=s8_primary_status(move);
        const S8HealMove *heal=s8_heal_move(move);
        if(status && !(status->powder && h4_has_type(b,T_GRASS)) &&
           (!status->type_immunity || h4_effectiveness(p,0,&S8_MOVES[move])>0))
            value=h4_accuracy(p,0,move)*h4_status_value(p,move,status->status,status->sleep,status->confusion,threat);
        if(heal && heal->kind==S8H_SIMPLE && !a->block && a->hp<100 &&
           (a->hp<=50 || a->hp<=threat.damage.mean*1.65))
            value=fmin(100-a->hp,100.0*heal->num/heal->den);
        if(move==MV_REST && a->block<=1 && a->hp<100 && attack<0) value=100-a->hp;
        if(move==MV_SWG) value=h4_accuracy(p,0,move)*h4_status_value(p,move,0,0,1,threat);
        if(move==MV_SUB && !a->sub && a->hp>=80 && (b->sleep || h4_revealed(p,MV_SWG)))
            value=fmax(0,threat.damage.mean);
        if(move==MV_SUB && (options&OBSH4_SUBSTITUTE))
            value=fmax(value,h4_substitute(p,threat,score));
        if(move==MV_SR && !p->v[474] && attack<0) value=1;
        if(a->hp>threat.damage.mean*1.6 && a->hp>55)
            value=fmax(value,h4_setup(p,move,attack,&hit,&threat,mask,options,attack_score));
        if(value>score) { best=i;score=value; }
    }
    return best>=0?best:first;
}
static H4Position h4_mega_position(const H4Position *p,int target) {
    H4Position after=*p;
    H4Side *a=&after.mon[0];
    const int32_t *v=p->v;
    const S8Species *form=&S8_SPECIES[target];
    for(int i=1;i<6;i++) for(int points=0;points<=32;points++) {
        if(h4_stat_base(a->form,i,v[23],v[24],points)==a->stat[i][1]) {
            int stat=h4_stat_base(target,i,v[23],v[24],points);
            for(int sample=0;sample<3;sample++) a->stat[i][sample]=stat;
            break;
        }
    }
    a->form=target;a->type[0]=form->types[0];a->type[1]=form->types[1];a->type[2]=-1;
    a->ability=form->ability_count?S8_SPECIES_ABILITIES[form->ability_offset]:-1;
    for(size_t i=0;i<sizeof(S8_FIELD_ABILITIES)/sizeof(*S8_FIELD_ABILITIES);i++) {
        const S8FieldAbility *entry=&S8_FIELD_ABILITIES[i];
        if(entry->ability!=a->ability || entry->event!=S8AE_START) continue;
        if(entry->weather) after.weather=entry->weather;
        if(entry->terrain) after.terrain=entry->terrain;
    }
    return after;
}
static double h4_mega_value(const H4Position *p,const ActionMask *mask) {
    H4Threat threat=h4_threat(p);
    double best=-INFINITY;
    for(int i=0;i<5;i++) if(mask->legal[i]) {
        int move=i==4?S8_STRUGGLE:p->v[H4_SELF_SLOTS+4*i+1];
        H4Damage d=h4_damage(p,0,move);
        double net=d.hit?h4_attack_score(p,move,d,threat):0;
        if(net<0 && d.hit && (move==MV_SD || move==MV_EXP ||
           move==S8V_FINALGAMBIT || move==S8V_MISTYEXPLOSION)) continue;
        if(h4_delay(p,0,move)) d.ko=0;
        double costs=net-d.mean;
        double value=h4_exchange(p,move,d,threat)+costs;
        if(value>best) best=value;
    }
    return best;
}
static int h4_should_mega(const H4Position *p,const ActionMask *mask,H4Position *after) {
    const int32_t *v=p->v;
    int target=v[34],item=v[30];
    if(v[2] || v[32] || !v[33] || v[35] || v[31]==2 ||
       item<0 || !S8_ITEMS[item].is_stone || target<0 || target>=S8_SPECIES_ROWS_COUNT) return 0;
    H4Position before=*p;
    // Mega comparisons use only confirmed/publicly unique opponent identities.
    if(!v[900]) before.mon[1].item=ITEM_NONE;
    *after=h4_mega_position(&before,target);
    return h4_mega_value(after,mask)>=h4_mega_value(&before,mask);
}
static int obsh4_mega(const Observation3 *o,const ActionMask *mask) {
    H4Position p=h4_position(o),after;
    return h4_should_mega(&p,mask,&after);
}
static void h4_apply_opening_boost(H4Position *p,int side,int move) {
    const S8BoostMove *boost=s8_boost_move(move);
    for(int stat=0;stat<7;stat++)
        p->mon[side].rank[stat]=clamp_stage(p->mon[side].rank[stat]+boost->delta[stat]);
}

static double h4_opening_pressure(const H4Position *start,int move,int reply,int boost,int own_setup,int enemy_setup,int tie) {
    H4Position p=*start;
    double hp[2]={p.mon[0].hp,p.mon[1].hp};
    for(int turn=0;turn<5;turn++) {
        int action[2]={!turn && own_setup?boost:move,!turn && enemy_setup?boost:reply};
        double first=h4_first(&p,action[0],action[1]);
        int starts=first>.5?0:first<.5?1:tie;
        for(int k=0;k<2;k++) {
            int side=starts^k;
            if(action[side]==boost) h4_apply_opening_boost(&p,side,boost);
            else hp[1-side]-=h4_damage(&p,side,action[side]).mean;
            if(hp[1-side]<=0) return side==0?1:0;
        }
        for(int side=0;side<2;side++) {
            double recovery=h4_item_is(&p.mon[side],ITEM_LEFTOVERS)?6.25:0;
            hp[side]=fmin(100,hp[side]+recovery);
            p.mon[side].hp=hp[side];
        }
    }
    return .5;
}

static int h4_opening_choice(const H4Position *position,const ActionMask *mask,int chosen) {
    const int boost=S8B_CALMMIND;
    H4Position p=*position;
    if(p.v[0]!=1 || chosen<0 || chosen>=4 || !p.v[H4_MOVES] ||
       p.mon[0].status || p.mon[1].status || p.mon[0].sub || p.mon[1].sub ||
       p.mon[0].ability==S8Q_CONTRARY || p.mon[0].ability==S8Q_SIMPLE ||
       h4_ability_is(&p.mon[1],"unaware")) return chosen;
    int slot=h4_slot(&p,mask,boost);
    int move=p.v[H4_SELF_SLOTS+4*chosen+1];
    const int item=p.mon[0].item_id;
    const char *item_name=item>=0?S8_ITEMS[item].id:"";
    if(h4_item_is(&p.mon[0],ITEM_SCARF) || !strcmp(item_name,"choiceband") ||
       !strcmp(item_name,"choicespecs")) return chosen;
    if(slot<0 || chosen==slot || S8_MOVES[move].category!=CAT_SPEC ||
       !h4_race_move(&p,0,move) || p.v[H4_SELF_SLOTS+4*chosen+2]<5) return chosen;
    int representative[3]={-1,-1,-1}, stab_special=-1;
    double largest[3]={0};
    double largest_stab=0;
    for(int reply=0;reply<S8_MOVES_COUNT;reply++) {
        int category=S8_MOVES[reply].category;
        if(category==CAT_STATUS || !h4_bit(p.v+H4_MOVES+1,reply) || !h4_race_move(&p,1,reply)) continue;
        H4Damage d=h4_damage(&p,1,reply);
        if(d.hit>=.8 && d.mean>largest[category]) {largest[category]=d.mean;representative[category]=reply;}
        if(category==CAT_SPEC && h4_has_type(&p.mon[1],S8_MOVES[reply].type) &&
           d.hit>=.8 && d.mean>largest_stab) {largest_stab=d.mean;stab_special=reply;}
    }
    int can_boost=h4_bit(p.v+H4_MOVES+1,boost);
    if(representative[CAT_SPEC]<0 || representative[CAT_PHYS]<0 || stab_special<0 || !can_boost) return chosen;
    int replies[]={representative[CAT_SPEC],representative[CAT_PHYS],stab_special,stab_special};
    int count=replies[0]==stab_special?3:4;
    double lower[2]={1,1},average[2]={0,0};
    double worst_change=1;
    for(int hypothesis=0;hypothesis<count;hypothesis++) {
        int enemy_setup=hypothesis==count-1;
        int reply=replies[hypothesis];
        double score[2]={0,0};
        for(int selected=0;selected<2;selected++) {
            for(int sample=0;sample<3;sample++) {
                H4Position forecast=p;
                for(int stat=0;stat<6;stat++) for(int dst=0;dst<3;dst++)
                    forecast.mon[1].stat[stat][dst]=p.mon[1].stat[stat][sample];
                for(int tie=0;tie<2;tie++)
                    score[selected]+=h4_opening_pressure(&forecast,move,reply,boost,selected,enemy_setup,tie)/6;
            }
            lower[selected]=fmin(lower[selected],score[selected]);
            average[selected]+=score[selected]/count;
        }
        worst_change=fmin(worst_change,score[1]-score[0]);
    }

    return lower[1]>=lower[0] && average[1]>average[0]+1e-9 && worst_change>=0?slot:chosen;
}

static int obsh4(const Observation3 *o,const ActionMask *mask,int options) {
    int first=obsdemo3(o,mask);
    if(first<0 || first==4) return first;
    int choices=0;
    for(int i=0;i<4;i++) choices+=!!mask->legal[i];
    if(choices==1) return first;
    H4Position p=h4_position(o),after;
    if(h4_should_mega(&p,mask,&after)) p=after;
    int chosen=h4_choose(&p,mask,options);

    if(!(options&OBSH4_OPENING) || o->v[O3_CONTEXT]!=1 || h4_slot(&p,mask,S8B_CALMMIND)<0) return chosen;
    // Cache only exact public turn-one states. This avoids recomputing the
    // deterministic forecast across matches that differ only in the seed.
    static struct {
        Observation3 observation;
        ActionMask mask;
        int valid,chosen,result;
    } cached[2];
    static int next_cache;
    for(int i=0;i<2;i++) {
        if(cached[i].valid && cached[i].chosen==chosen &&
           !memcmp(&cached[i].observation,o,sizeof *o) &&
           !memcmp(&cached[i].mask,mask,sizeof *mask)) return cached[i].result;
    }
    int result=h4_opening_choice(&p,mask,chosen);
    cached[next_cache].observation=*o;
    cached[next_cache].mask=*mask;
    cached[next_cache].valid=1;
    cached[next_cache].chosen=chosen;
    cached[next_cache].result=result;
    next_cache^=1;
    return result;
}

#include "stage3/policy.h"
#include "campaign3/policy.h"

// ---------- Policies (ported from mirror_sim5.py choose()) ----------
static int choose_move(Mon *me, Mon *foe) {
#ifdef ENGINE_SCRIPTED_ACTION
    // Deterministic fixture seam: only the actor's own state is supplied.
    int scripted=ENGINE_SCRIPTED_ACTION(me);
    if(scripted>=-1) return scripted;
#endif
    switch (me->policy) {
    case POL_MIRROR: case POL_NOISE: {
        if (me->hp <= me->maxhp * 0.40 && pp_of(me, MV_REST) > 0 && me->hp < me->maxhp && me->healblock == 0) return find_move(me, MV_REST);
        if (me->st[1] < 2 && pp_of(me, MV_ID) > 0) return find_move(me, MV_ID);
        if (me->policy == POL_NOISE && foe->hp <= foe->maxhp * 0.55 && foe->healblock == 0 && pp_of(me, MV_PN) > 0) return find_move(me, MV_PN);
        if ((foe->st[1] > me->st[1] || foe->st[3] > me->st[3]) && pp_of(me, MV_PU) > 0) return find_move(me, MV_PU);
        if (foe->st[1] == 0 && me->st[1] >= 2 && pp_of(me, MV_BP) > 0) return find_move(me, MV_BP);
        if (pp_of(me, MV_SB) > 0) return find_move(me, MV_SB);
        if (pp_of(me, MV_BP) > 0) return find_move(me, MV_BP);
        if (pp_of(me, MV_EQ) > 0) return find_move(me, MV_EQ);
        if (pp_of(me, MV_REST) > 0 && me->hp < me->maxhp && me->healblock == 0) return find_move(me, MV_REST);
        return -1; }
    case POL_FAST: {
        if (me->hp <= me->maxhp * 0.30 && pp_of(me, MV_REST) > 0 && me->hp < me->maxhp) return find_move(me, MV_REST);
        if ((foe->st[1] >= 2 || foe->st[3] >= 2) && pp_of(me, MV_PU) > 0 && me->st[1] < foe->st[1]) return find_move(me, MV_PU);
        if (pp_of(me, MV_EQ) > 0) return find_move(me, MV_EQ);
        return find_move(me, MV_BP); }   // sim5 returns Body Press unconditionally here
    case POL_STALL: {
        if (me->hp <= me->maxhp * 0.50 && pp_of(me, MV_REST) > 0 && me->hp < me->maxhp) return find_move(me, MV_REST);
        if ((me->st[1] < 3 || me->st[3] < 3) && pp_of(me, MV_CP) > 0) return find_move(me, MV_CP);
        return find_move(me, MV_SCL); }  // sim5 returns Shadow Claw unconditionally here
    case POL_GENERIC: {
        // Heuristic pilot for arbitrary builds (screening stages; the learned
        // model is the authority for final evaluation, per the project skill).
        if (me->hp <= me->maxhp * 0.40 && pp_of(me, MV_REST) > 0 && me->hp < me->maxhp && me->healblock == 0) return find_move(me, MV_REST);
        if (pp_of(me, MV_PN) > 0 && foe->healblock == 0 && find_move(foe, MV_REST) >= 0 && foe->hp > foe->maxhp * 0.40) return find_move(me, MV_PN);
        if (pp_of(me, MV_PU) > 0) {
            int fs = 0, ms = 0;
            for (int i = 0; i < 5; i++) { if (foe->st[i] > 0) fs += foe->st[i]; if (me->st[i] > 0) ms += me->st[i]; }
            if (fs - ms >= 3) return find_move(me, MV_PU);
        }
        if (me->hp > me->maxhp * 0.55) {
            if (pp_of(me, MV_CP) > 0 && (me->st[1] < 2 || me->st[3] < 2)) return find_move(me, MV_CP);
            if (pp_of(me, MV_ID) > 0 && me->st[1] < 2) return find_move(me, MV_ID);
            if (pp_of(me, MV_AGI) > 0 && me->st[4] < 1 && eff_speed(me) < eff_speed(foe)) return find_move(me, MV_AGI);
        }
        if (pp_of(me, MV_SUB) > 0 && me->sub_hp == 0 && me->hp >= me->maxhp * 0.80) return find_move(me, MV_SUB);
        if (pp_of(me, MV_SF) > 0 && foe->st[4] == 0 && eff_speed(foe) > eff_speed(me) && foe->hp > foe->maxhp * 0.50) return find_move(me, MV_SF);
        if (pp_of(me, MV_TRK) > 0 &&
            (foe->item == ITEM_HELMET || foe->item == ITEM_LEFTOVERS || foe->item == ITEM_SCARF || foe->item == ITEM_LIFEORB || foe->item == ITEM_SASH) &&
            (me->item == ITEM_NONE || me->item == ITEM_IRONBALL)) return find_move(me, MV_TRK);
        int best = -1, bestd = 0;
        for (int i = 0; i < me->nmoves; i++) {
            if (me->pp[i] <= 0) continue;
            int d = est_damage(me, foe, me->moves[i]);
            if (d > bestd) { bestd = d; best = i; }
        }
        if (best >= 0) return best;
        for (int i = 0; i < me->nmoves; i++) if (me->pp[i] > 0) return i;
        return -1; }
    }
    return -1;
}

// ---------- Battle ----------
// Returns 0 = A wins, 1 = B wins, 2 = draw. Simultaneous move selection (mainline-style),
// execution order by priority, then stage-adjusted Speed, then random tiebreak.
// Sleep/heal-block bookkeeping follows sim5 (decrement at the start of the mon's action).
static int SEQ_MODE = -1;   // ENGINE_SEQ=1: sim5-style sequential move selection
// ---------- Stage 7: simultaneous Mega declarations ----------
// No opponent truth enters the observable decision. Existing MGO2 offsets:
// current forms 4/14, own item 42, ability 44, ranks 31..36, reveals 79+8*i.
static int mega_available(const Mon *m) {
    return !m->transformed && m->mega_mode==2 && !m->mega && m->item==ITEM_STONE && m->mega_stats_valid;
}
static int mega_delay(int ability, int lowered, int all_known, int speed_drop) {
    return ability==AB_CB && !lowered && all_known && speed_drop;
}
static int obsh3_mega(const Observation2 *o) {
    const int32_t *v=o->v;
    if(v[4] || v[42]!=ITEM_STONE) return 0;
    int lowered=0, all_known=1, speed_drop=0;
    for(int i=31;i<=36;i++) lowered|=v[i]<0;
    for(int i=0;i<4;i++) {
        int at=79+8*i;
        all_known &= v[at]!=0;
        if(v[at] && (v[at+1]==MV_SF || v[at+1]==MV_IW)) speed_drop=1;
    }
    return !mega_delay(v[44],lowered,all_known,speed_drop);
}
static int generic_mega(const Mon *me, const Mon *foe) {
    int lowered=me->acc<0, speed_drop=0;
    for(int i=0;i<5;i++) lowered|=me->st[i]<0;
    for(int i=0;i<foe->nmoves;i++) speed_drop|=foe->moves[i]==MV_SF || foe->moves[i]==MV_IW;
    return !mega_delay(me->ability,lowered,foe->nmoves==4,speed_drop);
}
static void resolve_mega(Mon *m, int declared) {
    if(!declared || !mega_available(m)) return;
    // HP, ranks, status, PP and every volatile survive this form change.
    s8_ability_end(m);
    if(m->spec_v3) {
        s8_calculate_stats(m,m->table_mega_target,m->stats);
        const S8Species *target=&S8_SPECIES[m->table_mega_target];
        s8_set_form(m,m->table_mega_target,S8_SPECIES_ABILITIES[target->ability_offset]);
    } else {
        memcpy(m->stats,m->mega_stats,sizeof(m->stats));
        m->ability=AB_NONE; m->ability_changed=0; // Legacy Tough Claws is represented by mega.
        m->type1=S8_SPECIES[S8_METAGROSS_MEGA].types[0];
        m->type2=S8_SPECIES[S8_METAGROSS_MEGA].types[1]; m->added_type=0;
    }
    m->mega=1;
    s8_ability_new_state(m);s8_ability_start(m);
    obs_emit(EV_MEGA,m,1,0,0);
    if(OBS && OBS->version==3) obs_emit(EV3_IDENTITY,m,m->table_base_id,m->table_species_id,1);
    if(field_member(m)) s8_ability_phase(FIELD.seat[0],FIELD.seat[1],S8AE_AFTER_MEGA,-1,0);
}
static void mega_validate_pilots(const Mon *a, const Mon *b) {
    const Mon *ms[]={a,b};
    for(int i=0;i<2;i++) if(ms[i]->mega_mode==2 &&
       ms[i]->policy!=POL_GENERIC && ms[i]->policy!=POL_OBSH3 && !is_v3_policy(ms[i]->policy)) {
        fprintf(stderr,"mega=2 requires generic, obsh3, obsdemo3 or obsh4\n"); exit(2);
    }
}

enum { OBSH4_DIAG_TURNS=6 };
typedef struct {
    Observation3 observation[2];
    ActionMask masks[2];
    uint64_t rng;
    int choice[2], attempted;
} Obsh4Turn;

typedef struct {
    int seat, candidate, baseline, turn, slot;
    Obsh4Turn *record;
    const Obsh4Turn *expected;
} Obsh4Audit;

static int battle_with_audit(Mon *A, Mon *B, Rng *r, int *turns_out,const Obsh4Audit *audit) {
    s8_require_battle(A,B);
    mega_validate_pilots(A,B);
    field_bind(A,B); BATTLE_RNG=r; // reset before attachment, including reused seat addresses
    if (SEQ_MODE < 0) SEQ_MODE = getenv("ENGINE_SEQ") ? 1 : 0;
    if(SEQ_MODE && (!mon_v1_supported(A) || !mon_v1_supported(B))) {
        fprintf(stderr,"new move vocabulary requires simultaneous selection\n"); exit(2);
    }
    Mon *mons[2] = {A, B};
    int observable=is_observable_policy(A->policy) || is_observable_policy(B->policy);
    int version3=A->spec_v3 && B->spec_v3;
    if((is_v3_policy(A->policy) || is_v3_policy(B->policy)) && !version3) { fprintf(stderr,"MGO3 requires two v3 specs\n");exit(2); }
    if(version3 && ((is_observable_policy(A->policy) && !is_v3_policy(A->policy)) || (is_observable_policy(B->policy) && !is_v3_policy(B->policy)))) { fprintf(stderr,"MGO1/MGO2 pilots require legacy specs\n");exit(2); }
    int version2=is_v2_policy(A->policy) || is_v2_policy(B->policy) || getenv("ENGINE_OBS_V2")!=NULL || A->mega_mode==2 || B->mega_mode==2;
    int enabled=observable || (OBS_FORCE>=0 ? OBS_FORCE : getenv("ENGINE_OBSERVER")!=NULL);
    OBS=NULL;S8_ABILITY_TRACE=0;
    if(enabled && version3) obs3_attach(A,B);
    s8_ability_battle_start(A,B); // v3 captures public entry announcements
    obs3_publish();
    if(enabled && SEQ_MODE) { fprintf(stderr,"observer/obsdemo requires simultaneous selection; ENGINE_SEQ is unsupported\n"); exit(2); }
    if(!version3 && (version2 || obs2_vocabulary(A,B)) && (
       (is_observable_policy(A->policy) && !is_v2_policy(A->policy)) ||
       (is_observable_policy(B->policy) && !is_v2_policy(B->policy)))) {
        fprintf(stderr,"MGO1/g12 requires original 17-move vocabulary and cannot mix with MGO2/v2 pilots\n"); exit(2);
    }
    if(enabled && !version3) {
        int profile=OBS_PROFILE ? OBS_PROFILE : (getenv("ENGINE_OBS_COMPAT") ? OBS_COMPAT : OBS_REGULAR);
        if(!(version2?obs2_valid_build(A,profile):obs_valid_build(A,profile)) ||
           !(version2?obs2_valid_build(B,profile):obs_valid_build(B,profile))) {
            fprintf(stderr,"observer build rejected: MGO1/g12 requires original 17-move vocabulary and v1 state; MGO2 requires neutral initialization; regular requires four distinct moves, fixed PP, legal Metagross stats and normal CB/LM; legacy builds require ENGINE_OBS_COMPAT=1\n"); exit(2);
        }
        if(version2) obs2_attach(A,B,profile); else obs_attach(A,B,profile);
    }
    for (int turn = 1; turn <= MAX_TURN; turn++) {
        Obsh4Turn *record=audit && turn<=OBSH4_DIAG_TURNS &&
            (!audit->turn || audit->turn==turn) && audit->record?&audit->record[turn-1]:NULL;
        g5_begin_turn(A,B);
        if (SEQ_MODE) {
            // order by speed only (as sim5), each mon chooses at its own action moment
            int first = 0;
            {
                double sa = A->stats[5] * smult(A->st[4]);
                double sb = B->stats[5] * smult(B->st[4]);
                if (sb > sa) first = 1;
                else if (sb == sa && rng_dbl(r) < 0.5) first = 1;
            }
            for (int k = 0; k < 2; k++) {
                Mon *me = mons[k == 0 ? first : 1 - first];
                Mon *foe = mons[k == 0 ? 1 - first : first];
                if (me->hp <= 0 || foe->hp <= 0) break;
                if (me->healblock > 0) me->healblock--;
                if (me->sleep > 0) {
                    me->sleep--;
                    int sti = find_move(me, MV_ST);
                    if (sti >= 0 && me->pp[sti] > 0) {
                        if(!status_before_move(me,r)) continue;
                        s8_spend_pp(me,sti); leppa_check(me, sti);
                        int others[4], no = 0;
                        for (int i = 0; i < me->nmoves; i++) if (i != sti && g5_sleep_callable(me->moves[i])) others[no++] = i;
                        if (no > 0) {
                            int call = others[rng_int(r, no)];
                            int cmv = me->moves[call];
                            if (cmv != MV_REST && cmv != MV_ST) execute_sleep_move(me, foe, cmv, r);
                            note_move_used(me, cmv);       // the called move, including a failed Rest, was used
                        }
                    }
                    if (me->hp <= 0 || foe->hp <= 0) break;
                    continue;
                }
                int ci = choose_move(me, foe);
                if (ci < 0) { do_struggle(me, foe, r); me->last_move = -1; me->metro_streak = 0; }
                else { if(!s8_rampage_locked(me) && !s8_charge_release(me,me->moves[ci])) s8_spend_pp(me,ci); leppa_check(me, ci); execute_move(me, foe, me->moves[ci], r); note_move_used(me, me->moves[ci]); }
                if (me->hp <= 0 || foe->hp <= 0) break;
            }
            end_of_turn(A, B);
            A->protected_now = 0; B->protected_now = 0;
            if (A->hp <= 0 || B->hp <= 0) {
                *turns_out = turn;
                if (A->hp <= 0 && B->hp <= 0) return 2;
                return A->hp <= 0 ? 1 : 0;
            }
            if (turn == MAX_TURN) {
                *turns_out = turn;
                double fa = (double)A->hp / A->maxhp, fb = (double)B->hp / B->maxhp;
                if (fa == fb) return 2;
                return fa > fb ? 0 : 1;
            }
            continue;
        }
        Observation frozen[2]; Observation2 frozen2[2]; Observation3 frozen3[2]; ActionMask masks[2];
        if(OBS) {
            OBS->turn=turn;
            if(turn>1) obs_emit(EV_BEGIN,A,0,0,0);
            // Both snapshots exist before either callback or submitted action.
            for(int i=0;i<2;i++) {
                if(version3) obs3_build(mons[i],&OBS->v3[i],&frozen3[i],&masks[i]);
                else if(version2) obs2_build(mons[i],&OBS->v2[i],&frozen2[i],&masks[i]);
                else obs_build(mons[i],&OBS->view[i],&frozen[i],&masks[i]);
            }
        }
        int choice[2], mega_choice[2]={0,0};
        for (int i = 0; i < 2; i++) {
            int policy=mons[i]->policy;
            if(mega_available(mons[i])) {
                if(policy==POL_OBSH4) mega_choice[i]=obsh4_mega(&frozen3[i],&masks[i]);
                else mega_choice[i]=policy==POL_OBSDEMO3?obsdemo3_mega(&frozen3[i]):policy==POL_OBSH3?obsh3_mega(&frozen2[i]):generic_mega(mons[i],mons[1-i]);
            }
            if(policy==POL_OBSH4) {
                int candidate=OBSH4_DEFAULT;
                if(turn==1 && !strcmp(S8_SPECIES[frozen3[i].v[O3_SELF_ID+1]].id,"sylveon"))
                    candidate|=OBSH4_OPENING;
                if(audit && audit->candidate>=0)
                    candidate=i==audit->seat?audit->candidate:audit->baseline;
                choice[i]=obs_checked_action(obsh4(&frozen3[i],&masks[i],candidate),&masks[i]);
            } else if(is_v3_policy(policy)) choice[i]=obs_checked_action(obsdemo3(&frozen3[i],&masks[i]),&masks[i]);
            else if(is_v2_policy(policy)) choice[i]=obs_checked_action((policy==POL_LEARNED2?learned2_policy:policy==POL_OBSH3?obsh3:observable_policy2)(&frozen2[i],&masks[i]),&masks[i]);
            else if(is_observable_policy(policy)) choice[i]=obs_checked_action((policy==POL_LEARNED?learned_policy:policy==POL_OBSH2?obsh2:observable_policy)(&frozen[i],&masks[i]),&masks[i]);
            else choice[i]=choose_move(mons[i],mons[1-i]);
        }
        if(record) {
            memcpy(record->observation,frozen3,sizeof frozen3);
            memcpy(record->masks,masks,sizeof masks);
            memcpy(record->choice,choice,sizeof choice);
            record->rng=r->s;
            record->attempted=-1;
            if(audit->expected) {
                const Obsh4Turn *expected=audit->expected;
                if(memcmp(expected->observation,frozen3,sizeof frozen3) ||
                   memcmp(expected->masks,masks,sizeof masks) ||
                   memcmp(expected->choice,choice,sizeof choice) || expected->rng!=r->s ||
                   !masks[audit->seat].legal[audit->slot]) {
                    fprintf(stderr,"obsh4 diagnostic replay diverged before turn %d\n",turn);
                    exit(2);
                }
                choice[audit->seat]=audit->slot==4?-1:audit->slot;
            }
        }
        // Both choices and declarations are frozen before either form changes. No RNG.
        for(int i=0;i<2;i++) resolve_mega(mons[i],mega_choice[i]);
        s8_ability_phase(A,B,S8AE_UPDATE,-1,0);
        // Choice Scarf lock: a locked mon must repeat its locked move while it has PP.
        for (int i = 0; i < 2; i++) {
            Mon *m = mons[i];
            if (!is_observable_policy(m->policy) && held_item(m) == ITEM_SCARF && m->choice_lock >= 0) {
                int li = find_move(m, m->choice_lock);
                if (li >= 0 && m->pp[li] > 0) choice[i] = li;
            }
        }
        // Both policies and item locks have committed before focus is announced.
        for(int i=0;i<2;i++) {
            Mon *m=mons[i];
            if(m->charging) choice[i]=s8_charge_slot(m);
            choice[i]=s8_control_commit(m,mons[1-i],choice[i]);
            s8_commit_move(m,choice[i]>=0?m->moves[choice[i]]:-1);
        }
        for(int i=0;i<2;i++) if(FIELD.committed[i]==S8U_M_BEAKBLAST && !mons[i]->recharge) {
            mons[i]->beak_hot=1;mons[i]->state_features|=MON_POST_V1_STATE;
            obs_emit(EV_PREPARE,mons[i],S8U_M_BEAKBLAST,0,0);
        }
        for(int i=0;i<2;i++) if(FIELD.committed[i]==MV_FP && !mons[i]->recharge) {
            mons[i]->focusing=1; mons[i]->state_features|=MON_POST_V1_STATE;
            obs_emit(EV_PREPARE,mons[i],MV_FP,0,0);
        }
        // order
        int first = 0;
        {
            int pa = g5_priority(mons[0],choice[0])*10;
            int pb = g5_priority(mons[1],choice[1])*10;
            int fractional=s8_has_ability(A,S8V3_QUICKDRAW) || s8_has_ability(B,S8V3_QUICKDRAW) || s8_has_ability(A,S8V3_STALL) || s8_has_ability(B,S8V3_STALL);
            if(fractional) { pa+=FIELD.fractional_priority[0];pb+=FIELD.fractional_priority[1]; }
            if (pb > pa) first = 1;
            else if (pb == pa) {
                // Quick Claw: 20% to move first within the same priority bracket.
                int ca = (!fractional && held_item(A) == ITEM_CLAW && rng_dbl(r) < 0.20) ? 1 : 0;
                int cb = (!fractional && held_item(B) == ITEM_CLAW && rng_dbl(r) < 0.20) ? 1 : 0;
                if(ca) obs_item(A,0);
                if(cb) obs_item(B,0);
                if (ca != cb) first = cb;
                else {
                    double sa = s8_action_speed(A);
                    double sb = s8_action_speed(B);
                    if (sb > sa) first = 1;
                    else if (sb == sa && rng_dbl(r) < 0.5) first = 1;
                }
            }
        }
        for (int k = 0; k < 2; k++) {
            Mon *me = mons[k == 0 ? first : 1 - first];
            Mon *foe = mons[k == 0 ? 1 - first : first];
            if (me->hp <= 0 || foe->hp <= 0) break;
            int seat=me==FIELD.seat[1];
            int selected=s8_control_redirect(me,k==0?choice[first]:choice[1-first]);
            if(k==0) choice[first]=selected; else choice[1-first]=selected;
            s8_control_begin_action(me);
            if(me->turn_cancelled) { FIELD.consumed[seat]=1;continue; }
            FIELD.consumed[seat]=1;
            CUR_SECOND = (k == 1);
            obs_emit(EV_OPPORTUNITY,me,OBS && OBS->version>=2?ACT_NONE:me->flinched?ACT_FLINCH:me->sleep>0?ACT_SLEEP:ACT_USED,0,0);
            if(me->recharge) { s8_control_abort(me,selected<0?-1:me->moves[selected]); obs2_emit(EV_SKIP,me,SKIP_RECHARGE,0,0); me->recharge=0; obs2_cancel_charge(me); me->charging=0; attempt_result(me,ATTEMPT_SKIP_NOT_FAIL); obs_emit(EV_RECHARGE,me,0,0,0); continue; }
            if(!freeze_before_move(me,s8_selected_defrost(me,k==0?choice[first]:choice[1-first]),0,r)) { s8_control_abort(me,selected<0?-1:me->moves[selected]); obs2_emit(EV_SKIP,me,SKIP_FREEZE,0,0); attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue; }
            if (me->flinched) { s8_hit_flinch(me);s8_control_abort(me,selected<0?-1:me->moves[selected]); obs2_emit(EV_SKIP,me,SKIP_FLINCH,0,0); me->flinched = 0; attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue; }   // flinch: lose this action
            if (me->healblock > 0) { obs2_emit(EV_BLOCK_TICK,me,0,0,0); me->healblock--; if(!me->healblock) obs_emit(EV_BLOCK,me,0,0,0); }
            int committed = (k == 0 ? choice[first] : choice[1 - first]);
            if(committed>=0 && (me->moves[committed]==MV_MR || (me->moves[committed]>=MV_COUNT &&
                    (S8_MOVES[me->moves[committed]].flags&S8_FLAG_GRAVITY))) && current_gravity(me)) {
                s8_control_abort(me,me->moves[committed]);
                // R4: named failure before PP deduction, as in Gravity's
                // BeforeMove handler. Origin 4 announces a direct nonspending
                // attempt; no execution/note/lock or repetition update occurs.
                obs2_emit(EV_USE,me,me->moves[committed],4,0);
                obs2_emit(EV_HIT,me,HIT_FAIL,0,0);
                attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue;
            }
            int snore=selected_snore(me,foe,committed,r);
            if(snore) {
                if(record && seat==audit->seat && snore==2) record->attempted=MV_SN;
                if (me->hp <= 0 || foe->hp <= 0) break;
                continue;
            }
            if (me->sleep > 0) {
                s8_control_abort(me,selected<0?-1:me->moves[selected]);
                attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0;
                if(!s8_has_ability(me,S8V3_EARLYBIRD)) { obs2_emit(EV_SLEEP_TICK,me,0,0,0); me->sleep--; }
                if(!me->sleep) obs_emit(EV_SLEEP,me,0,0,0);
                int sti = find_move(me, MV_ST);
                if (sti >= 0 && me->pp[sti] > 0) {
                    if(!s8_control_before_move(me,foe,MV_ST,r)) continue;
                    if(record && seat==audit->seat) record->attempted=MV_ST;
                    attempt_result(me,ATTEMPT_FAIL);
                    obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,MV_ST},2);
                    s8_spend_pp(me,sti); leppa_check(me, sti);
                    // choose uniformly among the other known moves
                    int others[4], no = 0;
                    for (int i = 0; i < me->nmoves; i++) if (i != sti && g5_sleep_callable(me->moves[i])) others[no++] = i;
                    if (no > 0) {
                        int call = others[rng_int(r, no)];
                        int cmv = me->moves[call];
                        obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,cmv},1);
                        if(cmv==MV_REST || cmv==MV_ST) obs_emit(EV_HIT,me,HIT_FAIL,0,0);
                        if (cmv != MV_REST && cmv != MV_ST) execute_sleep_move(me, foe, cmv, r);
                        note_move_used(me, cmv);           // Sleep Talk records the move it called
                    } else obs2_emit(EV_HIT,me,HIT_FAIL,0,0); // R5: wrapper announced, no eligible call
                }
                if (me->hp <= 0 || foe->hp <= 0) break;
                continue;
            }
            if(!s8_control_before_move(me,foe,committed<0?-1:me->moves[committed],r)) continue;
            int ci = (k == 0 ? choice[first] : choice[1 - first]);
            if(ci>=0 && me->moves[ci]==MV_FP && me->focus_lost) { obs2_emit(EV_SKIP,me,SKIP_FOCUS,0,0); attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); continue; }
            if(record && seat==audit->seat) record->attempted=ci<0?S8_STRUGGLE:me->moves[ci];
            if (ci < 0) { do_struggle(me, foe, r); me->last_move = -1; me->metro_streak = 0; }
            else {
                obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,me->moves[ci]},0);
                if(!s8_rampage_locked(me) && !s8_charge_release(me,me->moves[ci])) s8_spend_pp(me,ci);
                leppa_check(me, ci);
                execute_move(me, foe, me->moves[ci], r);
                note_move_used(me, me->moves[ci]);
            }
            if (me->hp <= 0 || foe->hp <= 0) break;
        }
        end_of_turn(A, B);
        A->protected_now = 0; B->protected_now = 0;
        obs3_publish();
        obs_emit(EV_END,A,0,0,0);
        if (A->hp <= 0 || B->hp <= 0) {
            *turns_out = turn;
            if (A->hp <= 0 && B->hp <= 0) return obs_finish(ordered_faint_result(),0);
            return obs_finish(A->hp <= 0 ? 1 : 0,0);
        }
        if (turn == MAX_TURN) {
            *turns_out = turn;
            double fa = (double)A->hp / A->maxhp, fb = (double)B->hp / B->maxhp;
            if (fa == fb) return obs_finish(2,1);
            return obs_finish(fa > fb ? 0 : 1,1);
        }
    }
    *turns_out = MAX_TURN;
    return 2;
}

static int battle(Mon *A, Mon *B, Rng *r, int *turns_out) {
    return battle_with_audit(A,B,r,turns_out,NULL);
}

// ---------- Trace (one battle, verbose) ----------
static int battle_trace(Mon *A, Mon *B, Rng *r) {
    s8_require_battle(A,B);
    mega_validate_pilots(A,B);
    OBS=NULL;
    if(is_observable_policy(A->policy) || is_observable_policy(B->policy)) {
        fprintf(stderr,"observable policies use battle(), not the privileged trace path\n"); exit(2);
    }
    Mon *mons[2] = {A, B};
    field_bind(A,B); BATTLE_RNG=r;S8_ABILITY_TRACE=1;
    s8_ability_battle_start(A,B);
    for (int turn = 1; turn <= MAX_TURN; turn++) {
        g5_begin_turn(A,B);
        int choice[2];
        int mega_choice[2];
        for (int i = 0; i < 2; i++) {
            mega_choice[i]=mega_available(mons[i]) && generic_mega(mons[i],mons[1-i]);
            choice[i] = choose_move(mons[i], mons[1 - i]);
        }
        for(int i=0;i<2;i++) if(mega_choice[i]) {
            resolve_mega(mons[i],1);
            printf("T%d %c Mega Evolution\n",turn,'A'+i);
        }
        s8_ability_phase(A,B,S8AE_UPDATE,-1,0);
        for (int i = 0; i < 2; i++) {
            Mon *m = mons[i];
            if (held_item(m) == ITEM_SCARF && m->choice_lock >= 0) {
                int li = find_move(m, m->choice_lock);
                if (li >= 0 && m->pp[li] > 0) choice[i] = li;
            }
        }
        // Both policies and item locks have committed before focus is announced.
        for(int i=0;i<2;i++) {
            Mon *m=mons[i];
            if(m->charging) choice[i]=s8_charge_slot(m);
            choice[i]=s8_control_commit(m,mons[1-i],choice[i]);
            s8_commit_move(m,choice[i]>=0?m->moves[choice[i]]:-1);
        }
        for(int i=0;i<2;i++) if(FIELD.committed[i]==S8U_M_BEAKBLAST && !mons[i]->recharge) {
            mons[i]->beak_hot=1;mons[i]->state_features|=MON_POST_V1_STATE;
            obs_emit(EV_PREPARE,mons[i],S8U_M_BEAKBLAST,0,0);
        }
        for(int i=0;i<2;i++) if(FIELD.committed[i]==MV_FP && !mons[i]->recharge) {
            mons[i]->focusing=1; mons[i]->state_features|=MON_POST_V1_STATE;
            obs_emit(EV_PREPARE,mons[i],MV_FP,0,0);
        }
        int first = 0;
        {
            int pa = g5_priority(mons[0],choice[0])*10;
            int pb = g5_priority(mons[1],choice[1])*10;
            int fractional=s8_has_ability(A,S8V3_QUICKDRAW) || s8_has_ability(B,S8V3_QUICKDRAW) || s8_has_ability(A,S8V3_STALL) || s8_has_ability(B,S8V3_STALL);
            if(fractional) { pa+=FIELD.fractional_priority[0];pb+=FIELD.fractional_priority[1]; }
            if (pb > pa) first = 1;
            else if (pb == pa) {
                int ca = (!fractional && held_item(A) == ITEM_CLAW && rng_dbl(r) < 0.20) ? 1 : 0;
                int cb = (!fractional && held_item(B) == ITEM_CLAW && rng_dbl(r) < 0.20) ? 1 : 0;
                if (ca != cb) first = cb;
                else {
                    double sa = s8_action_speed(A);
                    double sb = s8_action_speed(B);
                    if (sb > sa) first = 1;
                    else if (sb == sa && rng_dbl(r) < 0.5) first = 1;
                }
            }
        }
        for (int k = 0; k < 2; k++) {
            Mon *me = mons[k == 0 ? first : 1 - first];
            Mon *foe = mons[k == 0 ? 1 - first : first];
            const char *who = (me == A) ? "A" : "B";
            if (me->hp <= 0 || foe->hp <= 0) break;
            int seat=me==FIELD.seat[1];
            int selected=s8_control_redirect(me,k==0?choice[first]:choice[1-first]);
            if(k==0) choice[first]=selected; else choice[1-first]=selected;
            s8_control_begin_action(me);
            if(me->turn_cancelled) { FIELD.consumed[seat]=1;continue; }
            FIELD.consumed[seat]=1;
            CUR_SECOND = (k == 1);
            if(me->recharge) { s8_control_abort(me,selected<0?-1:me->moves[selected]); obs2_emit(EV_SKIP,me,SKIP_RECHARGE,0,0); me->recharge=0; obs2_cancel_charge(me); me->charging=0; attempt_result(me,ATTEMPT_SKIP_NOT_FAIL); obs_emit(EV_RECHARGE,me,0,0,0); continue; }
            if(!freeze_before_move(me,s8_selected_defrost(me,k==0?choice[first]:choice[1-first]),0,r)) { s8_control_abort(me,selected<0?-1:me->moves[selected]); obs2_emit(EV_SKIP,me,SKIP_FREEZE,0,0); attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue; }
            if (me->flinched) { s8_hit_flinch(me);s8_control_abort(me,selected<0?-1:me->moves[selected]); obs2_emit(EV_SKIP,me,SKIP_FLINCH,0,0); me->flinched = 0; attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; printf("T%d %s: FLINCH  A HP%d B HP%d\n", turn, who, A->hp, B->hp); continue; }
            if (me->healblock > 0) me->healblock--;
            int committed = (k == 0 ? choice[first] : choice[1 - first]);
            if(committed>=0 && (me->moves[committed]==MV_MR || (me->moves[committed]>=MV_COUNT &&
                    (S8_MOVES[me->moves[committed]].flags&S8_FLAG_GRAVITY))) && current_gravity(me)) { s8_control_abort(me,me->moves[committed]); obs2_emit(EV_SKIP,me,SKIP_UNANNOUNCED,0,0); attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue; }
            if(selected_snore(me,foe,committed,r)) {
                printf("T%d %s: SN (asleep)  A HP%d B HP%d\n",turn,who,A->hp,B->hp);
                if (me->hp <= 0 || foe->hp <= 0) break;
                continue;
            }
            if (me->sleep > 0) {
                s8_control_abort(me,selected<0?-1:me->moves[selected]);
                attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0;
                if(!s8_has_ability(me,S8V3_EARLYBIRD)) me->sleep--;
                int sti = find_move(me, MV_ST);
                if (sti >= 0 && me->pp[sti] > 0) {
                    if(!s8_control_before_move(me,foe,MV_ST,r)) continue;
                    s8_spend_pp(me,sti); leppa_check(me, sti);
                    int others[4], no = 0;
                    for (int i = 0; i < me->nmoves; i++) if (i != sti && g5_sleep_callable(me->moves[i])) others[no++] = i;
                    if (no > 0) {
                        int call = others[rng_int(r, no)];
                        int cmv = me->moves[call];
                        if (cmv != MV_REST && cmv != MV_ST) execute_sleep_move(me, foe, cmv, r);
                        note_move_used(me, cmv);           // match the non-trace battle path
                        if (cmv == MV_TRK && trick_stone_blocked(me, foe))
                            printf("T%d %s: ST->TRK (Mega Stone blocks swap)  A HP%d B HP%d\n", turn, who, A->hp, B->hp);
                        else printf("T%d %s: ST->%s  A HP%d B HP%d\n", turn, who, move_def(cmv)->code, A->hp, B->hp);
                    } else printf("T%d %s: SLEEP  A HP%d B HP%d\n", turn, who, A->hp, B->hp);
                } else printf("T%d %s: SLEEP  A HP%d B HP%d\n", turn, who, A->hp, B->hp);
                if (me->hp <= 0 || foe->hp <= 0) break;
                continue;
            }
            if(!s8_control_before_move(me,foe,committed<0?-1:me->moves[committed],r)) continue;
            int ci = (k == 0 ? choice[first] : choice[1 - first]);
            if(ci>=0 && me->moves[ci]==MV_FP && me->focus_lost) { obs2_emit(EV_SKIP,me,SKIP_FOCUS,0,0); attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); continue; }
            if (ci < 0) { do_struggle(me, foe, r); me->last_move = -1; me->metro_streak = 0; printf("T%d %s: STRUGGLE  A HP%d B HP%d\n", turn, who, A->hp, B->hp); }
            else { if(!s8_rampage_locked(me) && !s8_charge_release(me,me->moves[ci])) s8_spend_pp(me,ci); leppa_check(me, ci); execute_move(me, foe, me->moves[ci], r); note_move_used(me, me->moves[ci]);
                   if (me->moves[ci] == MV_TRK && trick_stone_blocked(me, foe))
                       printf("T%d %s: TRK (Mega Stone blocks swap)  A HP%d B HP%d\n", turn, who, A->hp, B->hp);
                   else printf("T%d %s: %s  A HP%d B HP%d\n", turn, who, move_def(me->moves[ci])->code, A->hp, B->hp); }
            if (me->hp <= 0 || foe->hp <= 0) break;
        }
        end_of_turn(A, B);
        A->protected_now = 0; B->protected_now = 0;
        if (A->hp <= 0 || B->hp <= 0) {
            if (A->hp <= 0 && B->hp <= 0) { int result=ordered_faint_result(); if(result==2) printf("DRAW at T%d\n",turn); else printf("double faint at T%d result %d\n", turn,result); return result; }
            printf("%s wins at T%d\n", A->hp <= 0 ? "B" : "A", turn);
            return A->hp <= 0 ? 1 : 0;
        }
    }
    printf("turn limit\n");
    return 2;
}

// ---------- Build specs ----------
static int parse_policy(const char *s) {
    if (!strcmp(s, "mirror")) return POL_MIRROR;
    if (!strcmp(s, "noise")) return POL_NOISE;
    if (!strcmp(s, "fast")) return POL_FAST;
    if (!strcmp(s, "stall")) return POL_STALL;
    if (!strcmp(s, "generic")) return POL_GENERIC;
    if (!strcmp(s, "obsdemo")) return POL_OBSDEMO;
    if (!strcmp(s, "obsh2")) return POL_OBSH2;
    if (!strcmp(s, "learned")) return POL_LEARNED;
    if (!strcmp(s, "obsdemo2")) return POL_OBSDEMO2;
    if (!strcmp(s, "obsh3")) return POL_OBSH3;
    if (!strcmp(s, "learned2")) return POL_LEARNED2;
    if (!strcmp(s, "obsdemo3")) return POL_OBSDEMO3;
    if (!strcmp(s, "obsh4")) return POL_OBSH4;
    static int warned = 0;
    if (!warned) { warned = 1; fprintf(stderr, "warning: unknown policy token \"%s\" treated as mirror\n", s); }
    return POL_MIRROR;
}
static int parse_item(const char *s) {
    if (!strcmp(s, "none")) return ITEM_NONE;
    if (!strcmp(s, "helmet")) return ITEM_HELMET;
    if (!strcmp(s, "stone")) return ITEM_STONE;
    if (!strcmp(s, "leftovers")) return ITEM_LEFTOVERS;
    if (!strcmp(s, "sitrus")) return ITEM_SITRUS;
    if (!strcmp(s, "scarf")) return ITEM_SCARF;
    if (!strcmp(s, "lifeorb")) return ITEM_LIFEORB;
    if (!strcmp(s, "muscle")) return ITEM_MUSCLE;
    if (!strcmp(s, "wise")) return ITEM_WISE;
    if (!strcmp(s, "belt")) return ITEM_BELT;
    if (!strcmp(s, "tag")) return ITEM_TAG;
    if (!strcmp(s, "sand")) return ITEM_SAND;
    if (!strcmp(s, "spoon")) return ITEM_SPOON;
    if (!strcmp(s, "bbelt")) return ITEM_BBELT;
    if (!strcmp(s, "metronome")) return ITEM_METRO;
    if (!strcmp(s, "shellbell")) return ITEM_SHELLBELL;
    if (!strcmp(s, "lens")) return ITEM_LENS;
    if (!strcmp(s, "kings")) return ITEM_KINGS;
    if (!strcmp(s, "wide")) return ITEM_WIDE;
    if (!strcmp(s, "zoom")) return ITEM_ZOOM;
    if (!strcmp(s, "powder")) return ITEM_POWDER;
    if (!strcmp(s, "claw")) return ITEM_CLAW;
    if (!strcmp(s, "ironball")) return ITEM_IRONBALL;
    if (!strcmp(s, "shuca")) return ITEM_SHUCA;
    if (!strcmp(s, "chople")) return ITEM_CHOPLE;
    if (!strcmp(s, "kasib")) return ITEM_KASIB;
    if (!strcmp(s, "payapa")) return ITEM_PAYAPA;
    if (!strcmp(s, "chesto")) return ITEM_CHESTO;
    if (!strcmp(s, "lum")) return ITEM_LUM;
    if (!strcmp(s, "leppa")) return ITEM_LEPPA;
    if (!strcmp(s, "oran")) return ITEM_ORAN;
    if (!strcmp(s, "sash")) return ITEM_SASH;
    if (!strcmp(s, "fband")) return ITEM_FBAND;
    if (!strcmp(s, "balloon")) return ITEM_BALLOON;
    if (!strcmp(s, "herb")) return ITEM_HERB;
    int type_item=s8_type_boost_parse(s);
    if(type_item) return type_item;
    int resist_berry=s8_resist_berry_parse(s);
    if(resist_berry) return resist_berry;
    int status_item=s8_status_item_parse(s);
    if(status_item) return status_item;
    int duration_item=s8_duration_item_parse(s);
    if(duration_item) return duration_item;
    int seed=s8_seed_parse(s);
    if(seed) return seed;
    int misc=s8_misc_item_parse(s);
    if(misc) return misc;
    int switch_item=s8_switch_item_parse(s);
    if(switch_item) return switch_item;
    static int warned = 0;
    if (!warned) { warned = 1; fprintf(stderr, "warning: unknown item token \"%s\" treated as none\n", s); }
    return ITEM_NONE;
}
// ---------- Stage 8b-2: v3 construction and explicit execution boundary ----------
// Table move 438 is Struggle. It is not an ordinary selectable move, nor the
// MGO1/MGO2 wire sentinel 17/68. ACTION_STRUGGLE alone maps to those wire IDs.
// The first 68 table IDs share the frozen legacy ordering; projection below is
// explicit and rejects every new table ID before a legacy pilot can index it.
static void s8_error(const char *reason, const char *token) {
    fprintf(stderr,"spec v3 rejected: %s (%s)\n",reason,token?token:""); exit(2);
}
static int s8_species_id(const char *id) {
    for(int i=0;i<S8_SPECIES_ROWS_COUNT;i++) if(!strcmp(id,S8_SPECIES[i].id)) return i;
    return -1;
}
static int s8_ability_id(const char *id) {
    for(int i=0;i<S8_ABILITIES_COUNT;i++) if(!strcmp(id,S8_ABILITIES[i].id)) return i;
    return -1;
}
static int s8_item_id(const char *id) {
    for(int i=0;i<S8_NON_STONE_ITEMS_COUNT+S8_STONES_COUNT;i++) if(!strcmp(id,S8_ITEMS[i].id)) return i;
    return -1;
}
static const char *const S8_LEGACY_ITEM_KEYS[] = {
    "none","rockyhelmet","metagrossite","leftovers","sitrusberry","choicescarf","lifeorb","muscleband",
    "wiseglasses","expertbelt","spelltag","softsand","twistedspoon","blackbelt","metronome","shellbell",
    "scopelens","kingsrock","widelens","zoomlens","brightpowder","quickclaw","ironball","shucaberry",
    "chopleberry","kasibberry","payapaberry","chestoberry","lumberry","leppaberry","oranberry",
    "focussash","focusband","airballoon","whiteherb"
};
static const char *const S8_LEGACY_ITEM_TOKENS[] = {
    "none","helmet","stone","leftovers","sitrus","scarf","lifeorb","muscle",
    "wise","belt","tag","sand","spoon","bbelt","metronome","shellbell",
    "lens","kings","wide","zoom","powder","claw","ironball","shuca",
    "chople","kasib","payapa","chesto","lum","leppa","oran","sash","fband","balloon","herb"
};
static int s8_item_projection(int table_id) {
    if(table_id<0) return ITEM_NONE;
    if(S8_ITEMS[table_id].is_stone) return ITEM_STONE;
    for(int i=1;i<=ITEM_HERB;i++) if(!strcmp(S8_ITEMS[table_id].id,S8_LEGACY_ITEM_KEYS[i])) return i;
    int type_item=s8_type_boost_parse(S8_ITEMS[table_id].id);
    if(type_item) return type_item;
    int resist_berry=s8_resist_berry_parse(S8_ITEMS[table_id].id);
    if(resist_berry) return resist_berry;
    int status_item=s8_status_item_parse(S8_ITEMS[table_id].id);
    if(status_item) return status_item;
    int duration_item=s8_duration_item_parse(S8_ITEMS[table_id].id);
    if(duration_item) return duration_item;
    int seed=s8_seed_parse(S8_ITEMS[table_id].id);
    if(seed) return seed;
    int misc=s8_misc_item_parse(S8_ITEMS[table_id].id);
    if(misc) return misc;
    int switch_item=s8_switch_item_parse(S8_ITEMS[table_id].id);
    if(switch_item) return switch_item;
    return -1;
}
static int s8_ability_projection(int table_id) {
    const char *key=S8_ABILITIES[table_id].id;
    if(!strcmp(key,"clearbody")) return AB_CB;
    if(!strcmp(key,"lightmetal")) return AB_LM;
    if(s8_power_implemented(table_id) || s8_resist_implemented(table_id) || s8_hit_implemented(table_id) || s8_property_implemented(table_id) || s8_guard_implemented(table_id) || s8_boost_implemented(table_id) || s8_field_implemented(table_id) || s8_weather_implemented(table_id) || s8_form_implemented(table_id) || s8_items_implemented(table_id) || s8_switch_implemented(table_id)) return AB_NONE;
    if(!strcmp(key,"toughclaws")) return AB_NONE; // legacy Mega-only mechanics
    return -1;
}
static void s8_calculate_stats(const Mon *m, int species_id, int stats[6]) {
    const S8Species *s=&S8_SPECIES[species_id];
    for(int i=0;i<6;i++) {
        int value=(2*s->base_stats[i]+31)/2+(i?5:60)+m->points[i];
        if(i && i==m->nature_up) value=value*110/100;
        if(i && i==m->nature_down) value=value*90/100;
        stats[i]=value;
    }
}
static void s8_set_form(Mon *m, int species_id, int ability_id) {
    const S8Species *s=&S8_SPECIES[species_id];
    m->table_species_id=species_id; m->table_ability_id=ability_id;
    m->type1=s->types[0]; m->type2=s->types[1]; m->added_type=0; m->weight_hg=s->weight_hg;
    m->ability_changed=0;
    m->ability=s8_ability_projection(ability_id);
    // Preserve only the existing Metagross Light Metal effect in this unit.
    if(species_id==S8_METAGROSS && m->ability==AB_LM) m->weight_hg/=2;
    if(OBS && OBS->version==3) {
        obs_emit(EV3_IDENTITY,m,m->table_base_id,species_id,m->mega);
        obs_emit(EV3_TYPES,m,m->type1,m->type2,m->added_type);
    }
}
static int s8_number(const char *s, int max, const char *label) {
    int n=0;
    if(!*s) s8_error(label,s);
    for(const char *p=s;*p;p++) {
        if(*p<'0' || *p>'9') s8_error(label,s);
        n=n*10+(*p-'0'); if(n>max) s8_error(label,s);
    }
    return n;
}
static void s8_numbers(char *s, int *values, int count, int max, const char *label) {
    for(int i=0;i<count;i++) {
        char *comma=strchr(s,',');
        if((i<count-1)!=!!comma) s8_error(label,s);
        if(comma) *comma=0;
        values[i]=s8_number(s,max,label);
        if(comma) s=comma+1;
    }
}
static void s8_init_mon(Mon *m, const char *spec) {
    char buf[512];
    if(strlen(spec)>=sizeof(buf)) s8_error("spec too long","");
    strcpy(buf,spec);
    char *fields[8], *next=buf;
    for(int i=0;i<8;i++) {
        fields[i]=next;
        char *bar=strchr(next,'|');
        if((i<7)!=!!bar) s8_error("expected exactly eight nonempty fields",spec);
        if(bar) { *bar=0; next=bar+1; }
        if(!*fields[i]) s8_error("empty required field",spec);
    }
    char *colon=strchr(fields[0],':');
    if(!colon) s8_error("missing base species",fields[0]);
    *colon=0;
    int source=s8_species_id(fields[0]);
    if(source<0 || !S8_SPECIES[source].initial || S8_SPECIES[source].form_kind!=S8_FORM_NORMAL)
        s8_error("expected legal normal-form species key, not a Mega/battle/helper row",fields[0]);
    int sex=!strcmp(fields[7],"m")?1:!strcmp(fields[7],"f")?2:!strcmp(fields[7],"n")?4:0;
    // The canonical key stays meowstic; the auxiliary female row is reached by
    // its base link, and its own stone link selects the female Mega target.
    if(source==s8_species_id("meowstic") && sex==2) {
        int female=s8_species_id("meowsticf");
        if(female<0 || S8_SPECIES[female].base_id!=source) s8_error("missing female base link",fields[0]);
        source=female;
    }
    const S8Species *normal=&S8_SPECIES[source];
    if(!sex || !(normal->sex_mask & sex)) s8_error("illegal sex for species",fields[7]);
    m->spec_v3=1; m->sex=sex; m->table_base_id=normal->base_id;
    m->table_mega_target=-1; m->table_item_id=-1;
    s8_numbers(colon+1,m->points,6,32,"points must be six integers in 0..32");
    int total=0; for(int i=0;i<6;i++) total+=m->points[i];
    if(total!=66) s8_error("points must total exactly 66",spec);
    int nature[2]; s8_numbers(fields[6],nature,2,5,"nature must be 0,0 or distinct indices 1..5");
    if((!nature[0])!=(!nature[1]) || (nature[0] && nature[0]==nature[1])) s8_error("invalid nature",spec);
    m->nature_up=nature[0]; m->nature_down=nature[1];
    m->mega_mode=s8_number(fields[4],2,"mega must be 0/1/2"); m->mega=m->mega_mode==1;
    // Canonical item IDs; none is a deliberate no-item sentinel outside the table.
    // Old aliases are confined to the legacy grammar, including stone/cb/lm.
    if(strcmp(fields[3],"none")) {
        m->table_item_id=s8_item_id(fields[3]);
        if(m->table_item_id<0) s8_error("unknown item ID (use canonical key)",fields[3]);
    }
    if(m->table_item_id>=0 && S8_ITEMS[m->table_item_id].is_stone) {
        for(int i=0;i<normal->mega_count;i++) {
            const S8MegaLink *link=&S8_MEGA_LINKS[normal->mega_offset+i];
            if(link->stone==m->table_item_id) m->table_mega_target=link->target;
        }
        if(m->table_mega_target<0) s8_error("stone does not correspond to species/sex",fields[3]);
    }
    if(m->mega_mode && m->table_mega_target<0) s8_error("mega=1/2 requires a corresponding stone and Mega target",fields[3]);
    int start=m->mega?m->table_mega_target:source;
    const S8Species *form=&S8_SPECIES[start];
    int ability=s8_ability_id(fields[5]), candidate=0;
    for(int i=0;i<form->ability_count;i++) candidate|=ability==S8_SPECIES_ABILITIES[form->ability_offset+i];
    if(ability<0 || !candidate) s8_error("ability is not a candidate of the starting form",fields[5]);
    s8_set_form(m,start,ability);
    s8_calculate_stats(m,start,m->stats); m->maxhp=m->hp=m->stats[0];
    if(m->mega_mode==2) {
        s8_calculate_stats(m,m->table_mega_target,m->mega_stats); m->mega_stats_valid=1;
    }
    m->item=s8_item_projection(m->table_item_id);
    m->crit_bonus=m->item==ITEM_LENS;
    m->choice_lock=m->last_move=-1;
    const char *policies[]={"mirror","noise","fast","stall","generic","obsdemo","obsh2","learned","obsdemo2","obsh3","learned2","obsdemo3","obsh4"};
    int valid_policy=0;
    for(int i=0;i<=POL_OBSH4;i++) if(!strcmp(fields[2],policies[i])) valid_policy=1;
    if(!valid_policy) s8_error("unknown policy",fields[2]);
    m->policy=parse_policy(fields[2]);
    next=fields[1];
    for(;;) {
        char *comma=strchr(next,','); if(comma) *comma=0;
        int move=-1;
        for(int i=0;i<S8_MOVES_COUNT;i++) if(!strcmp(next,S8_MOVES[i].id) || !strcmp(next,S8_MOVES[i].code)) move=i;
        if(move<0 || move==S8_STRUGGLE) s8_error("unknown/unselectable move",next);
        if(m->nmoves==4) s8_error("one to four distinct moves required",fields[1]);
        for(int i=0;i<m->nmoves;i++) if(m->table_moves[i]==move) s8_error("duplicate move",next);
        // Learnset enforcement belongs to construction, independently of whether
        // a move's effect is implemented for battle yet.
        int learned=0;
        for(int i=0;i<normal->move_count;i++) learned|=move==S8_LEARNSETS[normal->move_offset+i];
        if(!learned) s8_error("move is outside species learnset",next);
        int slot=m->nmoves++;
        m->table_moves[slot]=move; m->moves[slot]=move_executable(move)?move:-1; m->pp[slot]=S8_MOVES[move].pp;
        if(!comma) break;
        next=comma+1;
    }
}
static void s8_require_battle(const Mon *a, const Mon *b) {
    const Mon *mons[]={a,b};
    if((a->spec_v3 || b->spec_v3) &&
       (a->spec_v3?a->table_base_id:S8_METAGROSS)!=(b->spec_v3?b->table_base_id:S8_METAGROSS)) {
        fprintf(stderr,"battle rejected: same base species mirror required\n"); exit(2);
    }
    for(int i=0;i<2;i++) {
        const Mon *m=mons[i];
        if(m->state_features & MON_UNKNOWN_VOCAB) {
            fprintf(stderr,"battle rejected: unimplemented/unknown legacy vocabulary\n"); exit(2);
        }
        if(!m->spec_v3) continue;
        for(int j=0;j<m->nmoves;j++) if(!move_executable(m->moves[j])) {
            fprintf(stderr,"battle rejected: unimplemented move %s\n",S8_MOVES[m->table_moves[j]].id); exit(2);
        }
        if(m->ability<0) {
            fprintf(stderr,"battle rejected: unimplemented ability %s\n",S8_ABILITIES[m->table_ability_id].id); exit(2);
        }
        if(m->item<0) {
            fprintf(stderr,"battle rejected: unimplemented item %s\n",S8_ITEMS[m->table_item_id].id); exit(2);
        }
        if(m->mega_mode==2) {
            const S8Species *target=&S8_SPECIES[m->table_mega_target];
            int ability=S8_SPECIES_ABILITIES[target->ability_offset];
            if(s8_ability_projection(ability)<0) {
                fprintf(stderr,"battle rejected: unimplemented Mega ability %s\n",S8_ABILITIES[ability].id); exit(2);
            }
        }
    }
}

// Stats in mode=2 are NORMAL-form level-50 final stats. Optional |up,down
// (indices A=1..S=5, 0,0 neutral) fixes the nature. Otherwise choose the first
// legal nature, neutral first then up/down lexicographic, preferring total=66.
// All base-stat deltas are multiples of 10; the nature-adjusted delta is exact,
// including the otherwise ambiguous inverse of a hindering nature.
static void mega_prepare_stats(Mon *m, const char *nature) {
    int explicit_up=-1, explicit_down=-1;
    if(nature) {
        char tail;
        if(sscanf(nature,"%d,%d%c",&explicit_up,&explicit_down,&tail)!=2 ||
           explicit_up<0 || explicit_up>5 || explicit_down<0 || explicit_down>5 ||
           ((explicit_up==0)!=(explicit_down==0)) ||
           (explicit_up && explicit_up==explicit_down)) {
            fprintf(stderr,"invalid mega=2 nature; use 0,0 or distinct indices 1..5\n"); exit(2);
        }
    }
    for(int exact=1;exact>=0;exact--) for(int n=0;n<=25;n++) {
        int up=n?(n-1)/5+1:0, down=n?(n-1)%5+1:0;
        if(n && up==down) continue;
        if(nature && (up!=explicit_up || down!=explicit_down)) continue;
        int total=0, max_total=0, valid=1;
        for(int i=0;i<6;i++) {
            int least=33, most=-1, core=OBS_BASE[0][i]+(i?20:75);
            for(int points=0;points<=32;points++) {
                int v=core+points;
                if(i && i==up) v=v*11/10;
                if(i && i==down) v=v*9/10;
                if(v==m->stats[i]) { if(least==33) least=points; most=points; }
            }
            if(least==33) valid=0;
            total+=least; max_total+=most;
        }
        if(!valid || total>66 || (exact && max_total<66)) continue;
        for(int i=0;i<6;i++) {
            int delta=OBS_BASE[1][i]-OBS_BASE[0][i];
            if(i && i==up) delta=delta*11/10;
            if(i && i==down) delta=delta*9/10;
            m->mega_stats[i]=m->stats[i]+delta;
        }
        m->mega_stats_valid=1; return;
    }
    fprintf(stderr,"invalid mega=2 normal stats/nature: no legal 0..32 allocation with total <=66\n"); exit(2);
}

static void init_mon(Mon *m, const char *spec) {
    if(field_member(m)) FIELD=(BattleField){0};
    char buf[512]; strncpy(buf, spec, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    memset(m, 0, sizeof(*m));
    if(strchr(spec,':')) { s8_init_mon(m,spec); return; }
    char *stats_s = strtok(buf, "|");
    char *moves_s = strtok(NULL, "|");
    char *pol_s = strtok(NULL, "|");
    char *item_s = strtok(NULL, "|");
    char *mega_s = strtok(NULL, "|");
    char *ab_s = strtok(NULL, "|");
    char *nature_s = strtok(NULL, "|");
    int v[6]; char *p = stats_s;
    for (int i = 0; i < 6; i++) { v[i] = (int)strtol(p, &p, 10); if (*p == ',') p++; }
    for (int i = 0; i < 6; i++) m->stats[i] = v[i];
    m->maxhp = m->hp = v[0];
    m->nmoves = 0;
    if (moves_s) {
        char *tok = strtok(moves_s, ",");
        while (tok) {
            int mv = move_from_code(tok);
            if (mv < 0) {
                static int warned = 0;
                if (!warned) { warned = 1; fprintf(stderr, "warning: unknown move code \"%s\" ignored\n", tok); }
            }
            if (mv < 0 || m->nmoves >= 4) m->state_features |= MON_UNKNOWN_VOCAB;
            if (mv >= 0 && m->nmoves < 4) { m->moves[m->nmoves] = mv; m->pp[m->nmoves] = MOVES[mv].pp; m->nmoves++; }
            tok = strtok(NULL, ",");
        }
    }
    m->policy = pol_s ? parse_policy(pol_s) : POL_MIRROR;
    m->item = item_s ? parse_item(item_s) : ITEM_NONE;
    if(item_s) {
        int known=s8_type_boost_parse(item_s)!=ITEM_NONE || s8_resist_berry_parse(item_s)!=ITEM_NONE || s8_status_item_parse(item_s)!=ITEM_NONE || s8_duration_item_parse(item_s)!=ITEM_NONE || s8_seed_parse(item_s)!=ITEM_NONE || s8_misc_item_parse(item_s)!=ITEM_NONE || s8_switch_item_parse(item_s)!=ITEM_NONE;
        for(int i=0;i<=ITEM_HERB;i++) known|=!strcmp(item_s,S8_LEGACY_ITEM_TOKENS[i]);
        if(!known) m->state_features|=MON_UNKNOWN_VOCAB;
    }
    m->mega = mega_s ? atoi(mega_s) : 0;
    const S8Species *species=&S8_SPECIES[m->mega==1?S8_METAGROSS_MEGA:S8_METAGROSS];
    m->type1=species->types[0]; m->type2=species->types[1];
    m->mega_mode=m->mega;
    if(m->mega_mode==2) m->mega=0;
    m->choice_lock = -1;
    m->last_move = -1;
    m->crit_bonus = (m->item == ITEM_LENS) ? 1 : 0;
    m->ability = AB_NONE;
    if (ab_s && !m->mega) {   // Mega's ability is fixed to Tough Claws (via the mega flag)
        if (!strcmp(ab_s, "cb")) m->ability = AB_CB;
        else if (!strcmp(ab_s, "lm")) m->ability = AB_LM;
        else {
            if(strcmp(ab_s,"none")) m->state_features|=MON_UNKNOWN_VOCAB;
            static int warned = 0;
            if (!warned) { warned = 1; fprintf(stderr, "warning: unknown ability token \"%s\" treated as none\n", ab_s); }
        }
    }
    if(m->mega_mode==2) {
        mega_prepare_stats(m,nature_s);
        if(m->item!=ITEM_STONE) fprintf(stderr,"warning: mega=2 requires stone; Mega Evolution disabled\n");
    }
}

static uint64_t hash64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static uint64_t matchup_seed(uint64_t seed,long game) {
    return hash64(seed ^ ((uint64_t)(game + 1) * 0xD1B54A32D192ED03ULL));
}

#include "obsh4_diagnostics.h"

static void run_matchup(const char *specA, const char *specB, long n, uint64_t seed,
                        const char *label) {
    long wA = 0, wB = 0, dr = 0; long long turns = 0;
    for (long g = 0; g < n; g++) {
        Mon A, B; init_mon(&A, specA); init_mon(&B, specB);
        // Per-game state = hash(seed, g): starting states must be decorrelated,
        // otherwise streams are shifted windows of one arithmetic sequence.
        Rng r = { matchup_seed(seed,g) };
        int t = 0; int res = battle(&A, &B, &r, &t);
        turns += t;
        if (res == 0) wA++; else if (res == 1) wB++; else dr++;
    }
    printf("%s: A %ld - B %ld (引分 %ld) 平均%.1fターン\n", label, wA, wB, dr, (double)turns / (double)n);
    fflush(stdout);
}

// Validation presets (final stats from mirror_sim5.py)
static const char *SPEC_AI      = "187,165,211,112,130,142|ID,BP,REST,SB|mirror|none|1";
static const char *SPEC_B       = "187,165,211,112,142,130|ID,BP,REST,SB|mirror|none|1";
static const char *SPEC_C       = "187,165,198,112,142,142|ID,BP,REST,SB|mirror|none|1";
static const char *SPEC_F       = "187,165,211,112,142,130|ID,BP,REST,PN|noise|none|1";
static const char *SPEC_G       = "187,165,198,112,142,142|BP,SB,PU,REST|mirror|none|1";
static const char *SPEC_A0      = "157,197,170,112,130,178|EQ,PU,BP,REST|fast|none|1";
static const char *SPEC_SETSUNA = "187,155,179,103,131,90|CP,REST,ST,SCL|stall|helmet|0";
static const char *SPEC_ORIG    = "187,165,211,112,142,130|ID,BP,EQ,REST|mirror|none|1";


// ---------- Stage 8f: console progress for screen/screenx ----------
// stderr only: stdout formats (screen/screenx/matrix) are unchanged.
static long prog_count_lines(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = 0; int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}
static void prog_report(const char *tag, long done, long total, clock_t t0) {
    double el = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (total > 0) {
        double eta = done > 0 ? el * (double)(total - done) / (double)done : 0.0;
        fprintf(stderr, "[progress] %s %ld/%ld (%.1f%%) elapsed %.0fs eta %.0fs\n",
                tag, done, total, 100.0 * (double)done / (double)total, el, eta);
    } else {
        fprintf(stderr, "[progress] %s %ld done, elapsed %.0fs\n", tag, done, el);
    }
    fflush(stderr);
}

int main(int argc, char **argv) {
    if(argc>=2 && (!strcmp(argv[1],"diagnose-obsh4") || !strcmp(argv[1],"compare-obsh4")))
        return obsh4_diagnostic_cli(argc,argv);
    if (argc == 2 && !strcmp(argv[1], "policy-version")) {
        printf("schema=%s encoder=%s weights_sha256=%s\n",LEARNED_SCHEMA,LEARNED_ENCODER,LEARNED_SHA256);
        printf("policy=learned2 schema=%s encoder=%s weights_sha256=%s\n",LEARNED2_SCHEMA,LEARNED2_ENCODER,LEARNED2_SHA256);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "trace") && argc == 5) {
        Mon A, B; init_mon(&A, argv[2]); init_mon(&B, argv[3]);
        Rng r = { strtoull(argv[4], NULL, 10) };
        battle_trace(&A, &B, &r);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "battle") && argc == 6) {
        run_matchup(argv[2], argv[3], strtol(argv[4], NULL, 10), strtoull(argv[5], NULL, 10), "battle");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "screen") && argc == 6) {
        // screen <cand_file> <pool_file> <games> <seed>: every candidate vs every pool member.
        FILE *pf = fopen(argv[3], "r");
        if (!pf) { fprintf(stderr, "cannot open pool file\n"); return 1; }
        static char pool[512][512]; int np = 0;
        char line[512];
        while (fgets(line, sizeof line, pf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            if (np >= 512) { fprintf(stderr, "pool file exceeds maximum 512 builds\n"); fclose(pf); return 1; }
            strcpy(pool[np++], line);
        }
        fclose(pf);
        FILE *cf = fopen(argv[2], "r");
        if (!cf) { fprintf(stderr, "cannot open cand file\n"); return 1; }
        long games = strtol(argv[4], NULL, 10);
        uint64_t seed = strtoull(argv[5], NULL, 10);
        long prog_total = prog_count_lines(argv[2]); clock_t prog_t0 = clock();
        long idx = 0;
        while (fgets(line, sizeof line, cf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            long W = 0, L = 0, D = 0;
            for (int p = 0; p < np; p++) {
                for (long g = 0; g < games; g++) {
                    Mon A, B; init_mon(&A, line); init_mon(&B, pool[p]);
                    uint64_t pi = (uint64_t)(idx * np + p);
                    Rng r = { hash64(seed ^ ((pi * 1000003ULL + (uint64_t)g + 1) * 0xD1B54A32D192ED03ULL)) };
                    int t = 0; int res = battle(&A, &B, &r, &t);
                    if (res == 0) W++; else if (res == 1) L++; else D++;
                }
            }
            printf("%ld %ld %ld %ld\n", idx, W, L, D);
            fflush(stdout);
            idx++;
            if ((idx & 1023) == 0) prog_report("screen", idx, prog_total, prog_t0);
        }
                prog_report("screen", idx, prog_total, prog_t0);
        fclose(cf);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "screenx") && argc == 6) {
        // screenx: like screen, but prints per-pool-member W L D on one line per candidate.
        FILE *pf = fopen(argv[3], "r");
        if (!pf) { fprintf(stderr, "cannot open pool file\n"); return 1; }
        static char pool[512][512]; int np = 0;
        char line[512];
        while (fgets(line, sizeof line, pf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            if (np >= 512) { fprintf(stderr, "pool file exceeds maximum 512 builds\n"); fclose(pf); return 1; }
            strcpy(pool[np++], line);
        }
        fclose(pf);
        FILE *cf = fopen(argv[2], "r");
        if (!cf) { fprintf(stderr, "cannot open cand file\n"); return 1; }
        long games = strtol(argv[4], NULL, 10);
        uint64_t seed = strtoull(argv[5], NULL, 10);
        long prog_total = prog_count_lines(argv[2]); clock_t prog_t0 = clock();
        long idx = 0;
        while (fgets(line, sizeof line, cf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            printf("%ld", idx);
            for (int p = 0; p < np; p++) {
                long W = 0, L = 0, D = 0;
                for (long g = 0; g < games; g++) {
                    Mon A, B; init_mon(&A, line); init_mon(&B, pool[p]);
                    uint64_t pi = (uint64_t)(idx * np + p);
                    Rng r = { hash64(seed ^ ((pi * 1000003ULL + (uint64_t)g + 1) * 0xD1B54A32D192ED03ULL)) };
                    int t = 0; int res = battle(&A, &B, &r, &t);
                    if (res == 0) W++; else if (res == 1) L++; else D++;
                }
                printf(" %ld %ld %ld", W, L, D);
            }
            printf("\n");
            fflush(stdout);
            idx++;
            if ((idx & 1023) == 0) prog_report("screenx", idx, prog_total, prog_t0);
        }
                prog_report("screenx", idx, prog_total, prog_t0);
        fclose(cf);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "matrix")) {
        long n = argc >= 3 ? strtol(argv[2], NULL, 10) : 3000;
        uint64_t seed = argc >= 4 ? strtoull(argv[3], NULL, 10) : 12345;
        run_matchup(SPEC_B, SPEC_AI, n, seed, "候補B vs AI型     (sim5: 194-106)");
        run_matchup(SPEC_C, SPEC_AI, n, seed, "候補C vs AI型     (sim5: 242-58)");
        run_matchup(SPEC_F, SPEC_AI, n, seed, "候補F vs AI型     (sim5: 5-295)");
        run_matchup(SPEC_C, SPEC_A0, n, seed, "候補C vs A0型     (sim5: 147-153)");
        run_matchup(SPEC_C, SPEC_SETSUNA, n, seed, "候補C vs せつない型 (sim5: 42-258)");
        run_matchup(SPEC_B, SPEC_SETSUNA, n, seed, "候補B vs せつない型 (sim5: 50-250)");
        run_matchup(SPEC_G, SPEC_AI, n, seed, "候補G vs AI型     (sim5: 204-96)");
        run_matchup(SPEC_G, SPEC_A0, n, seed, "候補G vs A0型     (sim5: 0-300)");
        run_matchup(SPEC_G, SPEC_SETSUNA, n, seed, "候補G vs せつない型 (sim5: 138-162)");
        run_matchup(SPEC_ORIG, SPEC_AI, n, seed, "元の型 vs AI型    (sim5: 11-289)");
        run_matchup(SPEC_ORIG, SPEC_A0, n, seed, "元の型 vs A0型    (sim5: 174-126)");
        run_matchup(SPEC_ORIG, SPEC_SETSUNA, n, seed, "元の型 vs せつない型 (sim5: 76-224)");
        run_matchup(SPEC_A0, SPEC_SETSUNA, n, seed, "A0型 vs せつない型  (sim5: 173-127)");
        run_matchup(SPEC_A0, SPEC_AI, n, seed, "A0型 vs AI型      (sim5: 115-185)");
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("usage:\n"
               "  engine battle <specA> <specB> <n> <seed>\n"
               "  engine matrix [n] [seed]\n"
               "  engine screen <cand_file> <pool_file> <games> <seed>\n"
               "  engine screenx <cand_file> <pool_file> <games> <seed>\n"
               "  engine trace <specA> <specB> <seed>\n"
               OBSH4_DIAG_USAGE
               "  engine policy-version\n");
        return 0;
    }
    fprintf(stderr,
            "usage:\n"
            "  engine battle <specA> <specB> <n> <seed>\n"
            "  engine matrix [n] [seed]\n"
            "  engine screen <cand_file> <pool_file> <games> <seed>\n"
            "  engine screenx <cand_file> <pool_file> <games> <seed>\n"
            "  engine trace <specA> <specB> <seed>\n"
            OBSH4_DIAG_USAGE
            "  engine policy-version\n");
    return 1;
}
