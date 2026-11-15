// Pokémon Champions 1on1 battle engine (fast C core)
// Scope: 68 Metagross mirror moves (Stage 6 G0–G5) + Struggle.
// Round chaining uses the committed action queue. MGO1 and the v1/g12 pilot calculations remain frozen.
// Items: the 33 Champions-implemented candidates verified for Reg M-C
// (see parse_item for codes) plus none/stone (Mega).
// Mechanics follow the mainline where the Python sim was silent, and follow the
// Python sim where it made explicit choices (documented per-move below).
// Build: gcc -O3 -march=native -o engine engine.c -lm
// Usage:
//   ./engine battle "<specA>" "<specB>" <n> <seed>
//   ./engine matrix <n> <seed>
// spec: "HP,A,B,C,D,S|MV,MV,MV,MV|policy|item|mega"
//   moves: BP BD EQ SCL SB MS AGI CP ID PRO PU REST SF ST SUB TRK PN
//          AA SP PC SE BUL RND BSW PSH RS IH ZH SN RT IW PSY FC MM TB HA
//          IP TP BSL SLB SWG FAC
//   policy: mirror noise fast stall generic obsdemo obsh2 learned obsdemo2 obsh3 learned2; mega: 0/1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

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
// Full Gen VI+ chart, in enum order; stage8-prep/type_chart.json.
static const double TYPE_CHART[18][18] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.5, 0, 1, 1, 0.5, 1}, // Normal
    {1, 0.5, 0.5, 1, 2, 2, 1, 1, 1, 1, 1, 2, 0.5, 1, 0.5, 1, 2, 1}, // Fire
    {1, 2, 0.5, 1, 0.5, 1, 1, 1, 2, 1, 1, 1, 2, 1, 0.5, 1, 1, 1}, // Water
    {1, 1, 2, 0.5, 0.5, 1, 1, 1, 0, 2, 1, 1, 1, 1, 0.5, 1, 1, 1}, // Electric
    {1, 0.5, 2, 1, 0.5, 1, 1, 0.5, 2, 0.5, 1, 0.5, 2, 1, 0.5, 1, 0.5, 1}, // Grass
    {1, 0.5, 0.5, 1, 2, 0.5, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 0.5, 1}, // Ice
    {2, 1, 1, 1, 1, 2, 1, 0.5, 1, 0.5, 0.5, 0.5, 2, 0, 1, 2, 2, 0.5}, // Fighting
    {1, 1, 1, 1, 2, 1, 1, 0.5, 0.5, 1, 1, 1, 0.5, 0.5, 1, 1, 0, 2}, // Poison
    {1, 2, 1, 2, 0.5, 1, 1, 2, 1, 0, 1, 0.5, 2, 1, 1, 1, 2, 1}, // Ground
    {1, 1, 1, 0.5, 2, 1, 2, 1, 1, 1, 1, 2, 0.5, 1, 1, 1, 0.5, 1}, // Flying
    {1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 0.5, 1, 1, 1, 1, 0, 0.5, 1}, // Psychic
    {1, 0.5, 1, 1, 2, 1, 0.5, 0.5, 1, 0.5, 2, 1, 1, 0.5, 1, 2, 0.5, 0.5}, // Bug
    {1, 2, 1, 1, 1, 2, 0.5, 1, 0.5, 2, 1, 2, 1, 1, 1, 1, 0.5, 1}, // Rock
    {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 2, 1, 0.5, 1, 1}, // Ghost
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 0.5, 0}, // Dragon
    {1, 1, 1, 1, 1, 1, 0.5, 1, 1, 1, 2, 1, 1, 2, 1, 0.5, 1, 0.5}, // Dark
    {1, 0.5, 0.5, 0.5, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 0.5, 2}, // Steel
    {1, 0.5, 1, 1, 1, 1, 2, 0.5, 1, 1, 1, 1, 1, 1, 2, 2, 0.5, 1}, // Fairy
};
static double eff_vs(int atk, int def) {
    return TYPE_CHART[atk][def];
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
    int count=version==1?V1_MOVE_COUNT:version==2?V2_MOVE_COUNT:0;
    if(!count) return -1;
    if(action.kind==ACTION_STRUGGLE) return version==1?V1_STRUGGLE_WIRE:V2_STRUGGLE_WIRE;
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
enum { WEATHER_NONE, WEATHER_SUN, WEATHER_RAIN, WEATHER_SAND };
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
static int move_from_code(const char *s) {
    for (int i = 0; i < MV_COUNT; i++) if (strcmp(s, MOVES[i].code) == 0) return i;
    return -1;
}

// ---------- Battler ----------
enum { POL_MIRROR, POL_NOISE, POL_FAST, POL_STALL, POL_GENERIC, POL_OBSDEMO, POL_OBSH2, POL_LEARNED, POL_OBSDEMO2, POL_OBSH3, POL_LEARNED2 };
static int is_v2_policy(int policy) { return policy==POL_OBSDEMO2 || policy==POL_OBSH3 || policy==POL_LEARNED2; }
static int is_observable_policy(int policy) { return policy==POL_OBSDEMO || policy==POL_OBSH2 || policy==POL_LEARNED || is_v2_policy(policy); }
enum { AB_NONE, AB_CB, AB_LM };   // abilities: none / Clear Body / Light Metal (Mega = Tough Claws via the mega flag)
enum { ITEM_NONE, ITEM_HELMET, ITEM_STONE,
       ITEM_LEFTOVERS, ITEM_SITRUS, ITEM_SCARF, ITEM_LIFEORB, ITEM_MUSCLE,
       ITEM_WISE, ITEM_BELT, ITEM_TAG, ITEM_SAND, ITEM_SPOON, ITEM_BBELT,
       ITEM_METRO, ITEM_SHELLBELL, ITEM_LENS, ITEM_KINGS, ITEM_WIDE,
       ITEM_ZOOM, ITEM_POWDER, ITEM_CLAW, ITEM_IRONBALL, ITEM_SHUCA,
       ITEM_CHOPLE, ITEM_KASIB, ITEM_PAYAPA, ITEM_CHESTO, ITEM_LUM,
       ITEM_LEPPA, ITEM_ORAN, ITEM_SASH, ITEM_FBAND, ITEM_BALLOON, ITEM_HERB };
enum { MON_UNKNOWN_VOCAB=1u, MON_POST_V1_STATE=2u };
typedef struct {
    int stats[6];          // H A B C D S (final values)
    int hp, maxhp;
    int st[5];             // stages A B C D S
    int acc;               // accuracy stage
    int sleep, healblock;
    int sub_hp;
    int protect_streak, protected_now;
    int item, mega;
    int ability;      // AB_*; only Clear Body has an effect within the 17-move pool
    int item_used;    // one-shot item state; travels with the item through Trick
    int choice_lock;  // move code locked by Choice Scarf; -1 = none
    int last_move;    // last move code used (Metronome item streak); -1 = none
    int metro_streak; // consecutive prior uses of last_move
    int flinched;     // set by native effects or King's Rock; checked when this mon's action comes
    int crit_bonus;   // kept in sync with Scope Lens at init; calc reads the item directly
    int type1, type2;
    int moves[4], pp[4], nmoves;
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
    int attempt_prev, attempt_this; // detailed results; zero means no attempt
    int recharge, charging, focusing, focus_lost, round_chained;
    int future_turns; // target slot reservation: 3 -> impact at end of T+2
    unsigned state_features;
} Mon;

// Like OBS, the field belongs to the current serial battle, never to a pilot.
// init_mon invalidates membership before reusing a seat; fixtures bind explicitly.
enum { TERRAIN_NONE, TERRAIN_PSYCHIC, TERRAIN_OTHER };
enum { ATTEMPT_NONE, ATTEMPT_SUCCESS, ATTEMPT_FAIL, ATTEMPT_NOT_FAIL, ATTEMPT_SKIP_FAIL, ATTEMPT_SKIP_NOT_FAIL };
enum { FAINT_DAMAGE, FAINT_SELF_DESTRUCT, FAINT_ATTEMPT_COST, FAINT_RECOIL };
typedef struct { int seat, cause; } FaintEntry;
typedef struct {
    Mon *seat[2]; int weather, weather_turns, gravity, terrain;
    int track_attempts, committed[2], acted[2];
    FaintEntry faint_queue[2]; int faint_count;
} BattleField;
static BattleField FIELD;
static int field_member(const Mon *m) { return FIELD.seat[0]==m || FIELD.seat[1]==m; }
static int current_weather(const Mon *m) { return field_member(m)?FIELD.weather:m->weather; }
static int current_gravity(const Mon *m) { return field_member(m)?FIELD.gravity:0; }
static void field_bind(Mon *a, Mon *b) {
    FIELD=(BattleField){.seat={a,b}};
    // Import only the explicit, legacy harness projection; normal init is neutral.
    FIELD.weather=a->weather?a->weather:b->weather;
    if(FIELD.weather) FIELD.weather_turns=5;
    a->weather=b->weather=FIELD.weather;
    for(int j=0;j<2;j++) for(int i=0;i<FIELD.seat[j]->nmoves;i++)
        if(FIELD.seat[j]->moves[i]>=MV_FP) FIELD.track_attempts=1;
}
static int is_grounded(const Mon *m) {
    if(current_gravity(m) || m->item==ITEM_IRONBALL) return 1;
    if(m->type1==T_FLY || m->type2==T_FLY || m->magnet_rise) return 0;
    return m->item!=ITEM_BALLOON || m->item_used;
}
static int field_v1_unsupported(const Mon *m) {
    return m->attempt_prev || m->attempt_this || m->recharge || m->charging || m->focusing || m->focus_lost || m->round_chained || m->future_turns ||
           m->reflect || m->light_screen || m->stealth_rock || m->magnet_rise || m->endured_now ||
           (field_member(m) && (FIELD.weather || FIELD.gravity || FIELD.terrain));
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
static void queue_faint(Mon *m,int cause) {
    if(m->hp>0 || !field_member(m)) return;
    int seat=m==FIELD.seat[1];
    for(int i=0;i<FIELD.faint_count;i++) if(FIELD.faint_queue[i].seat==seat) return;
    if(FIELD.faint_count>=2) abort();
    // Attempt costs resolve before recipient faint; ordinary post-damage
    // recoil appends after it. This is a resolver queue, not a final-HP rule.
    if(cause==FAINT_ATTEMPT_COST && FIELD.faint_count) {
        FIELD.faint_queue[1]=FIELD.faint_queue[0]; FIELD.faint_queue[0]=(FaintEntry){seat,cause};
    } else FIELD.faint_queue[FIELD.faint_count]=(FaintEntry){seat,cause};
    FIELD.faint_count++;
}
static int ordered_faint_result(void) {
    return FIELD.faint_count?1-FIELD.faint_queue[0].seat:2;
}
static void g5_begin_turn(Mon *a,Mon *b) {
    Mon *ms[]={a,b};
    for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if(g5_tracking(m)) { m->attempt_prev=m->attempt_this; m->attempt_this=ATTEMPT_NONE; }
        m->focusing=m->focus_lost=m->round_chained=0;
        FIELD.committed[i]=-1; FIELD.acted[i]=0;
    }
}
static int g5_priority(const Mon *m,int slot) {
    return m->recharge?0:slot>=0?MOVES[m->moves[slot]].prio:-1;
}
static int g5_sleep_callable(int mv) { return mv!=MV_FP && mv!=MV_MB; }
static void future_residual(Mon *a,Mon *b,Rng *r);
static Rng *BATTLE_RNG;

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
static const int OBS_BASE[2][6] = {{80,135,130,95,90,70},{80,145,150,105,110,110}};
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
       EV_ATTACK_START, EV_ATTACK_END, EV_OWN_MAXHP, EV_FUTURE_TICK };
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
typedef struct {
    Mon *seat[2]; Knowledge view[2]; int seq, turn, profile;
    int version; Knowledge2 v2[2];
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
    case EV_SLEEP: p->sleep=e->x; p->sleep_set=e->x?4:1; break;
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
static void obs_deliver(int viewer, int kind, int seat, int x, int y, int z) {
    ObsEvent e={OBS->seq,OBS->turn,kind,seat==viewer?0:1,x,y,z};
    if(OBS->version==2) obs2_replay(&OBS->v2[viewer],&e);
    else obs_replay(&OBS->view[viewer],&e);
    if(obs_sink) obs_sink(viewer,&e);
}
static void obs_emit(int kind, const Mon *m, int x, int y, int z) {
#ifdef ENGINE_EVENT_SINK
    ENGINE_EVENT_SINK(kind,m,x,y,z);
#endif
    if(!OBS) return;
    int seat=m==OBS->seat[1]; ++OBS->seq;
    for(int v=0;v<2;v++) obs_deliver(v,kind,seat,x,y,z);
}
static void obs_emit_action(int kind, const Mon *m, MoveAction action, int called) {
#ifdef ENGINE_EVENT_SINK
    if(!OBS) { obs_emit(kind,m,action_wire_id(action,2),called,0); return; }
#endif
    if(!OBS) return;
    int wire=action_wire_id(action,OBS->version==2?2:1);
    if(OBS->version==2 && kind==EV_USE && action.kind==ACTION_MOVE && action.move==MV_MB && m->charging) called=3;
    if(wire<0) { fprintf(stderr,"MGO1/g12 rejects post-v1 action\n"); exit(2); }
    obs_emit(kind,m,wire,called,0);
}
// Sanitize at source, before creating either public event. hp before/after are
// local resolution values, never retained by a shared event object.
static void obs_hp(const Mon *m, int before, int cause) {
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
    if(OBS && OBS->version==2) obs_emit(kind,m,x,y,z);
}
static void obs2_cancel_charge(const Mon *m) {
    // A known charge ending is visible; no new submitted choice is disclosed.
    obs2_emit(EV_CHARGE_END,m,0,0,0);
}
static void obs_item(const Mon *m, int consumed) { obs_emit(EV_ITEM,m,m->item,consumed?2:1,0); }
static void obs_mask(const Mon *m, int terminal, ActionMask *mask) {
    memset(mask,0,sizeof(*mask)); if(terminal) return;
    if(m->item==ITEM_SCARF && m->choice_lock>=0) {
        int i=find_move(m,m->choice_lock);
        if(i>=0 && m->pp[i]>0) { mask->legal[i]=1; return; }
    }
    int any=0;
    for(int i=0;i<m->nmoves;i++) if(m->pp[i]>0) any=mask->legal[i]=1;
    mask->legal[4]=!any;
}
static int mon_v1_supported(const Mon *m) {
    if(field_v1_unsupported(m) || m->state_features || m->status || m->freeze_turns || m->toxic_stage || m->confusion || m->weather || m->nmoves<1 || m->nmoves>4 ||
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


// ---------- Item helpers ----------
// Mainline-style fixed-point item modifier, rounding exact halves down.
static int item_mod(int value, int numerator) {
    return (int)(((long)value * numerator + 2047) / 4096);
}
static int trick_stone_blocked(const Mon *a, const Mon *b) {
    // A mega flag implies a held Mega Stone even when item stores another code.
    return a->mega || b->mega || a->item == ITEM_STONE || b->item == ITEM_STONE;
}
static double eff_speed(const Mon *m) {
    double s = m->stats[5] * smult(m->st[4]);
    // Once paralysis is active, use the pinned getStat/finalModify integer
    // rounding before its final Speed halving. The legacy no-status path stays exact.
    if(m->status==STATUS_PAR) {
        int speed=(int)floor(s);
        if(m->item==ITEM_SCARF) speed=item_mod(speed,6144);
        if(m->item==ITEM_IRONBALL) speed=item_mod(speed,2048);
        return speed/2;
    }
    if (m->item == ITEM_SCARF) s *= 1.5;        // Choice Scarf
    if (m->item == ITEM_IRONBALL) s *= 0.5;     // Iron Ball
    return s;
}
// Sitrus / Oran Berry: heal once when HP is at or below half (blocked by Heal Block).
static void berry_hp_check(Mon *m) {
    if (m->hp <= 0 || m->item_used || m->healblock > 0) return;
    if (m->item == ITEM_SITRUS && m->hp <= m->maxhp / 2) {
        int before=m->hp; m->hp += m->maxhp / 4; if (m->hp > m->maxhp) m->hp = m->maxhp; m->item_used = 1; obs_item(m,1); obs_hp(m,before,HP_HEAL);
    } else if (m->item == ITEM_ORAN && m->hp <= m->maxhp / 2) {
        int before=m->hp; m->hp += 10; if (m->hp > m->maxhp) m->hp = m->maxhp; m->item_used = 1; obs_item(m,1); obs_hp(m,before,HP_HEAL);
    }
}
// White Herb: once, restore all negative stages (incl. accuracy) to 0.
static void white_herb_check(Mon *m) {
    if (m->item != ITEM_HERB || m->item_used) return;
    int neg = (m->acc < 0);
    for (int i = 0; i < 5; i++) if (m->st[i] < 0) neg = 1;
    if (!neg) return;
    for (int i = 0; i < 5; i++) if (m->st[i] < 0) m->st[i] = 0;
    if (m->acc < 0) m->acc = 0;
    m->item_used = 1; obs_item(m,1); obs_emit(EV_HERB,m,0,0,0);
}
// Leppa Berry: once, when a move's PP reaches 0, restore 10 PP to it.
static void leppa_check(Mon *m, int slot) {
    if (slot < 0 || m->item != ITEM_LEPPA || m->item_used) return;
    if (m->pp[slot] == 0) {
        m->pp[slot] += 10;
        int mx = MOVES[m->moves[slot]].pp;
        if (m->pp[slot] > mx) m->pp[slot] = mx;
        m->item_used = 1; obs_item(m,1); obs_emit(EV_LEPPA,m,m->moves[slot],0,0);
    }
}
// Attacker-side effects of landing a damaging move (apply even vs a substitute).
static void post_hit_attacker(Mon *me, int dmg) {
    if (dmg <= 0 || me->hp <= 0) return;
    if (me->item == ITEM_SHELLBELL && me->healblock == 0) {
        int heal = dmg / 8;                              // Shell Bell: 512/4096 of damage, minimum 1 HP
        if (heal < 1) heal = 1;
        int before=me->hp; me->hp += heal; if (me->hp > me->maxhp) me->hp = me->maxhp;
        if(me->hp!=before) { obs_item(me,0); obs_hp(me,before,HP_HEAL); }
    }
    if (me->item == ITEM_LIFEORB) { int before=me->hp; me->hp -= me->maxhp / 10; obs_item(me,0); obs_hp(me,before,HP_RECOIL); }   // Life Orb recoil
}
// Bookkeeping after a mon actually uses a move: Metronome streak + Choice lock.
// metro_streak counts consecutive uses of last_move INCLUDING the latest one,
// so the next use of the same move boosts by 20% x min(streak, 5).
static void note_move_used(Mon *m, int mv) {
    obs_emit_action(EV_NOTE,m,(MoveAction){ACTION_MOVE,mv},0);
    if (m->last_move == mv) m->metro_streak++;
    else { m->last_move = mv; m->metro_streak = 1; }
    if (m->item == ITEM_SCARF) m->choice_lock = mv;
}
// G3 state never crosses the frozen MGO1 boundary. These helpers are also the
// harness seam for statuses with no source in Metagross's learnset.
static void clear_major_status(Mon *m) {
    if(m->status) obs2_emit(EV_STATUS_END,m,m->status,0,0);
    m->status=STATUS_NONE; m->freeze_turns=0; m->toxic_stage=0;
    if(m->sleep) { m->sleep=0; obs_emit(EV_SLEEP,m,0,0,0); }
}
static void status_berry_check(Mon *m) {
    if(m->hp<=0 || m->item_used) return;
    if(m->item==ITEM_LUM && (m->status || m->sleep || m->confusion)) {
        m->item_used=1; obs_item(m,1); clear_major_status(m); if(m->confusion) obs2_emit(EV_CONFUSION_END,m,0,0,0); m->confusion=0;
    } else if(m->item==ITEM_CHESTO && m->sleep) {
        m->item_used=1; obs_item(m,1); clear_major_status(m);
    }
}
static int has_type(const Mon *m, int type) { return m->type1==type || m->type2==type; }
static int inflict_status(Mon *m, int status, Rng *r) {
    if(m->hp<=0 || m->status || m->sleep) return 0;
    if((status==STATUS_PAR && has_type(m,T_ELEC)) ||
       (status==STATUS_FRZ && (has_type(m,T_ICE) || current_weather(m)==WEATHER_SUN)) ||
       ((status==STATUS_PSN || status==STATUS_TOX) && (has_type(m,T_STEEL) || has_type(m,T_POISON))) ||
       (status==STATUS_BRN && has_type(m,T_FIRE))) return 0;
    if(status<=STATUS_NONE || status>STATUS_BRN) return 0;
    (void)r; // non-sleep major statuses do not draw on Start
    m->status=status; m->freeze_turns=status==STATUS_FRZ?3:0; m->toxic_stage=0;
    m->state_features|=MON_POST_V1_STATE;
    obs2_emit(EV_STATUS_START,m,status,0,0);
    status_berry_check(m); // Lum's AfterSetStatus runs before the next action
    return 1;
}
static int inflict_confusion(Mon *m, Rng *r) {
    if(m->hp<=0 || m->confusion) return 0;
    m->confusion=2+rng_int(r,4); // inherited random(2,6), before expiry decrement
    m->state_features|=MON_POST_V1_STATE;
    obs2_emit(EV_CONFUSION_START,m,0,0,0);
    status_berry_check(m); // Lum's Update also cures confusion
    return 1;
}
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
    if(loss) { int before=m->hp; m->hp-=loss; obs_hp(m,before,HP_RECOIL); berry_hp_check(m); }
}
static void end_of_turn(Mon *A, Mon *B) {
    Mon *ms[2] = {A, B};
    if(field_member(A) && FIELD.weather_turns && !--FIELD.weather_turns) {
        obs2_emit(EV_WEATHER_END,A,0,0,0);
        FIELD.weather=WEATHER_NONE; A->weather=B->weather=WEATHER_NONE;
    }
    if(current_weather(A)==WEATHER_SAND) for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if(m->hp>0 && !has_type(m,T_ROCK) && !has_type(m,T_GROUND) && !has_type(m,T_STEEL)) {
            int loss=m->maxhp/16; if(loss<1) loss=1;
            int before=m->hp; m->hp-=loss; obs_hp(m,before,HP_RECOIL); berry_hp_check(m);
        }
    }
    if(A->hp>0 && B->hp>0 && (A->future_turns || B->future_turns)) future_residual(A,B,BATTLE_RNG);
    for (int i = 0; i < 2; i++) {
        Mon *m = ms[i];
        m->flinched = 0;
        if (m->hp > 0 && m->item == ITEM_LEFTOVERS && m->healblock == 0) {
            int before=m->hp; m->hp += m->maxhp / 16;
            if (m->hp > m->maxhp) m->hp = m->maxhp;
            if(m->hp!=before) { obs_item(m,0); obs_hp(m,before,HP_HEAL); }
        }
    }
    for(int order=9;order<=10;order++) for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if((order==9 && (m->status==STATUS_PSN || m->status==STATUS_TOX)) ||
           (order==10 && m->status==STATUS_BRN)) status_residual(m);
    }
    for(int i=0;i<2;i++) {
        Mon *m=ms[i];
        if(m->magnet_rise && !--m->magnet_rise) obs2_emit(EV_RISE_END,m,0,0,0); // residual 18
        if(m->reflect && !--m->reflect) obs2_emit(EV_SCREEN_END,m,0,0,0); // side residual 26.1
        if(m->light_screen && !--m->light_screen) obs2_emit(EV_SCREEN_END,m,1,0,0); // 26.2
        m->endured_now=0;
    }
    if(field_member(A) && FIELD.gravity && !--FIELD.gravity) obs2_emit(EV_GRAVITY_END,A,0,0,0); // field residual 27.2
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
static int damage_power(const Mon *atk, const MoveDef *mv) {
    int modifier=4096;
    // BasePower event priorities: Tough Claws 21, band/glasses 16, type item 15.
    if(mv->contact && atk->mega) modifier=damage_chain(modifier,5325);
    if((mv->cat==CAT_PHYS && atk->item==ITEM_MUSCLE) ||
       (mv->cat==CAT_SPEC && atk->item==ITEM_WISE)) modifier=damage_chain(modifier,4505);
    if((atk->item==ITEM_TAG && mv->type==T_GHOST) || (atk->item==ITEM_SAND && mv->type==T_GROUND) ||
       (atk->item==ITEM_SPOON && mv->type==T_PSY) || (atk->item==ITEM_BBELT && mv->type==T_FIGHT))
        modifier=damage_chain(modifier,4915);
    // Facade onBasePower priority -2, after item/ability hooks; includes freeze.
    if(mv==&MOVES[MV_FAC] && atk->status && !atk->sleep) modifier=damage_chain(modifier,8192);
    int power=item_mod(mv->power,modifier);
    return power<1?1:power;
}
static double damage_effectiveness(const Mon *def, int mvi) {
    // Frozen legacy behavior: awake ST falls through to a power-0 damage
    // attempt. It historically used the partial chart's neutral Normal row.
    // Preserve that compatibility quirk without altering any real attack row.
    if(mvi==MV_ST) return 1.0;
    if(MOVES[mvi].type==T_GROUND) {
        if(!is_grounded(def)) return 0;
        // Grounded Flying loses its type immunity (Gravity/Iron Ball).
        return (def->type1==T_FLY?1:eff_vs(T_GROUND,def->type1))*
               (def->type2==T_FLY?1:eff_vs(T_GROUND,def->type2));
    }
    return eff_vs(MOVES[mvi].type,def->type1)*eff_vs(MOVES[mvi].type,def->type2);
}
static int damage_berry(const Mon *def, const MoveDef *mv, double eff) {
    // Preserve the legacy eligibility/consumption path, including Substitute.
    return eff>1 && !def->item_used &&
        ((def->item==ITEM_SHUCA && mv->type==T_GROUND) || (def->item==ITEM_CHOPLE && mv->type==T_FIGHT) ||
         (def->item==ITEM_KASIB && mv->type==T_GHOST) || (def->item==ITEM_PAYAPA && mv->type==T_PSY));
}
static int damage_final_modifier(const Mon *atk, int mvi, double eff, int berry, int screen) {
    int modifier=screen;
    if(atk->item==ITEM_LIFEORB) modifier=damage_chain(modifier,5324);
    if(eff>1 && atk->item==ITEM_BELT) modifier=damage_chain(modifier,4915);
    if(atk->item==ITEM_METRO && atk->last_move==mvi) {
        static const int MOD[6]={4096,4915,5734,6553,7372,8192};
        int k=atk->metro_streak>5?5:atk->metro_streak;
        modifier=damage_chain(modifier,MOD[k]);
    }
    if(berry) modifier=damage_chain(modifier,2048);
    return modifier;
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
    if(stab) base=item_mod(base,6144);                   // ModifySTAB, ties down
    base=(int)(base*eff);                               // type: double / truncate halves
    if(hooks->burned) base=item_mod(base,2048);          // burn is its own rounding point
    base=item_mod(base,final_modifier);                 // ModifyDamage hook chain
    return base<1?1:base;
}
// Dataset weights in hectograms: Metagross 550.0 kg; Mega 942.9 kg.
static int mon_weight(const Mon *m) {
    int weight=m->mega?9429:5500;
    if(!m->mega && m->ability==AB_LM) weight/=2;
    return weight;
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
    return MOVES[mv].power;
}
static int LIVE_DAMAGE;
static int resolved_damage_power(const Mon *atk,const Mon *def,int mv) {
    if(!LIVE_DAMAGE) return damage_power(atk,&MOVES[mv]);
    int terrain=field_member(atk) && FIELD.terrain==TERRAIN_PSYCHIC && is_grounded(atk) && MOVES[mv].type==T_PSY;
    int dynamic=mv==MV_TAN || mv==MV_GYRO || mv==MV_HP || mv==MV_GK || (mv==MV_RND && atk->round_chained);
    if(!dynamic && !terrain) return damage_power(atk,&MOVES[mv]);
    MoveDef move=MOVES[mv]; move.power=move_base_power(atk,def,mv);
    if(!terrain) return damage_power(atk,&move);
    int mods=4096;
    if(move.contact && atk->mega) mods=damage_chain(mods,5325);
    if((move.cat==CAT_PHYS && atk->item==ITEM_MUSCLE) || (move.cat==CAT_SPEC && atk->item==ITEM_WISE)) mods=damage_chain(mods,4505);
    if(atk->item==ITEM_SPOON) mods=damage_chain(mods,4915);
    mods=damage_chain(mods,5325); // Psychic Terrain priority 6
    if(mv==MV_EF) mods=damage_chain(mods,6144); // Expanding Force priority 0
    int power=item_mod(move.power,mods); return power>0?power:1;
}

static int weather_damage_modifier(const Mon *def, const MoveDef *mv) {
    int weather=current_weather(def);
    if((weather==WEATHER_SUN && mv->type==T_FIRE) || (weather==WEATHER_RAIN && mv->type==T_WATER)) return 6144;
    if((weather==WEATHER_SUN && mv->type==T_WATER) || (weather==WEATHER_RAIN && mv->type==T_FIRE)) return 2048;
    return 4096;
}
static int damage_value(const Mon *atk, const Mon *def, int mvi, int crit, int roll,
                        const DamageHooks *hooks, int berry) {
    const MoveDef *mv=&MOVES[mvi];
    int ai=mv->cat==CAT_PHYS?(mvi==MV_BP?2:1):3, di=mv->cat==CAT_PHYS?2:4;
    if(MOVE_TRAITS[mvi].defensive_stat) di=MOVE_TRAITS[mvi].defensive_stat;
    int as=atk->st[ai-1], ds=def->st[di-1];
    if(crit && as<0) as=0;
    if(crit && ds>0) ds=0;
    int attack=damage_stat(atk->stats[ai],as), defense=damage_stat(def->stats[di],ds);
    if(di==4 && current_weather(def)==WEATHER_SAND && has_type(def,T_ROCK)) defense=item_mod(defense,6144);
    int power=resolved_damage_power(atk,def,mvi);
    int base=(int)((int64_t)22*power*attack/defense/50)+2;
    double eff=damage_effectiveness(def,mvi);
    if(eff<=0) return 0;
    DamageHooks actual=*hooks;
    if((mv->cat==CAT_PHYS && def->reflect) || (mv->cat==CAT_SPEC && def->light_screen)) actual.screen=2048;
    int weather_mod=weather_damage_modifier(def,mv);
    if(weather_mod!=4096) actual.weather=weather_mod;
    int modifier=damage_final_modifier(atk,mvi,eff,berry,crit?4096:actual.screen);
    if(atk->status==STATUS_BRN && mv->cat==CAT_PHYS && mvi!=MV_FAC) actual.burned=1;
    return damage_modifiers(base,crit,roll,mv->type==atk->type1 || mv->type==atk->type2,eff,&actual,modifier);
}
static double damage_crit_probability(const Mon *atk, int mvi) {
    static const double CRITP[4]={1.0/24.0,1.0/8.0,1.0/2.0,1.0};
    int stage=MOVE_TRAITS[mvi].crit_bonus+(atk->item==ITEM_LENS);
    return CRITP[stage>3?3:stage];
}
static int calc_damage(Mon *atk, Mon *def, const MoveDef *mv, Rng *r) {
    int mvi=(int)(mv-MOVES);
    // Same two draws, crit then roll, as the historical engine. Retain the
    // splitmix roll mapping 85+draw%16; only arithmetic order changes.
    int crit=rng_dbl(r)<damage_crit_probability(atk,mvi);
    if(crit) obs_emit(EV_HIT,atk,HIT_CRIT,0,0);
    double eff=damage_effectiveness(def,mvi);
    obs_emit(EV_HIT,atk,HIT_EFFECT,eff<1?0:eff==1?1:2,0);
    int berry=damage_berry(def,mv,eff);
    if(berry) { def->item_used=1; obs_item(def,1); }
    int roll=85+rng_int(r,16);
    LIVE_DAMAGE=1;
    int damage=damage_value(atk,def,mvi,crit,roll,&DAMAGE_NEUTRAL,berry);
    LIVE_DAMAGE=0; return damage;
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
    if(m->status==STATUS_PAR && rng_dbl(r)<0.125) { obs2_emit(EV_SKIP,m,SKIP_PARALYSIS,0,0); attempt_result(m,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(m); m->charging=0; return 0; }
    return 1;
}

static int CUR_SECOND = 0;   // set by the battle loops: 1 when the acting mon moves second this turn
static int accuracy_check(Mon *atk, Mon *def, const MoveDef *mv, Rng *r) {
    if (mv->acc > 100) return 1;
    double eff = mv->acc * acc_mult(atk->acc);
    if(current_gravity(atk)) {
        // Gravity (priority 0) and these items (priority -2) share
        // ModifyAccuracy: chain once, round once, THEN truncate stages.
        int modifier=6840;
        if(atk->item==ITEM_WIDE) modifier=damage_chain(modifier,4505);
        if(atk->item==ITEM_ZOOM && CUR_SECOND) modifier=damage_chain(modifier,4915);
        if(def && def->item==ITEM_POWDER) modifier=damage_chain(modifier,3686);
        int accuracy=item_mod(mv->acc,modifier);
        eff=atk->acc>=0?accuracy*(3+atk->acc)/3:accuracy*3/(3-atk->acc);
    } else {
        if (atk->item == ITEM_WIDE) eff *= 1.1;                    // Wide Lens
        if (atk->item == ITEM_ZOOM && CUR_SECOND) eff *= 1.2;      // Zoom Lens
        if (def && def->item == ITEM_POWDER) eff *= 0.9;           // Bright Powder
    }
    return rng_dbl(r) * 100.0 < eff;
}

// Stage changes and flinches share one post-hit path for old and new moves.
// Legacy effects retain their skipped rolls and announcement behavior exactly.
static void apply_move_effect(Mon *me, Mon *foe, int mv, int body_hit, Rng *r) {
    const MoveEffect *e=&MOVE_EFFECTS[mv];
    if(!e->kind) return;
    int legacy=mv<V1_MOVE_COUNT;
    if(legacy && (!body_hit || foe->hp<=0)) return;
    Mon *recipient=e->self?me:foe;
    int blocked=!e->self && e->kind==EFFECT_STAGE && e->delta<0 && recipient->ability==AB_CB;
    if(legacy && blocked) {
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
        if(!CUR_SECOND && !recipient->focusing) recipient->flinched=1; // only a remaining action can be lost
        return;
    }
    int *stage=e->stage==5?&recipient->acc:&recipient->st[e->stage];
    int before=*stage; *stage=clamp_stage(*stage+e->delta);
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
    if((mv==MV_GI || mv==MV_HB) && HIT_LANDED) { obs2_emit(EV_RECHARGE_START,me,mv,0,0); me->recharge=1; me->state_features|=MON_POST_V1_STATE; }
    if(mv==MV_ROLL && HIT_LANDED) { if(FIELD.terrain) obs2_emit(EV_TERRAIN_END,me,0,0,0); FIELD.terrain=TERRAIN_NONE; }
    if(mv==MV_BEAM || (mv==MV_DE && dealt>0)) {
        int loss=mv==MV_BEAM?(me->maxhp+1)/2:(dealt*33+50)/100;
        if(loss<1) loss=1;
        int before=me->hp; me->hp-=loss;
        queue_faint(me,mv==MV_BEAM?FAINT_ATTEMPT_COST:FAINT_RECOIL);
        obs_hp(me,before,HP_RECOIL); berry_hp_check(me);
    }
}
static void execute_move_core(Mon *me, Mon *foe, int mv, Rng *r) {
    const MoveDef *M = &MOVES[mv];
    attempt_result(me,ATTEMPT_SUCCESS); LAST_DIRECT_DAMAGE=0;
    if(mv==MV_RND && field_member(me)) {
        int other=foe==FIELD.seat[1];
        if(FIELD.committed[other]==MV_RND && !FIELD.acted[other]) {
            foe->round_chained=1; foe->state_features|=MON_POST_V1_STATE;
        }
    }
    status_modify_move(me,0); // no current move carries defrost
    obs_emit_action(EV_EXEC,me,(MoveAction){ACTION_MOVE,mv},0);
    if (mv != MV_PRO && mv != MV_END && me->protect_streak > 0) me->protect_streak = 0;
    if(mv==MV_FS && !CUR_FUTURE_IMPACT) {
        if(foe->future_turns) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); return; }
        foe->future_turns=3; foe->state_features|=MON_POST_V1_STATE;
        obs2_emit(EV_FUTURE_SET,foe,MV_FS,0,0);
        attempt_result(me,ATTEMPT_NOT_FAIL); return;
    }
    if(mv==MV_ROLL && (!field_member(me) || !FIELD.terrain)) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); return; }
    if(mv==MV_MB) {
        if(me->charging) { obs2_cancel_charge(me); me->charging=0; }
        else { me->charging=1; me->st[2]=clamp_stage(me->st[2]+1); me->state_features|=MON_POST_V1_STATE;
            obs_emit(EV_STAGE,me,2,1,0); obs2_emit(EV_CHARGE_START,me,MV_MB,0,0); attempt_result(me,ATTEMPT_NOT_FAIL); return; }
    }
    switch (mv) {
    case MV_SUN: case MV_RAIN: case MV_SAND: {
        if(!field_member(me)) field_bind(me,foe);
        int weather=mv==MV_SUN?WEATHER_SUN:mv==MV_RAIN?WEATHER_RAIN:WEATHER_SAND;
        if(FIELD.weather==weather) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        if(FIELD.weather) obs2_emit(EV_WEATHER_END,me,0,0,0);
        FIELD.weather=weather; FIELD.weather_turns=5; obs2_emit(EV_WEATHER_START,me,weather,0,0);
        me->weather=foe->weather=weather;
        me->state_features|=MON_POST_V1_STATE; foe->state_features|=MON_POST_V1_STATE;
        return;
    }
    case MV_GRAV:
        if(!field_member(me)) field_bind(me,foe);
        if(FIELD.gravity) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        FIELD.gravity=5; obs2_emit(EV_GRAVITY_START,me,0,0,0);
        if(me->magnet_rise) obs2_emit(EV_RISE_END,me,0,0,0);
        if(foe->magnet_rise) obs2_emit(EV_RISE_END,foe,0,0,0);
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
        *timer=mv==MV_SR?1:5; target->state_features|=MON_POST_V1_STATE;
        obs2_emit(mv==MV_SR?EV_ROCKS:EV_SCREEN_START,target,mv==MV_LS,0,0);
        // Stealth Rock switch-in: floor(maxHP * 2^clamp(typeMod,-6,6) / 8),
        // minimum 1 via damage(). No switching exists; no switch hook is installed.
        return;
    }
    // self-target status
    case MV_AGI: if(me->st[4]==6) attempt_result(me,ATTEMPT_FAIL); me->st[4] = clamp_stage(me->st[4] + 2); obs_emit(EV_STAGE,me,4,2,0); return;
    case MV_CP: if(me->st[1]==6 && me->st[3]==6) attempt_result(me,ATTEMPT_FAIL); me->st[1] = clamp_stage(me->st[1] + 1); me->st[3] = clamp_stage(me->st[3] + 1); obs_emit(EV_STAGE,me,1,1,0); obs_emit(EV_STAGE,me,3,1,0); return;
    case MV_ID: if(me->st[1]==6) attempt_result(me,ATTEMPT_FAIL); me->st[1] = clamp_stage(me->st[1] + 2); obs_emit(EV_STAGE,me,1,2,0); return;
    case MV_REST:
        if (me->healblock > 0) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }                    // fails under heal block
        if (me->hp >= me->maxhp) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }                  // fails at full HP
        clear_major_status(me); // Rest replaces the major status, not confusion
        { int before=me->hp; me->hp = me->maxhp; obs_hp(me,before,HP_HEAL); } me->sleep = 2; obs_emit(EV_SLEEP,me,1,0,0);
        // Chesto / Lum Berry cure the Rest sleep immediately.
        if ((me->item == ITEM_CHESTO || me->item == ITEM_LUM) && !me->item_used) { me->sleep = 0; me->item_used = 1; obs_item(me,1); obs_emit(EV_SLEEP,me,0,0,0); }
        return;
    case MV_PRO: case MV_END: {
        // Endure's onPrepareHit requires a remaining queued action. Protect's
        // pre-v1 compatibility behavior stays frozen; both share the same streak.
        if(mv==MV_END && CUR_SECOND) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        int streak=me->protect_streak;
        if(mv==MV_END && streak>6) streak=6; // pinned stall counterMax 729
        double p = pow(1.0 / 3.0, streak);
        if (rng_dbl(r) < p) { if(mv==MV_END) { me->endured_now=1; me->state_features|=MON_POST_V1_STATE; }
            else me->protected_now = 1;
            me->protect_streak++; obs_emit(EV_PROTECT,me,1,0,0); }
        else { me->protect_streak = 0; attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_PROTECT,me,0,0,0); }
        return; }
    case MV_SUB: {
        if (me->sub_hp > 0) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        int cost = me->maxhp / 4;
        if (me->hp <= cost) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        int before=me->hp; me->hp -= cost; me->sub_hp = cost; obs_hp(me,before,HP_COST); obs_emit(EV_SUB,me,cost>0,0,0); berry_hp_check(me); return; }
    // foe-target status
    case MV_PU:
        if (foe->protected_now && !CUR_FUTURE_IMPACT) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
        for (int i = 0; i < 5; i++) me->st[i] = foe->st[i];
        me->acc = foe->acc; obs_emit(EV_COPY,me,0,0,0); white_herb_check(me); return;
    case MV_SWG: {
        if(foe->protected_now) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
        if(!accuracy_check(me,foe,M,r)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_MISS,0,0)); return; }
        if(foe->sub_hp>0) { obs_emit(EV_HIT,me,HIT_SUB,0,0); return; }
        // spreadMoveHit: boosts first, volatile second, combine the results.
        int before=foe->st[0]; foe->st[0]=clamp_stage(before+2);
        if(foe->st[0]!=before) obs_emit(EV_STAGE,foe,0,foe->st[0]-before,0);
        int confused=inflict_confusion(foe,r);
        if(before==foe->st[0] && !confused) (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0));
        return;
    }
    case MV_SF:
        if (foe->protected_now && !CUR_FUTURE_IMPACT) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
        if (foe->sub_hp > 0) { obs_emit(EV_HIT,me,HIT_SUB,0,0); return; }
        if (!accuracy_check(me, foe, M, r)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_MISS,0,0)); return; }
        if (foe->ability == AB_CB) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_ABILITY,foe,AB_CB,0,0); return; }                 // Clear Body blocks foe-caused drops
        if(foe->st[4]==-6) attempt_result(me,ATTEMPT_FAIL);
        foe->st[4] = clamp_stage(foe->st[4] - 2); obs_emit(EV_STAGE,foe,4,-2,0); white_herb_check(foe); return;
    case MV_TRK:
        if (foe->protected_now && !CUR_FUTURE_IMPACT) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
        if (foe->sub_hp > 0) { obs_emit(EV_HIT,me,HIT_SUB,0,0); return; }
        if (!accuracy_check(me, foe, M, r)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_MISS,0,0)); return; }
        if (trick_stone_blocked(me, foe)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
        if (me->item == ITEM_NONE && foe->item == ITEM_NONE) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
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
    // damaging moves
    if(mv==MV_SN && !me->sleep && !CUR_SLEEP_ACTION) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_FAIL,0,0)); return; }
    if (foe->protected_now && !CUR_FUTURE_IMPACT) { (attempt_result(me,ATTEMPT_NOT_FAIL), obs_emit(EV_HIT,me,HIT_PROTECT,0,0)); return; }
    if(mv>=MV_FP && damage_effectiveness(foe,mv)==0) { attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_IMMUNE,0,0); return; }
    if (!accuracy_check(me, foe, M, r)) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_MISS,0,0)); return; }
    // All Ground immunities use the shared groundedness query.
    if (M->type == T_GROUND && !is_grounded(foe)) { if(!OBS || OBS->version!=2 || (foe->item==ITEM_BALLOON && !foe->item_used)) obs_item(foe,0); (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_IMMUNE,0,0)); return; }
    if(damage_effectiveness(foe,mv)==0) { (attempt_result(me,ATTEMPT_FAIL), obs_emit(EV_HIT,me,HIT_IMMUNE,0,0)); return; }
    // Move-local onTryHit: after protection/accuracy/immunity, before Substitute.
    if(mv==MV_BB || mv==MV_PF) {
        if(foe->reflect) obs2_emit(EV_SCREEN_END,foe,0,0,0);
        if(foe->light_screen) obs2_emit(EV_SCREEN_END,foe,1,0,0);
        foe->reflect=foe->light_screen=0;
    }
    int before_hp = foe->hp;
    int dmg = calc_damage(me, foe, M, r);
    HIT_LANDED=1;
    if (foe->sub_hp > 0 && !MOVE_TRAITS[mv].sound) {
        LAST_DIRECT_DAMAGE=dmg<foe->sub_hp?dmg:foe->sub_hp;
        foe->sub_hp -= dmg;
        if (foe->sub_hp < 0) foe->sub_hp = 0;
        obs_emit(EV_HIT,me,HIT_SUB,foe->sub_hp>0,0);
        if(!foe->sub_hp) obs_emit(EV_SUB,foe,0,0,0);
        apply_move_effect(me,foe,mv,0,r);                  // self effects survive Substitute
        g5_after_damage(me,foe,mv,LAST_DIRECT_DAMAGE);
        post_hit_attacker(me, mv>=MV_FP?LAST_DIRECT_DAMAGE:dmg);                        // Life Orb / Shell Bell apply vs substitutes too
        return;                                            // target effects blocked
    }
    foe->hp -= dmg;
    // Focus Sash (from full HP) / Focus Band (10%): survive a KO hit at 1 HP.
    if (foe->hp <= 0) {
        if(foe->endured_now) foe->hp=1;
        else if (foe->item == ITEM_SASH && !foe->item_used && before_hp >= foe->maxhp) { foe->hp = 1; foe->item_used = 1; obs_item(foe,1); }
        else if (foe->item == ITEM_FBAND && rng_dbl(r) < 0.10) { foe->hp = 1; obs_item(foe,0); }
    }
    LAST_DIRECT_DAMAGE=before_hp-(foe->hp>0?foe->hp:0);
    if(foe->focusing) foe->focus_lost=1;
    if(mv>=MV_FP) queue_faint(foe,FAINT_DAMAGE);
    obs_hp(foe,before_hp,HP_HIT);
    status_damaging_hit(foe,M->type,M->cat,0);
    apply_move_effect(me,foe,mv,1,r);
    if (mv == MV_PN && foe->hp > 0) { foe->healblock = 2; obs_emit(EV_BLOCK,foe,1,0,0); }
    white_herb_check(foe);
    if (foe->hp > 0) {
        if (me->item == ITEM_KINGS && MOVE_EFFECTS[mv].kind!=EFFECT_FLINCH && rng_dbl(r) < 0.10 && (mv<V1_MOVE_COUNT || !CUR_SECOND) && !foe->focusing) foe->flinched = 1;   // King's Rock
        if (foe->item == ITEM_BALLOON && !foe->item_used && dmg > 0) { foe->item_used = 1; obs_item(foe,1); }  // balloon pops
        berry_hp_check(foe);
    }
    g5_after_damage(me,foe,mv,LAST_DIRECT_DAMAGE);
    post_hit_attacker(me, mv>=MV_FP?LAST_DIRECT_DAMAGE:dmg);
    if (M->contact && foe->item == ITEM_HELMET && me->hp > 0) {
        int before=me->hp; me->hp -= me->maxhp / 6; obs_item(foe,0); obs_hp(me,before,HP_RECOIL);                           // Rocky Helmet recoil
        berry_hp_check(me);
    }
}

static void execute_move(Mon *me, Mon *foe, int mv, Rng *r) {
    if(mv>=MV_FP && !field_member(me)) field_bind(me,foe);
    if(field_member(me)) FIELD.acted[me==FIELD.seat[1]]=1;
    if(mv==MV_SD || mv==MV_EXP) {
        int before=me->hp; me->hp=0; me->state_features|=MON_POST_V1_STATE;
        queue_faint(me,FAINT_SELF_DESTRUCT); obs_hp(me,before,HP_COST);
    }
    HIT_LANDED=G5_COST_DONE=0;
    obs2_emit(EV_ATTACK_START,me,mv,CUR_FUTURE_IMPACT,0);
    execute_move_core(me,foe,mv,r);
    g5_after_damage(me,foe,mv,LAST_DIRECT_DAMAGE);
    if(mv>=MV_FP) queue_faint(me,FAINT_RECOIL);
    obs2_emit(EV_ATTACK_END,me,0,0,0);
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
        double sa=eff_speed(a), sb=eff_speed(b);
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
        if(!HIT_LANDED && source->item==ITEM_LIFEORB) { post_hit_attacker(source,1); queue_faint(source,FAINT_RECOIL); }
        source->attempt_this=saved;
    }
}

// Sleep Talk can call Snore (no nosleeptalk/charge flag). Called moves spend
// only Sleep Talk's PP. Scope the sleep context without changing Mon's timer.
static void execute_sleep_move(Mon *me, Mon *foe, int mv, Rng *r) {
    CUR_SLEEP_ACTION=1;
    execute_move(me,foe,mv,r);
    CUR_SLEEP_ACTION=0;
}
// A committed Snore takes precedence over the legacy automatic Sleep Talk.
// Called before the old sleep branch; other old sleep choices are untouched.
static int selected_snore(Mon *me, Mon *foe, int slot, Rng *r) {
    if(me->sleep<=0 || slot<0 || me->moves[slot]!=MV_SN) return 0;
    obs2_emit(EV_SLEEP_TICK,me,0,0,0); me->sleep--;
    if(!me->sleep) obs_emit(EV_SLEEP,me,0,0,0);
    if(!status_before_move(me,r)) return 1;
    obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,MV_SN},0);
    me->pp[slot]--; leppa_check(me,slot);
    execute_sleep_move(me,foe,MV_SN,r);
    note_move_used(me,MV_SN);
    return 1;
}

// Struggle (わるあがき), ported from sim5's formula.
static void do_struggle(Mon *me, Mon *foe, Rng *r) {
    attempt_result(me,ATTEMPT_SUCCESS);
    obs_emit_action(EV_USE,me,(MoveAction){ACTION_STRUGGLE,0},0); obs_emit_action(EV_NOTE,me,(MoveAction){ACTION_STRUGGLE,0},0);
    double d = foe->stats[2] * smult(foe->st[1]);
    long base = (long)floor(22.0 * 50 * me->stats[1] / d);
    base = base / 50 + 2;
    int roll = 85 + rng_int(r, 16);
    int dmg = (int)floor(base * roll / 100.0);
    // Preserve Struggle's legacy kernel; apply the new burn stage only when active.
    if(me->status==STATUS_BRN) { dmg=item_mod(dmg,2048); if(dmg<1) dmg=1; }
    if (foe->sub_hp > 0) { foe->sub_hp -= dmg; if (foe->sub_hp < 0) foe->sub_hp = 0; obs_emit(EV_HIT,me,HIT_SUB,foe->sub_hp>0,0); if(!foe->sub_hp) obs_emit(EV_SUB,foe,0,0,0); }
    else { int before=foe->hp; foe->hp -= dmg; if(foe->endured_now && foe->hp<1) foe->hp=1; if(foe->focusing) foe->focus_lost=1; obs_hp(foe,before,HP_HIT); }
    int before=me->hp; me->hp -= dmg / 4; obs_hp(me,before,HP_RECOIL);
}

// ---------- Damage estimate (for the generic policy) ----------
// Exact discrete expectation of the same integer chain as calc_damage. No RNG,
// observer access or item mutation. Crit probabilities and all 16 rolls match.
static int est_damage(Mon *atk, Mon *def, int mvi) {
    const MoveDef *mv=&MOVES[mvi];
    if(mv->cat==CAT_STATUS || mv->power<=0 || (mvi==MV_SN && !atk->sleep)) return 0;
    if(mv->type==T_GROUND && def->item==ITEM_BALLOON && !def->item_used) return 0;
    double eff=damage_effectiveness(def,mvi);
    if(eff<=0) return 0;
    int berry=damage_berry(def,mv,eff), sum[2]={0,0};
    for(int crit=0;crit<2;crit++) for(int roll=85;roll<=100;roll++)
        sum[crit]+=damage_value(atk,def,mvi,crit,roll,&DAMAGE_NEUTRAL,berry);
    double cp=damage_crit_probability(atk,mvi);
    int estimate=(int)(((1-cp)*sum[0]+cp*sum[1])/16);
    return estimate<1?1:estimate;
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
static int battle(Mon *A, Mon *B, Rng *r, int *turns_out) {
    field_bind(A,B); BATTLE_RNG=r; // reset before attachment, including reused seat addresses
    if (SEQ_MODE < 0) SEQ_MODE = getenv("ENGINE_SEQ") ? 1 : 0;
    if(SEQ_MODE && (!mon_v1_supported(A) || !mon_v1_supported(B))) {
        fprintf(stderr,"new move vocabulary requires simultaneous selection\n"); exit(2);
    }
    Mon *mons[2] = {A, B};
    int observable=is_observable_policy(A->policy) || is_observable_policy(B->policy);
    int version2=is_v2_policy(A->policy) || is_v2_policy(B->policy) || getenv("ENGINE_OBS_V2")!=NULL;
    int enabled=observable || (OBS_FORCE>=0 ? OBS_FORCE : getenv("ENGINE_OBSERVER")!=NULL);
    OBS=NULL;
    if(enabled && SEQ_MODE) { fprintf(stderr,"observer/obsdemo requires simultaneous selection; ENGINE_SEQ is unsupported\n"); exit(2); }
    if((version2 || obs2_vocabulary(A,B)) && (
       (is_observable_policy(A->policy) && !is_v2_policy(A->policy)) ||
       (is_observable_policy(B->policy) && !is_v2_policy(B->policy)))) {
        fprintf(stderr,"MGO1/g12 requires original 17-move vocabulary and cannot mix with MGO2/v2 pilots\n"); exit(2);
    }
    if(enabled) {
        int profile=OBS_PROFILE ? OBS_PROFILE : (getenv("ENGINE_OBS_COMPAT") ? OBS_COMPAT : OBS_REGULAR);
        if(!(version2?obs2_valid_build(A,profile):obs_valid_build(A,profile)) ||
           !(version2?obs2_valid_build(B,profile):obs_valid_build(B,profile))) {
            fprintf(stderr,"observer build rejected: MGO1/g12 requires original 17-move vocabulary and v1 state; MGO2 requires neutral initialization; regular requires four distinct moves, fixed PP, legal Metagross stats and normal CB/LM; legacy builds require ENGINE_OBS_COMPAT=1\n"); exit(2);
        }
        if(version2) obs2_attach(A,B,profile); else obs_attach(A,B,profile);
    }
    for (int turn = 1; turn <= MAX_TURN; turn++) {
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
                        me->pp[sti]--; leppa_check(me, sti);
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
                else { if(!(me->charging && me->moves[ci]==MV_MB)) me->pp[ci]--; leppa_check(me, ci); execute_move(me, foe, me->moves[ci], r); note_move_used(me, me->moves[ci]); }
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
        Observation frozen[2]; Observation2 frozen2[2]; ActionMask masks[2];
        if(OBS) {
            OBS->turn=turn;
            if(turn>1) obs_emit(EV_BEGIN,A,0,0,0);
            // Both snapshots exist before either callback or submitted action.
            for(int i=0;i<2;i++) {
                if(version2) obs2_build(mons[i],&OBS->v2[i],&frozen2[i],&masks[i]);
                else obs_build(mons[i],&OBS->view[i],&frozen[i],&masks[i]);
            }
        }
        int choice[2];
        for (int i = 0; i < 2; i++) {
            int policy=mons[i]->policy;
            if(is_v2_policy(policy)) choice[i]=obs_checked_action((policy==POL_LEARNED2?learned2_policy:policy==POL_OBSH3?obsh3:observable_policy2)(&frozen2[i],&masks[i]),&masks[i]);
            else if(is_observable_policy(policy)) choice[i]=obs_checked_action((policy==POL_LEARNED?learned_policy:policy==POL_OBSH2?obsh2:observable_policy)(&frozen[i],&masks[i]),&masks[i]);
            else choice[i]=choose_move(mons[i],mons[1-i]);
        }
        // Choice Scarf lock: a locked mon must repeat its locked move while it has PP.
        for (int i = 0; i < 2; i++) {
            Mon *m = mons[i];
            if (!is_observable_policy(m->policy) && m->item == ITEM_SCARF && m->choice_lock >= 0) {
                int li = find_move(m, m->choice_lock);
                if (li >= 0 && m->pp[li] > 0) choice[i] = li;
            }
        }
        // Both policies and item locks have committed before focus is announced.
        for(int i=0;i<2;i++) {
            Mon *m=mons[i];
            if(m->charging) choice[i]=find_move(m,MV_MB);
            FIELD.committed[i]=choice[i]>=0?m->moves[choice[i]]:-1;
        }
        for(int i=0;i<2;i++) if(FIELD.committed[i]==MV_FP && !mons[i]->recharge) {
            mons[i]->focusing=1; mons[i]->state_features|=MON_POST_V1_STATE;
            obs_emit(EV_PREPARE,mons[i],MV_FP,0,0);
        }
        // order
        int first = 0;
        {
            int pa = g5_priority(mons[0],choice[0]);
            int pb = g5_priority(mons[1],choice[1]);
            if (pb > pa) first = 1;
            else if (pb == pa) {
                // Quick Claw: 20% to move first within the same priority bracket.
                int ca = (A->item == ITEM_CLAW && rng_dbl(r) < 0.20) ? 1 : 0;
                int cb = (B->item == ITEM_CLAW && rng_dbl(r) < 0.20) ? 1 : 0;
                if(ca) obs_item(A,0);
                if(cb) obs_item(B,0);
                if (ca != cb) first = cb;
                else {
                    double sa = eff_speed(A);
                    double sb = eff_speed(B);
                    if (sb > sa) first = 1;
                    else if (sb == sa && rng_dbl(r) < 0.5) first = 1;
                }
            }
        }
        for (int k = 0; k < 2; k++) {
            Mon *me = mons[k == 0 ? first : 1 - first];
            Mon *foe = mons[k == 0 ? 1 - first : first];
            if (me->hp <= 0 || foe->hp <= 0) break;
            CUR_SECOND = (k == 1);
            obs_emit(EV_OPPORTUNITY,me,OBS && OBS->version==2?ACT_NONE:me->flinched?ACT_FLINCH:me->sleep>0?ACT_SLEEP:ACT_USED,0,0);
            if(me->recharge) { obs2_emit(EV_SKIP,me,SKIP_RECHARGE,0,0); me->recharge=0; obs2_cancel_charge(me); me->charging=0; attempt_result(me,ATTEMPT_SKIP_NOT_FAIL); obs_emit(EV_RECHARGE,me,0,0,0); continue; }
            if(!freeze_before_move(me,0,0,r)) { obs2_emit(EV_SKIP,me,SKIP_FREEZE,0,0); attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue; }
            if (me->flinched) { obs2_emit(EV_SKIP,me,SKIP_FLINCH,0,0); me->flinched = 0; attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue; }   // flinch: lose this action
            if (me->healblock > 0) { obs2_emit(EV_BLOCK_TICK,me,0,0,0); me->healblock--; if(!me->healblock) obs_emit(EV_BLOCK,me,0,0,0); }
            int committed = (k == 0 ? choice[first] : choice[1 - first]);
            if(committed>=0 && me->moves[committed]==MV_MR && current_gravity(me)) {
                // R4: named failure before PP deduction, as in Gravity's
                // BeforeMove handler. Origin 4 announces a direct nonspending
                // attempt; no execution/note/lock or repetition update occurs.
                obs2_emit(EV_USE,me,MV_MR,4,0);
                obs2_emit(EV_HIT,me,HIT_FAIL,0,0);
                attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue;
            }
            if(selected_snore(me,foe,committed,r)) {
                if (me->hp <= 0 || foe->hp <= 0) break;
                continue;
            }
            if (me->sleep > 0) {
                attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0;
                obs2_emit(EV_SLEEP_TICK,me,0,0,0); me->sleep--;
                if(!me->sleep) obs_emit(EV_SLEEP,me,0,0,0);
                int sti = find_move(me, MV_ST);
                if (sti >= 0 && me->pp[sti] > 0) {
                    if(!status_before_move(me,r)) continue;
                    attempt_result(me,ATTEMPT_FAIL);
                    obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,MV_ST},2);
                    me->pp[sti]--; leppa_check(me, sti);
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
            if(!status_before_move(me,r)) continue;
            int ci = (k == 0 ? choice[first] : choice[1 - first]);
            if(ci>=0 && me->moves[ci]==MV_FP && me->focus_lost) { obs2_emit(EV_SKIP,me,SKIP_FOCUS,0,0); attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); continue; }
            if (ci < 0) { do_struggle(me, foe, r); me->last_move = -1; me->metro_streak = 0; }
            else {
                obs_emit_action(EV_USE,me,(MoveAction){ACTION_MOVE,me->moves[ci]},0);
                if(!(me->charging && me->moves[ci]==MV_MB)) me->pp[ci]--;
                leppa_check(me, ci);
                execute_move(me, foe, me->moves[ci], r);
                note_move_used(me, me->moves[ci]);
            }
            if (me->hp <= 0 || foe->hp <= 0) break;
        }
        end_of_turn(A, B);
        A->protected_now = 0; B->protected_now = 0;
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

// ---------- Trace (one battle, verbose) ----------
static int battle_trace(Mon *A, Mon *B, Rng *r) {
    OBS=NULL;
    if(is_observable_policy(A->policy) || is_observable_policy(B->policy)) {
        fprintf(stderr,"observable policies use battle(), not the privileged trace path\n"); exit(2);
    }
    Mon *mons[2] = {A, B};
    field_bind(A,B); BATTLE_RNG=r;
    for (int turn = 1; turn <= MAX_TURN; turn++) {
        g5_begin_turn(A,B);
        int choice[2];
        for (int i = 0; i < 2; i++) choice[i] = choose_move(mons[i], mons[1 - i]);
        for (int i = 0; i < 2; i++) {
            Mon *m = mons[i];
            if (m->item == ITEM_SCARF && m->choice_lock >= 0) {
                int li = find_move(m, m->choice_lock);
                if (li >= 0 && m->pp[li] > 0) choice[i] = li;
            }
        }
        // Both policies and item locks have committed before focus is announced.
        for(int i=0;i<2;i++) {
            Mon *m=mons[i];
            if(m->charging) choice[i]=find_move(m,MV_MB);
            FIELD.committed[i]=choice[i]>=0?m->moves[choice[i]]:-1;
        }
        for(int i=0;i<2;i++) if(FIELD.committed[i]==MV_FP && !mons[i]->recharge) {
            mons[i]->focusing=1; mons[i]->state_features|=MON_POST_V1_STATE;
            obs_emit(EV_PREPARE,mons[i],MV_FP,0,0);
        }
        int first = 0;
        {
            int pa = g5_priority(mons[0],choice[0]);
            int pb = g5_priority(mons[1],choice[1]);
            if (pb > pa) first = 1;
            else if (pb == pa) {
                int ca = (A->item == ITEM_CLAW && rng_dbl(r) < 0.20) ? 1 : 0;
                int cb = (B->item == ITEM_CLAW && rng_dbl(r) < 0.20) ? 1 : 0;
                if (ca != cb) first = cb;
                else {
                    double sa = eff_speed(A);
                    double sb = eff_speed(B);
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
            CUR_SECOND = (k == 1);
            if(me->recharge) { obs2_emit(EV_SKIP,me,SKIP_RECHARGE,0,0); me->recharge=0; obs2_cancel_charge(me); me->charging=0; attempt_result(me,ATTEMPT_SKIP_NOT_FAIL); obs_emit(EV_RECHARGE,me,0,0,0); continue; }
            if(!freeze_before_move(me,0,0,r)) { obs2_emit(EV_SKIP,me,SKIP_FREEZE,0,0); attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue; }
            if (me->flinched) { obs2_emit(EV_SKIP,me,SKIP_FLINCH,0,0); me->flinched = 0; attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; printf("T%d %s: FLINCH  A HP%d B HP%d\n", turn, who, A->hp, B->hp); continue; }
            if (me->healblock > 0) me->healblock--;
            int committed = (k == 0 ? choice[first] : choice[1 - first]);
            if(committed>=0 && me->moves[committed]==MV_MR && current_gravity(me)) { obs2_emit(EV_SKIP,me,SKIP_UNANNOUNCED,0,0); attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0; continue; }
            if(selected_snore(me,foe,committed,r)) {
                printf("T%d %s: SN (asleep)  A HP%d B HP%d\n",turn,who,A->hp,B->hp);
                if (me->hp <= 0 || foe->hp <= 0) break;
                continue;
            }
            if (me->sleep > 0) {
                attempt_result(me,ATTEMPT_SKIP_FAIL); obs2_cancel_charge(me); me->charging=0;
                me->sleep--;
                int sti = find_move(me, MV_ST);
                if (sti >= 0 && me->pp[sti] > 0) {
                    if(!status_before_move(me,r)) continue;
                    me->pp[sti]--; leppa_check(me, sti);
                    int others[4], no = 0;
                    for (int i = 0; i < me->nmoves; i++) if (i != sti && g5_sleep_callable(me->moves[i])) others[no++] = i;
                    if (no > 0) {
                        int call = others[rng_int(r, no)];
                        int cmv = me->moves[call];
                        if (cmv != MV_REST && cmv != MV_ST) execute_sleep_move(me, foe, cmv, r);
                        note_move_used(me, cmv);           // match the non-trace battle path
                        if (cmv == MV_TRK && trick_stone_blocked(me, foe))
                            printf("T%d %s: ST->TRK (Mega Stone blocks swap)  A HP%d B HP%d\n", turn, who, A->hp, B->hp);
                        else printf("T%d %s: ST->%s  A HP%d B HP%d\n", turn, who, MOVES[cmv].code, A->hp, B->hp);
                    } else printf("T%d %s: SLEEP  A HP%d B HP%d\n", turn, who, A->hp, B->hp);
                } else printf("T%d %s: SLEEP  A HP%d B HP%d\n", turn, who, A->hp, B->hp);
                if (me->hp <= 0 || foe->hp <= 0) break;
                continue;
            }
            if(!status_before_move(me,r)) continue;
            int ci = (k == 0 ? choice[first] : choice[1 - first]);
            if(ci>=0 && me->moves[ci]==MV_FP && me->focus_lost) { obs2_emit(EV_SKIP,me,SKIP_FOCUS,0,0); attempt_result(me,ATTEMPT_FAIL); obs_emit(EV_HIT,me,HIT_FAIL,0,0); continue; }
            if (ci < 0) { do_struggle(me, foe, r); me->last_move = -1; me->metro_streak = 0; printf("T%d %s: STRUGGLE  A HP%d B HP%d\n", turn, who, A->hp, B->hp); }
            else { if(!(me->charging && me->moves[ci]==MV_MB)) me->pp[ci]--; leppa_check(me, ci); execute_move(me, foe, me->moves[ci], r); note_move_used(me, me->moves[ci]);
                   if (me->moves[ci] == MV_TRK && trick_stone_blocked(me, foe))
                       printf("T%d %s: TRK (Mega Stone blocks swap)  A HP%d B HP%d\n", turn, who, A->hp, B->hp);
                   else printf("T%d %s: %s  A HP%d B HP%d\n", turn, who, MOVES[me->moves[ci]].code, A->hp, B->hp); }
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
    return POL_MIRROR;
}
static int parse_item(const char *s) {
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
    return ITEM_NONE;
}
static void init_mon(Mon *m, const char *spec) {
    if(field_member(m)) FIELD=(BattleField){0};
    char buf[512]; strncpy(buf, spec, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    memset(m, 0, sizeof(*m));
    char *stats_s = strtok(buf, "|");
    char *moves_s = strtok(NULL, "|");
    char *pol_s = strtok(NULL, "|");
    char *item_s = strtok(NULL, "|");
    char *mega_s = strtok(NULL, "|");
    char *ab_s = strtok(NULL, "|");
    int v[6]; char *p = stats_s;
    for (int i = 0; i < 6; i++) { v[i] = (int)strtol(p, &p, 10); if (*p == ',') p++; }
    for (int i = 0; i < 6; i++) m->stats[i] = v[i];
    m->maxhp = m->hp = v[0];
    m->type1 = T_STEEL; m->type2 = T_PSY;   // Metagross (v1 scope)
    m->nmoves = 0;
    if (moves_s) {
        char *tok = strtok(moves_s, ",");
        while (tok) {
            int mv = move_from_code(tok);
            if (mv < 0 || m->nmoves >= 4) m->state_features |= MON_UNKNOWN_VOCAB;
            if (mv >= 0 && m->nmoves < 4) { m->moves[m->nmoves] = mv; m->pp[m->nmoves] = MOVES[mv].pp; m->nmoves++; }
            tok = strtok(NULL, ",");
        }
    }
    m->policy = pol_s ? parse_policy(pol_s) : POL_MIRROR;
    m->item = item_s ? parse_item(item_s) : ITEM_NONE;
    m->mega = mega_s ? atoi(mega_s) : 0;
    m->choice_lock = -1;
    m->last_move = -1;
    m->crit_bonus = (m->item == ITEM_LENS) ? 1 : 0;
    m->ability = AB_NONE;
    if (ab_s && !m->mega) {   // Mega's ability is fixed to Tough Claws (via the mega flag)
        if (!strcmp(ab_s, "cb")) m->ability = AB_CB;
        else if (!strcmp(ab_s, "lm")) m->ability = AB_LM;
    }
}

static uint64_t hash64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}
static void run_matchup(const char *specA, const char *specB, long n, uint64_t seed,
                        const char *label) {
    long wA = 0, wB = 0, dr = 0; long long turns = 0;
    for (long g = 0; g < n; g++) {
        Mon A, B; init_mon(&A, specA); init_mon(&B, specB);
        // Per-game state = hash(seed, g): starting states must be decorrelated,
        // otherwise streams are shifted windows of one arithmetic sequence.
        Rng r = { hash64(seed ^ ((uint64_t)(g + 1) * 0xD1B54A32D192ED03ULL)) };
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

int main(int argc, char **argv) {
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
        while (np < 512 && fgets(line, sizeof line, pf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0]) strcpy(pool[np++], line);
        }
        fclose(pf);
        FILE *cf = fopen(argv[2], "r");
        if (!cf) { fprintf(stderr, "cannot open cand file\n"); return 1; }
        long games = strtol(argv[4], NULL, 10);
        uint64_t seed = strtoull(argv[5], NULL, 10);
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
        }
        fclose(cf);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "screenx") && argc == 6) {
        // screenx: like screen, but prints per-pool-member W L D on one line per candidate.
        FILE *pf = fopen(argv[3], "r");
        if (!pf) { fprintf(stderr, "cannot open pool file\n"); return 1; }
        static char pool[512][512]; int np = 0;
        char line[512];
        while (np < 512 && fgets(line, sizeof line, pf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0]) strcpy(pool[np++], line);
        }
        fclose(pf);
        FILE *cf = fopen(argv[2], "r");
        if (!cf) { fprintf(stderr, "cannot open cand file\n"); return 1; }
        long games = strtol(argv[4], NULL, 10);
        uint64_t seed = strtoull(argv[5], NULL, 10);
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
        }
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
    fprintf(stderr, "usage: engine battle <specA> <specB> <n> <seed> | engine matrix [n] [seed] | engine screen <cand_file> <pool_file> <games> <seed>\n");
    return 1;
}
