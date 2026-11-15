// Pure observation-only deployment. No Mon, observer, resolution RNG or labels.
#define LEARNED_INPUT 415
#define LEARNED_H1 64
#define LEARNED_H2 32
typedef struct {
    float w1[64*415], b1[64], w2[32*64], b2[32];
    float wp[5*32], bp[5], wv[32], bv[1], prior;
} LearnedWeights;
#include "scales.h"
#ifndef LEARNED_WEIGHTS_HEADER
#define LEARNED_WEIGHTS_HEADER "weights.h"
#endif
#include LEARNED_WEIGHTS_HEADER
static const LearnedWeights *learned_weights = &learned_default;
static void learned_encode(const Observation *o, float x[LEARNED_INPUT]) {
    const int32_t *v=o->v; int n=181;
    for(int i=0;i<181;i++) x[i]=fmaxf(-1,fminf(1,(float)v[i]/learned_scale[i]));
    const int zero[]={42,77,78,112,114,117,49,53,57,61,80,88,96,104};
    for(unsigned i=0;i<sizeof zero/sizeof *zero;i++) x[zero[i]]=0;
    for(int k=0;k<8;k++) {
        int s=k<4?48+4*k:79+8*(k-4);
        for(int m=0;m<17;m++) x[n++]=v[s] && v[s+1]==m;
    }
    for(int m=0;m<35;m++) x[n++]=v[42]==m;
    for(int m=0;m<35;m++) x[n++]=v[111] && v[112]==m;
    const int at[]={117,114,77,78}, bits[]={18,4,3,3};
    for(int k=0;k<4;k++) for(int j=0;j<bits[k];j++) x[n++]=(v[at[k]]>>j)&1;
}
static void learned_forward(const Observation *o, const ActionMask *mask,
                            const LearnedWeights *w, float logits[5], float *value) {
    float x[LEARNED_INPUT],a[64],b[32]; learned_encode(o,x);
    for(int i=0;i<64;i++) {
        float z=w->b1[i]; for(int j=0;j<LEARNED_INPUT;j++) z+=w->w1[i*LEARNED_INPUT+j]*x[j];
        a[i]=fmaxf(0,z);
    }
    for(int i=0;i<32;i++) {
        float z=w->b2[i]; for(int j=0;j<64;j++) z+=w->w2[i*64+j]*a[j]; b[i]=fmaxf(0,z);
    }
    int prior=obsh2(o,mask);
    for(int i=0;i<5;i++) {
        float z=w->bp[i]+(i==prior?w->prior:0);
        for(int j=0;j<32;j++) z+=w->wp[i*32+j]*b[j];
        logits[i]=mask->legal[i]?z:-1e9f;
    }
    float z=w->bv[0]; for(int j=0;j<32;j++) z+=w->wv[j]*b[j]; *value=tanhf(z);
}
static int learned(const Observation *o, const ActionMask *mask) {
    float logits[5],value; learned_forward(o,mask,learned_weights,logits,&value);
    int best=-1; for(int i=0;i<5;i++) if(mask->legal[i] && (best<0 || logits[i]>logits[best])) best=i;
    return best;
}
