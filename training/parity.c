// Binary test adapter only: queries are already frozen MGO1 + masks.
#define STAGE3_LIBRARY
#include "loop.c"
int main(int argc,char **argv) {
    if(argc!=4) return 2;
    LearnedWeights weights; load_weights(argv[1],&weights);
    FILE *in=fopen(argv[2],"rb"),*out=fopen(argv[3],"wb"); if(!in || !out) return 2;
    unsigned char record[1037]; long n=0;
    while(fread(record,1,sizeof record,in)==sizeof record) {
        if(memcmp(record,"MGO1",4) || get32(record+4)!=256) return 2;
        Observation o; ActionMask m;
        for(int i=0;i<256;i++) o.v[i]=(int32_t)get32(record+8+4*i);
        for(int i=0;i<5;i++) m.legal[i]=record[1032+i];
        float x[415],logits[5],value; learned_encode(&o,x); learned_forward(&o,&m,&weights,logits,&value);
        unsigned char b[4];
        for(int i=0;i<415;i++) { putfloat(b,x[i]); if(fwrite(b,1,4,out)!=4) return 2; }
        for(int i=0;i<5;i++) { putfloat(b,logits[i]); if(fwrite(b,1,4,out)!=4) return 2; }
        putfloat(b,value); if(fwrite(b,1,4,out)!=4) return 2; n++;
    }
    if(ferror(in) || !feof(in) || fclose(in) || fclose(out)) return 2;
    printf("parity queries=%ld\n",n); return 0;
}
