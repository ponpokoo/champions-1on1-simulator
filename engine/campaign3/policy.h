// Pure MGO2 inference. Only Observation2 and the frozen own-action mask enter.
#define LEARNED2_INPUT 2334
#define LEARNED2_H1 64
#define LEARNED2_H2 32
typedef struct {
    float w1[64*2334], b1[64], w2[32*64], b2[32];
    float wp[5*32], bp[5], wv[32], bv[1], prior;
} Learned2Weights;
#include "encoder.h"
#ifndef LEARNED2_WEIGHTS_HEADER
#define LEARNED2_WEIGHTS_HEADER "weights.h"
#endif
#include LEARNED2_WEIGHTS_HEADER
static const Learned2Weights *learned2_weights = &learned2_default;
static void learned2_encode(const Observation2 *o, float x[LEARNED2_INPUT]) {
    const int32_t *v=o->v; int n=322;
    for(int i=0;i<322;i++) x[i]=fmaxf(-1,fminf(1,(float)v[i]/learned2_scale[i]));
    for(unsigned i=0;i<sizeof learned2_zero/sizeof *learned2_zero;i++) x[learned2_zero[i]]=0;
    for(int k=0;k<24;k++) {
        int s=learned2_moves[k][0],valid=learned2_moves[k][1];
        for(int m=0;m<69;m++) x[n++]=v[valid] && v[s]==m;
    }
    const int plus[]={45,46,118};
    for(int k=0;k<3;k++) for(int m=1;m<=69;m++) x[n++]=v[plus[k]]==m;
    for(int m=0;m<35;m++) x[n++]=v[42]==m;
    for(int m=0;m<35;m++) x[n++]=v[111] && v[112]==m;
    for(int bit=0;bit<69;bit++) x[n++]=((uint32_t)v[222+bit/32]>>(bit%32))&1;
    const int at[]={114,77,78},bits[]={4,3,3};
    for(int k=0;k<3;k++) for(int j=0;j<bits[k];j++) x[n++]=((uint32_t)v[at[k]]>>j)&1;
    if(n!=LEARNED2_INPUT) abort();
}
static void learned2_forward(const Observation2 *o, const ActionMask *mask,
                             const Learned2Weights *w, float logits[5], float *value) {
    float x[LEARNED2_INPUT],a[64],b[32]; learned2_encode(o,x);
    for(int i=0;i<64;i++) {
        float z=w->b1[i];
        for(int j=0;j<LEARNED2_INPUT;j++) z+=w->w1[i*LEARNED2_INPUT+j]*x[j];
        a[i]=fmaxf(0,z);
    }
    for(int i=0;i<32;i++) {
        float z=w->b2[i]; for(int j=0;j<64;j++) z+=w->w2[i*64+j]*a[j]; b[i]=fmaxf(0,z);
    }
    int prior=obsh3(o,mask);
    for(int i=0;i<5;i++) {
        float z=w->bp[i]+(i==prior?w->prior:0);
        for(int j=0;j<32;j++) z+=w->wp[i*32+j]*b[j];
        logits[i]=mask->legal[i]?z:-1e9f;
    }
    float z=w->bv[0]; for(int j=0;j<32;j++) z+=w->wv[j]*b[j]; *value=tanhf(z);
}
static int learned2(const Observation2 *o, const ActionMask *mask) {
    float logits[5],value; learned2_forward(o,mask,learned2_weights,logits,&value);
    int best=-1;
    for(int i=0;i<5;i++) if(mask->legal[i] && (best<0 || logits[i]>logits[best])) best=i;
    return best;
}
