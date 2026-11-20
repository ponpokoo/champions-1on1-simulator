// Build as a single translation unit against the actual battle and policy code.
#define main engine_cli_main
#include "engine.c"
#undef main
#include <assert.h>

static const char *target=
    "sylveon:32,0,2,0,32,0|calmmind,drainingkiss,mysticalfire,storedpower|obsh4|leftovers|0|pixilate|4,1|m";
static const char *opponent=
    "sylveon:32,0,2,0,32,0|drainingkiss,mysticalfire,rest,lightscreen|obsh4|leftovers|0|cutecharm|4,1|m";
static const char *hidden_variant=
    "sylveon:2,0,0,32,0,32|hypervoice,shadowball,psyshock,mysticalfire|obsh4|lifeorb|0|cutecharm|3,1|m";

static int play(const char *foe,long game,int *turns,const Obsh4Audit *audit) {
    Mon A,B;
    init_mon(&A,target);
    init_mon(&B,foe);
    Rng r={matchup_seed(12345,game)};
    return audit?battle_with_audit(&A,&B,&r,turns,audit):battle(&A,&B,&r,turns);
}

static void test_hidden_state(void) {
    Obsh4Turn original[OBSH4_DIAG_TURNS],variant[OBSH4_DIAG_TURNS];
    Obsh4Audit audit={.candidate=OBSH4_STAT_DROP|OBSH4_SUBSTITUTE|OBSH4_SETUP_RACE|OBSH4_SETUP_TEMPO|OBSH4_OPENING,
                     .turn=1,.record=original};
    int turns;
    play(opponent,0,&turns,&audit);
    audit.record=variant;
    play(hidden_variant,0,&turns,&audit);
    assert(!memcmp(&original[0].observation[0],&variant[0].observation[0],sizeof(Observation3)));
    assert(!memcmp(&original[0].masks[0],&variant[0].masks[0],sizeof(ActionMask)));
    assert(original[0].choice[0]==variant[0].choice[0]);
}

static void test_drop_score(const Obsh4Turn *turn) {
    H4Position p=h4_position(&turn->observation[0]);
    int move=turn->observation[0].v[H4_SELF_SLOTS+4*2+1];
    assert(!strcmp(S8_MOVES[move].id,"mysticalfire"));
    H4Damage hit=h4_damage(&p,0,move);
    H4Threat threat=h4_threat(&p);
    assert(h4_attack_drop(&p,move,hit,threat)>0);
    assert(h4_attack_drop(&p,move,(H4Damage){.hit=1,.ko=1},threat)==0);
    const int blocked[]={S8Q_CLEARBODY,S8G_SHIELDDUST,S8Q_CONTRARY,S8Q_MIRRORARMOR,S8Q_COMPETITIVE};
    for(size_t i=0;i<sizeof blocked/sizeof blocked[0];i++) {
        H4Position after=p;
        after.mon[1].ability=blocked[i];
        assert(h4_attack_drop(&after,move,hit,threat)==0);
    }
    H4Position after=p;
    after.mon[1].sub=1;
    assert(h4_attack_drop(&after,move,hit,threat)==0);
    after=p;
    after.mon[1].rank[2]=-6;
    assert(h4_attack_drop(&after,move,hit,threat)==0);
    after=p;
    after.mon[1].item_id=s8_item_id("whiteherb");
    after.mon[1].item=ITEM_HERB;
    assert(h4_attack_drop(&after,move,hit,threat)==0);
    after=p;
    after.mon[0].ability=S8D_SHEERFORCE;
    assert(h4_attack_drop(&after,move,hit,threat)==0);
}

static void test_replay(void) {
    for(long game=0;game<32;game++) {
        Obsh4Turn normal[OBSH4_DIAG_TURNS],branch[OBSH4_DIAG_TURNS];
        Obsh4Audit audit={.candidate=-1,.record=normal};
        int plain_turns,recorded_turns;
        int plain=play(opponent,game,&plain_turns,NULL);
        int recorded=play(opponent,game,&recorded_turns,&audit);
        assert(plain==recorded && plain_turns==recorded_turns);
        assert(recorded_turns>=2);
        audit.turn=2;
        audit.slot=normal[1].choice[0];
        audit.expected=&normal[1];
        audit.record=branch;
        int fork_turns;
        int fork=play(opponent,game,&fork_turns,&audit);
        assert(fork==recorded && fork_turns==recorded_turns && branch[1].attempted==normal[1].attempted);
        if(game==0) test_drop_score(&normal[1]);
    }
}

static void test_substitute(void) {
    const char *substitute_user=
        "vaporeon:32,0,2,32,0,0|icebeam,shadowball,substitute,rest|obsh4|leftovers|0|waterabsorb|3,1|m";
    const char *protect_user=
        "vaporeon:32,0,32,0,2,0|shadowball,bodyslam,protect,yawn|obsh4|leftovers|0|waterabsorb|2,1|m";
    Mon a,b;
    init_mon(&a,substitute_user);
    init_mon(&b,protect_user);
    Obsh4Turn record[OBSH4_DIAG_TURNS];
    Obsh4Audit audit={.record=record};
    Rng rng={matchup_seed(67890,0)};
    int turns;
    battle_with_audit(&a,&b,&rng,&turns,&audit);
    assert(turns>=2);
    const Observation3 *o=&record[1].observation[0];
    const ActionMask *mask=&record[1].masks[0];
    assert(obsh4(o,mask,0)==1);
    assert(obsh4(o,mask,OBSH4_SUBSTITUTE)==2);

    H4Position p=h4_position(o);
    H4Threat threat=h4_threat(&p);
    assert(h4_substitute(&p,threat,20)>0);
    assert(h4_substitute(&p,(H4Threat){.move=-1},20)==0);
    H4Position blocked=p;
    blocked.mon[0].hp=40;
    assert(h4_substitute(&blocked,threat,20)==0);
    blocked=p;
    blocked.mon[0].sub=1;
    assert(h4_substitute(&blocked,threat,20)==0);
    blocked=p;
    blocked.mon[1].ability=S8D_INFILTRATOR;
    assert(h4_substitute(&blocked,threat,20)==0);

    Observation3 visible=*o;
    int32_t *known=visible.v+O3_RECORDS+OBS3_RECORDS*H4_RECORD_WORDS;
    assert(known[0] && known[1]==MV_SB);
    int sound=-1;
    for(int i=0;i<S8_MOVES_COUNT;i++) if(!strcmp(S8_MOVES[i].id,"hypervoice")) sound=i;
    assert(sound>=0);
    known[1]=sound;
    blocked=h4_position(&visible);
    threat.move=sound;
    assert(h4_substitute(&blocked,threat,20)==0);

    init_mon(&a,substitute_user);
    init_mon(&b,protect_user);
    audit.candidate=-1;
    rng.s=matchup_seed(67890,0);
    battle_with_audit(&a,&b,&rng,&turns,&audit);
    assert(turns>=2 && record[1].choice[0]==2);
}

static void test_setup_race(void) {
    H4Position p={0};
    p.mon[0].hp=92;
    p.mon[1].hp=100;
    p.mon[0].stat[5][1]=90;
    for(int i=0;i<3;i++) p.mon[1].stat[5][i]=100;
    H4Position after=p;
    after.mon[0].rank[4]=1;
    const double gain[]={24,43,30,18};
    assert(h4_race(&p,&p,-1,MV_SB,MV_SB,MV_SB,gain,0)==-1);
    assert(h4_race(&p,&after,S8B_DRAGONDANCE,MV_SB,MV_SB,MV_SB,gain,1)==2);

    p.mon[0].hp=65;
    p.mon[0].stat[5][1]=120;
    after=p;
    const double cost[]={55,54,45,45};
    assert(h4_race(&p,&p,-1,MV_SB,MV_SB,MV_SB,cost,1)==4);
    assert(h4_race(&p,&after,S8B_CALMMIND,MV_SB,MV_SB,MV_SB,cost,0)==-1);
}

static void test_opening_calmmind(void) {
    const char *build="sylveon:9,3,28,2,18,6|bodyslam,calmmind,protect,psyshock|obsh4|sitrusberry|0|cutecharm|2,1|m";
    const char *foe="sylveon:18,0,16,17,0,15|copycat,detect,focusenergy,psychic|obsh4|babiriberry|0|cutecharm|2,3|m";
    Mon a,b;
    Obsh4Turn record[OBSH4_DIAG_TURNS];
    Obsh4Audit audit={.candidate=-1,.turn=1,.record=record};
    int turns;
    init_mon(&a,build);
    init_mon(&b,foe);
    Rng rng={matchup_seed(12345,0)};
    battle_with_audit(&a,&b,&rng,&turns,&audit);
    const Observation3 *o=&record[0].observation[0];
    const ActionMask *mask=&record[0].masks[0];
    int old=obsh4(o,mask,OBSH4_SETUP_BASELINE);
    assert(old==3 && record[0].choice[0]==1);
    assert(obsh4(o,mask,OBSH4_SETUP_BASELINE|OBSH4_OPENING)==1);
    H4Position p=h4_position(o);
    assert(h4_opening_choice(&p,mask,old)==1);
    p.mon[0].item_id=s8_item_id("choicescarf");
    p.mon[0].item=ITEM_SCARF;
    assert(h4_opening_choice(&p,mask,old)==old);
    Observation3 later=*o;
    later.v[O3_CONTEXT]=2;
    p=h4_position(&later);
    assert(h4_opening_choice(&p,mask,old)==old);
}

int main(void) {
    test_hidden_state();
    test_replay();
    test_substitute();
    test_setup_race();
    test_opening_calmmind();
    puts("obsh4 diagnostics: reproducibility, information boundary and bonus guards passed");
    return 0;
}
