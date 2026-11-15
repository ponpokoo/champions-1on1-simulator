// Acceptance for the actual generation callback, not just deployment argmax.
#define STAGE3_LIBRARY
#include "loop.c"
#define REQUIRE(x) do { if(!(x)) { fprintf(stderr,"sampling FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int extra_draws;
static int deterministic_with_draws(const Observation *o,const ActionMask *mask) {
    for(int i=0;i<extra_draws;i++) (void)rng_next(&policy_rng);
    return learned(o,mask);
}
int main(void) {
    const char *spec="187,155,150,115,110,122|EQ,SB,SF,ST|learned|none|0|cb";
    Mon a,b; init_mon(&a,spec); init_mon(&b,spec);
    OBS_FORCE=1; OBS_PROFILE=OBS_REGULAR; SEQ_MODE=0; obs_sink=NULL;
    obs_attach(&a,&b,OBS_REGULAR);
    Observation o; ActionMask mask; obs_build(&a,&OBS->view[0],&o,&mask);
    Observation original=o; ActionMask original_mask=mask;
    actor=learned_default;
    for(int i=0;i<160;i++) actor.wp[i]=(float)((i*13)%29-14)*.01f;
    for(int i=0;i<32;i++) actor.wv[i]=(float)((i*7)%19-9)*.01f;
    policy_rng.s=73103; ndecision=0; int expected=collect_policy(&o,&mask);
    unsigned char record[RECORD_BYTES]; memcpy(record,decisions[0],sizeof record);
    for(int hidden=0;hidden<1000;hidden++) {
        b.stats[hidden%6]=hidden+1; b.hp=hidden; b.maxhp=hidden+1; b.item=hidden%35;
        b.item_used=hidden%2; b.ability=hidden%3; b.moves[hidden%4]=hidden%17; b.pp[hidden%4]=hidden%20;
        b.sleep=hidden%3; b.healblock=hidden%3; b.sub_hp=hidden+1; b.choice_lock=hidden%17;
        b.last_move=hidden%17; b.metro_streak=hidden; b.policy=hidden%8; CUR_SECOND=hidden%2;
        policy_rng.s=73103; ndecision=0;
        REQUIRE(collect_policy(&o,&mask)==expected); REQUIRE(!memcmp(record,decisions[0],sizeof record));
        REQUIRE(!memcmp(&o,&original,sizeof o) && !memcmp(&mask,&original_mask,sizeof mask));
    }
    for(int bits=1;bits<32;bits++) {
        for(int i=0;i<5;i++) o.v[176+i]=mask.legal[i]=(bits>>i)&1;
        for(int sample=0;sample<32;sample++) {
            ndecision=0; int chosen=collect_policy(&o,&mask); REQUIRE(chosen>=0 && chosen<5 && mask.legal[chosen]);
        }
    }
    learned_policy=deterministic_with_draws;
    for(int seed=1;seed<=300;seed++) {
        Mon base_a,base_b,x,y; init_mon(&base_a,spec); init_mon(&base_b,spec); base_b.policy=POL_GENERIC;
        Rng r1={hash64(seed)},r2=r1; int t1,t2;
        a=base_a; b=base_b; extra_draws=0; int result1=battle(&a,&b,&r1,&t1);
        x=base_a; y=base_b; extra_draws=97; int result2=battle(&x,&y,&r2,&t2);
        REQUIRE(result1==result2 && t1==t2 && r1.s==r2.s);
        REQUIRE(!memcmp(&a,&x,sizeof a) && !memcmp(&b,&y,sizeof b));
    }
    REQUIRE(obs_fallbacks==0);
    puts("PASS sampler: 1000 hidden states, exact records/distribution, immutable inputs, 31 masks, 300 independent-policy-RNG battle pairs; fallbacks=0");
    return 0;
}
