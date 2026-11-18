// Offline generation/evaluation harness. Metadata and sampling streams stay here.
#define main engine_cli_main
#include "../engine/engine.c"
#undef main
#include <inttypes.h>
#define RECORD_BYTES 390
#define ACTIVE_WORDS 181
static Mon population[176];
static LearnedWeights actor, snapshots[16];
static int nsnap;
static Rng policy_rng;
static unsigned char decisions[MAX_TURN][RECORD_BYTES];
static int ndecision;
static const LearnedWeights *opponent_weights;
static void put16(unsigned char *p,unsigned v) { p[0]=v; p[1]=v>>8; }
static void put32(unsigned char *p,uint32_t v) { for(int j=0;j<4;j++) p[j]=v>>(j*8); }
static void put64(unsigned char *p,uint64_t v) { for(int j=0;j<8;j++) p[j]=v>>(j*8); }
static unsigned get32(const unsigned char *p) { return p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24; }
static void putfloat(unsigned char *p,float f) { uint32_t u; memcpy(&u,&f,4); put32(p,u); }
static void load_weights(const char *path,LearnedWeights *w) {
    if(!strcmp(path,"@compiled")) { *w=learned_default; return; }
    FILE *f=fopen(path,"rb"); unsigned char h[16],b[4];
    if(!f || fread(h,1,16,f)!=16 || memcmp(h,"MWP1",4) || get32(h+4)!=415 || get32(h+8)!=64 || get32(h+12)!=32) { fprintf(stderr,"bad model %s\n",path); exit(2); }
    // All fields are float arrays; no native-struct serialization on disk.
    float out[sizeof(*w)/sizeof(float)];
    for(size_t i=0;i<sizeof(*w)/sizeof(float);i++) {
        if(fread(b,1,4,f)!=4) exit(2);
        uint32_t u=get32(b); memcpy(out+i,&u,4);
        if(!isfinite(out[i])) exit(2);
    }
    if(fgetc(f)!=EOF) exit(2);
    fclose(f);
    memcpy(w,out,sizeof(*w));
}
static void read_population(const char *path) {
    FILE *f=fopen(path,"r"); char line[1024]; int n=0;
    if(!f) { perror(path); exit(2); }
    while(fgets(line,sizeof line,f)) {
        line[strcspn(line,"\r\n")]=0; if(!*line) continue;
        if(n>=176) exit(2);
        init_mon(&population[n],line);
        if(!obs_valid_build(&population[n],OBS_REGULAR)) { fprintf(stderr,"bad build %d\n",n); exit(2); } n++;
    }
    fclose(f); if(n!=176) { fprintf(stderr,"expected 168 finals + 8 regularized benchmarks, got %d\n",n); exit(2); }
}
static int snapshot_policy(const Observation *o,const ActionMask *m) {
    float logits[5],v; learned_forward(o,m,opponent_weights,logits,&v);
    int best=-1; for(int i=0;i<5;i++) if(m->legal[i] && (best<0 || logits[i]>logits[best])) best=i; return best;
}
static int collect_policy(const Observation *o,const ActionMask *m) {
    if(ndecision>=MAX_TURN) abort();
    float logits[5],value; learned_forward(o,m,&actor,logits,&value);
    float max=-1e9f; double p[5],sum=0;
    for(int i=0;i<5;i++) if(m->legal[i] && logits[i]>max) max=logits[i];
    for(int i=0;i<5;i++) { p[i]=m->legal[i]?exp((double)logits[i]-max):0; sum+=p[i]; }
    double u=rng_dbl(&policy_rng)*sum; int action=-1;
    for(int i=0;i<5;i++) if(m->legal[i]) { action=i; u-=p[i]; if(u<0) break; }
    unsigned char *b=decisions[ndecision++]; memset(b,0,RECORD_BYTES);
    for(int i=0;i<ACTIVE_WORDS;i++) {
        int v=i==117?0:o->v[i]; if(v<INT16_MIN || v>INT16_MAX) abort(); put16(b+2*i,(unsigned)v);
    }
    for(int i=181;i<256;i++) if(o->v[i]) abort();
    put32(b+362,(uint32_t)o->v[117]);
    for(int i=0;i<5;i++) b[366]|=m->legal[i]<<i;
    b[367]=action; b[388]=obsh2(o,m);
    putfloat(b+380,(float)log(p[action]/sum)); putfloat(b+384,value);
    return action;
}
static void generate(int argc,char **argv) {
    // generate population out games seed actor hash [earlier snapshot ...]
    if(argc<9 || strlen(argv[8])!=64) {
        fprintf(stderr,"usage: loop stage3 generate <population> <out.mgt> <games> <seed> <model|@compiled> <64-hex-sha256> [snapshots...]\n");
        exit(2);
    }
    long games=strtol(argv[5],NULL,10); uint64_t seed=strtoull(argv[6],NULL,10);
    if(games<1 || games>1000000) { fprintf(stderr,"games must be in 1..1000000\n"); exit(2); }
    load_weights(argv[7],&actor);
    nsnap=argc-9; if(nsnap>16) { fprintf(stderr,"too many snapshots (max 16)\n"); exit(2); }
    for(int s=0;s<nsnap;s++) load_weights(argv[9+s],&snapshots[s]);
    FILE *f=fopen(argv[4],"wb"); if(!f) { perror(argv[4]); exit(2); }
    unsigned char header[64]={0}; memcpy(header,"MGT3",4);
    put32(header+4,1); put32(header+8,256); put32(header+12,181); put32(header+16,RECORD_BYTES); put32(header+20,games); put64(header+24,seed);
    for(int i=0;i<32;i++) { unsigned h; if(sscanf(argv[8]+2*i,"%2x",&h)!=1) exit(2); header[32+i]=h; }
    if(fwrite(header,1,64,f)!=64) exit(2);
    long records=0,w=0,l=0,d=0,turns=0; long mix[4]={0};
    for(long g=0;g<games;g++) {
        Rng meta={hash64(seed^((uint64_t)(g+1)*0xA0761D6478BD642FULL))};
        int self=rng_int(&meta,2)?rng_int(&meta,168):168+rng_int(&meta,8);
        int foe=rng_int(&meta,2)?rng_int(&meta,168):168+rng_int(&meta,8);
        int seat=rng_int(&meta,2),opp=g%4; mix[opp]++;
        Mon a=population[self],b=population[foe]; a.policy=POL_LEARNED;
        int snapshot=nsnap?rng_int(&meta,nsnap):0;
        b.policy=opp==0?POL_GENERIC:opp==1?POL_OBSH2:POL_OBSDEMO;
        if(opp==3) { b.policy=POL_OBSDEMO; observable_policy=snapshot_policy; opponent_weights=nsnap?&snapshots[snapshot]:&actor; }
        else observable_policy=obsdemo;
        // Domain-separated per battle AND actor; neither stream is battle RNG.
        policy_rng.s=hash64(seed ^ ((uint64_t)(g+1)*0xE7037ED1A0B428DBULL) ^ ((uint64_t)seat*0x8EBC6AF09C88C6E3ULL));
        Rng r={hash64(seed ^ ((uint64_t)(g+1)*0xD1B54A32D192ED03ULL))};
        ndecision=0; learned_policy=collect_policy; int t,res=battle(seat?&b:&a,seat?&a:&b,&r,&t);
        int score=res==2?1:res==seat?2:0; w+=score==2; l+=score==0; d+=score==1; turns+=t;
        for(int k=0;k<ndecision;k++) {
            unsigned char *record=decisions[k]; record[368]=score; record[369]=seat; record[370]=opp==3?3+snapshot:opp; record[371]=t;
            put16(record+372,self); put16(record+374,foe); put32(record+376,g);
            if(fwrite(record,1,RECORD_BYTES,f)!=RECORD_BYTES) exit(2);
            records++;
        }
    }
    if(fclose(f)) exit(2);
    printf("{\"games\":%ld,\"decisions\":%ld,\"bytes\":%ld,\"w\":%ld,\"l\":%ld,\"d\":%ld,\"turns\":%ld,\"mixture\":[%ld,%ld,%ld,%ld],\"fallbacks\":%lu}\n",games,records,64+records*RECORD_BYTES,w,l,d,turns,mix[0],mix[1],mix[2],mix[3],obs_fallbacks);
}
static void evaluate(char **argv) {
    // evaluate population prefix games seed model
    long n=strtol(argv[5],NULL,10); uint64_t seed=strtoull(argv[6],NULL,10); if(n<1) exit(2);
    load_weights(argv[7],&actor); learned_weights=&actor; learned_policy=learned;
    char path[1024]; snprintf(path,sizeof path,"%s.csv",argv[4]); FILE *csv=fopen(path,"w");
    snprintf(path,sizeof path,"%s.bin",argv[4]); FILE *raw=fopen(path,"wb"); if(!csv || !raw) exit(2);
    fprintf(csv,"population,opponent,seat,n,w,l,d,turns,cap,offset,fallbacks\n"); long offset=0;
    for(int pop=0;pop<2;pop++) for(int opponent=0;opponent<3;opponent++) for(int seat=0;seat<2;seat++) {
        long w=0,l=0,d=0,cap=0,turns=0; unsigned long before=obs_fallbacks;
        for(long g=0;g<n;g++) {
            Rng meta={hash64(seed^((uint64_t)(g+1)*0xA0761D6478BD642FULL))};
            int self=rng_int(&meta,168),foe=pop?168+rng_int(&meta,8):rng_int(&meta,168);
            Mon a=population[self],b=population[foe]; a.policy=POL_LEARNED; b.policy=opponent==0?POL_OBSH2:opponent==1?POL_GENERIC:POL_LEARNED;
            Rng r={hash64(seed ^ ((uint64_t)(g+1)*0xD1B54A32D192ED03ULL))};
            int t,res=battle(seat?&b:&a,seat?&a:&b,&r,&t); int score=res==2?1:res==seat?2:0;
            w+=score==2; l+=score==0; d+=score==1; cap+=t==MAX_TURN; turns+=t;
            unsigned char record[2]={(unsigned char)score,(unsigned char)t}; if(fwrite(record,1,2,raw)!=2) exit(2);
        }
        fprintf(csv,"%s,%s,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%lu\n",pop?"final168_x_bench8_ST":"final168_x_final168",opponent==0?"obsh2":opponent==1?"generic":"learned",seat,n,w,l,d,turns,cap,offset,obs_fallbacks-before); offset+=2*n; fflush(csv);
    }
    if(fclose(csv) || fclose(raw)) exit(2);
    printf("evaluation games=%ld fallbacks=%lu\n",12*n,obs_fallbacks);
}
int stage3_cli_main(int argc,char **argv) {
    if(argc<8 || strcmp(argv[1],"stage3")) { fprintf(stderr,"loop stage3 generate|evaluate population output n seed model [hash snapshots...]\n"); return 2; }
    SEQ_MODE=0; OBS_FORCE=1; OBS_PROFILE=OBS_REGULAR; obs_sink=NULL; read_population(argv[3]);
    if(!strcmp(argv[2],"generate")) generate(argc,argv);
    else if(!strcmp(argv[2],"evaluate")) evaluate(argv);
    else return 2;
    return obs_fallbacks?1:0;
}

#ifndef STAGE3_LIBRARY
int main(int argc,char **argv) { return stage3_cli_main(argc,argv); }
#endif
