// analogBEAST-field 0.3.14 - 0.3.8 four-column UI, five push choices on SHIFT+TURN, 0.3.12 DSP fixes / OP-1 Field 1.7.3
// Source implementation, fixed-point, six-voice level 10 engine.
// The analogue filter models are approximations, NOT bit-identical GOLANA filters.
#include "build_r6e_card.h"
typedef unsigned int u32;
extern "C" const Op1fCard op1f_card_analogbeast OP1F_NAMED("op1f_card_analogbeast");

enum { ASK_ENGINE=0, ASK_KEEPS=1, FRAMES=128, VOICES=6, SLOTS=8, INPUT_BYTES=36, FRAME_BYTES=40, BLOCK_BYTES=52 };
// Native indices are fixed for the OP-1 LFO and saved synth knobs.
enum { CUT, RES, CTRL1, FOLD, MIX1, MIX2, MIXN, SEMI, NATIVE_COUNT };
// Extra state is kept alongside synth presets by open-field's level-7 keeps ask.
enum { W1, W2, MOD2, CTRL2, TUNE2, FTYPE, KEYTR, EGINT, DIST, FATT, FDEC, FSUS, FREL, EGRESET, EXTRA_COUNT };
static const int INIT[EXTRA_COUNT]={0,0,0,0,0,0,32,0,0,0,64,64,50,0};
static const int MINEX[EXTRA_COUNT]={0,0,0,0,-63,0,-63,-63,0,0,0,0,0,0};
static const int MAXEX[EXTRA_COUNT]={3,2,3,127,63,4,63,63,1,127,127,127,127,1};
static volatile int extras[SLOTS][EXTRA_COUNT];
static volatile u32 known[SLOTS], load_pending[SLOTS], dirty[SLOTS];
static volatile u32 current_slot=0,shown_flags=0,ui_page=0,help_open=0;
static volatile u32 reset_generation[SLOTS]; // momentary EG RESET, not saved
static volatile int turns[3][2][4], clicks[3][2][4];
static u32 stood=0;

static int clamp(int x,int a,int b){return x<a?a:x>b?b:x;}
static int abs_i(int a){return a<0?-a:a;}
static int qmul(int a,int b){return (a*b)>>15;} // callers clamp to avoid overflow
static int sat16(int a){return clamp(a,-32768,32767);}
static u32 noise32(u32* v){u32 x=*v;x^=x<<13;x^=x>>17;x^=x<<5;*v=x;return x;}
static int saw(u32 p){return (int)(p>>16)-32768;}
static int tri(u32 p){u32 x=p>>15;return x<65536u?(int)x-32768:98303-(int)x;}
static int sine(u32 p){int x=(int)p>>16;int a=abs_i(x);return sat16((x*(32768-a))>>13);}
static int pulse(u32 p,u32 width){return p<width?32767:-32767;}
static const unsigned short RATIOS[49]={1024,1085,1149,1218,1290,1367,1448,1534,1625,1722,1825,1933,2048,2170,2299,2435,2580,2734,2896,3069,3251,3444,3649,3866,4096,4340,4598,4871,5161,5468,5793,6137,6502,6889,7298,7732,8192,8679,9195,9742,10321,10935,11585,12274,13004,13777,14596,15464,16384};
static u32 tuned(u32 step,int semitone,int cents){
    // 32-bit arithmetic only; pitch fine-tune approximately +/- 63 cents.
    int centshift=cents*20; // Q15 fractional change approximation near unison
    u32 r=RATIOS[clamp(semitone,-24,24)+24];
    if(step>0x0fffffffu)step=0x0fffffffu;
    u32 out=(step>>12)*r;
    // avoid 64-bit and signed overflow: cents shifts by <=4%.
    int d=(int)(out>>12)*centshift;
    if(d>0 && out>(u32)(0x7fffffffu-(u32)d))return 0x7fffffffu;
    return (u32)((int)out+d);
}

// Native 8 indices follow the stable LFO mapping. All other settings by slot.
struct Voice {
    u32 p1,p2,nseed,l1phase;
    int lp,bp,lp2,bp2,eg,eg_stage;
    u32 started;
};
static_assert(sizeof(Voice)==44,"per-voice RAM");
static int filter_one(int x,int g,int damp,int *lp,int *bp,int tap){
    // Bounded digital state-variable filter, Q15 coefficient and signals.
    int hp=sat16(x-*lp-qmul(damp,*bp));
    int b=sat16(*bp+qmul(g,hp));
    int l=sat16(*lp+qmul(g,b));
    *bp=b;*lp=l;
    return tap==1?b:tap==2?hp:l;
}
static int folded(int x,int amount){
    if(!amount)return x;
    int gain=4096+amount*240; // 1..~8.4 in Q12
    int v=(x*(gain>>4))>>8;
    int pos=(int)((u32)(v+32768)&0x1ffffu);
    return sat16(pos<65536?pos-32768:98303-pos);
}
static int soft(int x){
    x=clamp(x,-49152,49151);
    if(x>16384)x=16384+((x-16384)>>2);
    if(x< -16384)x=-16384+((x+16384)>>2);
    return sat16(x);
}

extern "C" void abfield_render(const Op1fVoiceBlock* b) OP1F_NAMED("abfield_render");
extern "C" void abfield_render(const Op1fVoiceBlock* b){
    int* out=b->out;
    if(b->bytes<BLOCK_BYTES || !b->state || !b->values){for(u32 i=0;i<FRAMES;++i)out[i]=0;return;}
    Voice* v=(Voice*)b->state;
    u32 slot=b->slot<SLOTS?b->slot:0;
    const int* p=b->values;
    int e[EXTRA_COUNT];for(int j=0;j<EXTRA_COUNT;++j)e[j]=extras[slot][j];
    if(b->events&(u32)OP1F_VOICE_FRESH){
        v->p1=v->p2=v->l1phase=0;v->nseed=0x9e3779b9u^(b->voice*0x01010101u);
        v->lp=v->bp=v->lp2=v->bp2=v->eg=v->eg_stage=0;v->started=0;
    }
    if(b->events&(u32)OP1F_VOICE_BEGINS){
        v->started=(reset_generation[slot]<<1)|1u;v->p1=v->p2=0;
        v->eg_stage=1;
    }
    if(b->events&(u32)OP1F_VOICE_ENDS)v->eg_stage=3;
    if(!v->started){for(u32 i=0;i<FRAMES;++i)out[i]=0;return;}
    u32 generation=reset_generation[slot]&0x7fffffffu;
    if((v->started>>1)!=generation){
        v->eg=0;v->eg_stage=1;v->lp=v->bp=v->lp2=v->bp2=0;
        v->started=(generation<<1)|1u;
    }
    int ctrl=clamp(p[CTRL1],0,100),level1=clamp(p[MIX1],0,100),level2=clamp(p[MIX2],0,100),leveln=clamp(p[MIXN],0,100);
    int semi=clamp(p[SEMI],-24,24),w1=e[W1],w2=e[W2],mod=e[MOD2],type=e[FTYPE];
    u32 step1=b->step,step2=tuned(step1,semi,e[TUNE2]);
    int cutoff=clamp(p[CUT]+(((int)(b->step>>21)-10)*e[KEYTR]>>5),0,127);
    // cutoff Q15 SVF coefficient: low frequencies small, high frequencies moderate.
    int baseg=60+((cutoff*cutoff)>>1); // [60, 8124]
    int damping=clamp(31000-p[RES]*287,1800,31000);
    int amt=e[EGINT];
    int atkShift=4+(e[FATT]>>4),decShift=6+(e[FDEC]>>4),relShift=6+(e[FREL]>>4);
    if(atkShift>20)atkShift=20;
    if(decShift>20)decShift=20;
    if(relShift>20)relShift=20;
    const int sust=(e[FSUS]*32767)>>7;
    u32 ph1=v->p1,ph2=v->p2,ns=v->nseed,lphase=v->l1phase;
    int lp=v->lp,bp=v->bp,lp2=v->lp2,bp2=v->bp2,eg=v->eg,stage=v->eg_stage;
    const u32 width=0x80000000u-(u32)ctrl*0x011f5c28u;
    for(u32 i=0;i<FRAMES;++i){
        if(stage==1){eg+=(32767-eg)>>atkShift;if(eg>=32500){eg=32767;stage=2;}}
        else if(stage==2){eg+=(sust-eg)>>decShift;}
        else if(stage==3){eg-=eg>>relShift;if(eg<4){eg=0;stage=0;}}
        // For EG intensity 0 the coefficient is block-constant: do not
        // recompute cutoff, clamp or square it for every audio sample.
        int fcut=cutoff, g=baseg;
        if(amt){fcut=clamp(cutoff+((eg*amt)>>15),0,127);
            if(fcut!=cutoff)g=60+((fcut*fcut)>>1);}
        int a=0,bb=0;
        switch(w1){
            case 1:a=pulse(ph1,width);break;
            case 2:a=tri(ph1);if(ctrl)a=folded(a,ctrl);break;
            case 3:a=sine(ph1);break;
            default:a=saw(ph1);if(ctrl){int hi=saw(ph1<<1);a=sat16(a+(((hi-a)*(ctrl*327))>>15));}break;
        }
        // OSC2 need not be evaluated if it cannot reach either mixer or
        // OSC1 phase modulation; its phase is still advanced below.
        if(level2 || (w1==3 && ctrl))
            switch(w2){case 1:bb=pulse(ph2,0x80000000u);break;case 2:bb=tri(ph2);break;default:bb=saw(ph2);break;}
        // OSC1 CTRL2 is slow vibrato for every waveform (0.3.12 DSP fix).
        if(w1==3 && ctrl){a=sine(ph1+(u32)(bb*ctrl*128));}
        if(mod==1||mod==3)bb=qmul(a,bb);
        // Keep RNG state advancing, but skip noise mixing when its level is zero.
        u32 rand=noise32(&ns);
        int n=leveln?((int)(rand>>16)-32768):0;
        int m=sat16(((a*level1)>>7)+((bb*level2)>>7)+((n*leveln)>>7));
        m>>=1; // preserve headroom for the three sources
        m=folded(m,clamp(p[FOLD],0,100));
        if(type==4)m=soft(m<<1);
        int y=filter_one(m,g,damping,&lp,&bp,type==2?1:type==3?2:0);
        if(type==0)y=filter_one(y,g,26000,&lp2,&bp2,0); // 24dB LP
        if(fcut>=127 && (type==0 || type==1))y=m; // completely open LP filter
        if(e[DIST])y=soft(y<<2);
        out[i]=(int)((u32)sat16(y)<<16);
        int vibrato=(sine(lphase)*e[CTRL2])>>15;
        int pitch_offset=((int)(step1>>9)*vibrato)>>4;
        u32 prior=ph1;ph1+=step1+(u32)pitch_offset;ph2+=step2;
        if((mod==2||mod==3)&&ph1<prior)ph2=0;
        lphase+=0x00009600u;
    }
    v->p1=ph1;v->p2=ph2;v->nseed=ns;v->l1phase=lphase;
    v->lp=lp;v->bp=bp;v->lp2=lp2;v->bp2=bp2;v->eg=eg;v->eg_stage=stage;
}

static void int_text(char* dst,int x){
    int i=0;if(x<0){dst[i++]='-';x=-x;}
    char rev[12];int n=0;
    do{int q=0;while(x>=10){x-=10;++q;}rev[n++]=(char)('0'+x);x=q;}while(x && n<11);
    while(n){dst[i++]=rev[--n];}
    dst[i]=0;
}
static int json_number(const char** pp,int* output){
    const char* p=*pp;while(*p==' '||*p=='\t')++p;
    int sign=1;if(*p=='-'){sign=-1;++p;}
    if(*p<'0'||*p>'9')return 0;
    int n=0;while(*p>='0'&&*p<='9'){n=n*10+(*p-'0');++p;if(n>99999)return 0;}
    *pp=p;*output=n*sign;return 1;
}
static int parse_extra(const char* txt,int* values){
    if(*txt!='[')return 0;
    ++txt;
    for(int i=0;i<EXTRA_COUNT;++i){
        int n;if(!json_number(&txt,&n))return 0;
        values[i]=clamp(n,MINEX[i],MAXEX[i]);
        if(i<EXTRA_COUNT-1){if(*txt!=',')return 0;++txt;}
    }
    return *txt==']';
}
static void put(char* buf,int* pos,const char* s){while(*s && *pos<190)buf[(*pos)++]=*s++;buf[*pos]=0;}
static void extra_json(unsigned slot,char* buf){
    int at=0;put(buf,&at,"[");
    for(int i=0;i<EXTRA_COUNT;++i){char val[16];int_text(val,extras[slot][i]);put(buf,&at,val);if(i<EXTRA_COUNT-1)put(buf,&at,",");}
    put(buf,&at,"]");
}
extern "C" void abfield_changed(unsigned where,unsigned which,unsigned how) OP1F_NAMED("abfield_changed");
extern "C" void abfield_changed(unsigned where,unsigned which,unsigned how){
    if(where==(u32)OP1F_KEEP_SYNTH && which<SLOTS && (how==(u32)OP1F_KEPT_LOADED || how==(u32)OP1F_KEPT_START || how==(u32)OP1F_KEPT_DROPPED))load_pending[which]=1;
}
static void load_extra(unsigned slot){
    char buf[192];int tmp[EXTRA_COUNT];
    int n=op1f_value_get(&op1f_card_analogbeast,OP1F_KEEP_SYNTH,slot,"abfield_extra",buf,sizeof(buf));
    if(n==OP1F_NOT_NOW)return;
    for(int j=0;j<EXTRA_COUNT;++j)tmp[j]=INIT[j];
    if(n>=0){buf[sizeof(buf)-1]=0;if(!parse_extra(buf,tmp))for(int j=0;j<EXTRA_COUNT;++j)tmp[j]=INIT[j];}
    tmp[EGRESET]=0; // legacy stored toggle is ignored
    for(int j=0;j<EXTRA_COUNT;++j)extras[slot][j]=tmp[j];
    known[slot]=1;load_pending[slot]=0;dirty[slot]=0;
}
static const int NMIN[NATIVE_COUNT]={0,0,0,0,0,0,0,-24};
static const int NMAX[NATIVE_COUNT]={127,100,100,100,100,100,100,24};
static void change_value(unsigned slot,int id,int delta){
    if(id<0 || !delta)return;
    // Five discrete choices moved from press to SHIFT+TURN. Wrap in both directions.
    if(id==NATIVE_COUNT+W1 || id==NATIVE_COUNT+W2 ||
       id==NATIVE_COUNT+MOD2 || id==NATIVE_COUNT+FTYPE || id==NATIVE_COUNT+DIST){
        int j=id-NATIVE_COUNT,range=MAXEX[j]-MINEX[j]+1;
        int next=extras[slot][j]-MINEX[j];
        while(delta>0){next=(next+1==range)?0:next+1;--delta;}
        while(delta<0){next=(next==0)?range-1:next-1;++delta;}
        extras[slot][j]=MINEX[j]+next;dirty[slot]=1;
        return;
    }
    if(id<NATIVE_COUNT){
        int value=0;
        if(op1f_param_get(&op1f_card_analogbeast,ASK_ENGINE,slot,(unsigned)id,&value)==0)
            op1f_param_set(&op1f_card_analogbeast,ASK_ENGINE,slot,(unsigned)id,clamp(value+delta,NMIN[id],NMAX[id]));
    }else{
        int j=id-NATIVE_COUNT;
        if(j>=EXTRA_COUNT)return;
        extras[slot][j]=clamp(extras[slot][j]+delta,MINEX[j],MAXEX[j]);dirty[slot]=1;
    }
}
static int control(int page,int shifted,int knob){
    // non-native extra index is offset by 8.
    if(page==0){
        static const int plain[4]={MIX1,MIX2,SEMI,MIXN};
        static const int sh[4]={CTRL1,NATIVE_COUNT+CTRL2,NATIVE_COUNT+TUNE2,FOLD};
        return shifted?sh[knob]:plain[knob];
    }
    if(page==1){
        static const int plain[4]={CUT,RES,NATIVE_COUNT+KEYTR,NATIVE_COUNT+EGINT};
        static const int sh[4]={NATIVE_COUNT+FTYPE,NATIVE_COUNT+W1,NATIVE_COUNT+MOD2,NATIVE_COUNT+W2};
        return shifted?sh[knob]:plain[knob];
    }
    static const int adsr[4]={NATIVE_COUNT+FATT,NATIVE_COUNT+FDEC,NATIVE_COUNT+FSUS,NATIVE_COUNT+FREL};
    static const int plain[4]={NATIVE_COUNT+DIST,-1,NATIVE_COUNT+EGRESET,-1};
    return shifted?adsr[knob]:plain[knob];
}
static int clicked(int page,int knob){
    if(knob==3)return -2; // NEXT / PREV, unchanged
    if(page==0){
        static const int selections[3]={W1,W2,MOD2};
        return NATIVE_COUNT+selections[knob];
    }
    if(page==1 && knob==0)return NATIVE_COUNT+FTYPE;
    if(page==2 && knob==2)return NATIVE_COUNT+EGRESET;
    return -1;
}
extern "C" int abfield_input(const Op1fInput* e) OP1F_NAMED("abfield_input");
extern "C" int abfield_input(const Op1fInput* e){
    if(e->bytes<INPUT_BYTES || !e->mine)return 0;
    int shift=e->shift?1:0;
    if(e->key==0 && (e->what==(u32)OP1F_DOWN || e->what==(u32)OP1F_DOWN_SHIFT)) { help_open^=1u; return 1; }
    if(e->key==0 && e->what==(u32)OP1F_UP) return 1;
    if(e->what==(u32)OP1F_TURN && e->key<4){
        turns[ui_page][shift][e->key]+=e->turn>>10;
        return 1;
    }
    // 54..57 are the click events (on release); 61..64 are physical holds.
    if(e->key>=54 && e->key<=57 && (e->what==(u32)OP1F_DOWN || e->what==(u32)OP1F_DOWN_SHIFT)){
        clicks[ui_page][e->what==(u32)OP1F_DOWN_SHIFT][e->key-54]++;
        return 1;
    }
    if(e->key>=54 && e->key<=57 && e->what==(u32)OP1F_UP)return 1;
    return 0;
}
static void events(unsigned slot){
    for(int page=0;page<3;++page)for(int shift=0;shift<2;++shift)for(int knob=0;knob<4;++knob){
        int n=turns[page][shift][knob];turns[page][shift][knob]=0;
        if(n){
            int id=control(page,shift,knob);
            if(id==NATIVE_COUNT+EGRESET){
                // A turn in either direction produces a single momentary reset.
                reset_generation[slot]=(reset_generation[slot]+1u)&0x7fffffffu;
                extras[slot][EGRESET]=0;
            }else change_value(slot,id,n);
        }
    }
    for(int page=0;page<3;++page)for(int shift=0;shift<2;++shift)for(int knob=0;knob<4;++knob){
        int n=clicks[page][shift][knob];clicks[page][shift][knob]=0;
        while(n-->0){
            int id=clicked(page,knob);
            if(id==-2){u32 next=ui_page+(shift?2u:1u);if(next>=3u)next-=3u;ui_page=next;continue;}
            if(id<0)continue;
            if(id==NATIVE_COUNT+EGRESET){
                reset_generation[slot]=(reset_generation[slot]+1u)&0x7fffffffu;
                extras[slot][EGRESET]=0;
                continue;
            }
            int j=id-NATIVE_COUNT,range=MAXEX[j]-MINEX[j]+1;
            int v=extras[slot][j]-MINEX[j]+(shift?-1:1);
            if(v<0)v+=range;
            if(v>=range)v-=range;
            extras[slot][j]=MINEX[j]+v;dirty[slot]=1;
        }
    }
}
// Display: 480x240, no top banner. Knobs left 2x2, click value cards right 2x2.
// Physical knob colours: blue, ochre-yellow, light grey, orange.
static const unsigned COLOR[4]={OP1F_COLOUR(91,177,245),OP1F_COLOUR(247,215,46),OP1F_COLOUR(232,234,236),OP1F_COLOUR(255,127,42)};
static const unsigned DARK[4]={OP1F_COLOUR(10,34,70),OP1F_COLOUR(59,47,5),OP1F_COLOUR(46,48,50),OP1F_COLOUR(61,29,9)};
static const unsigned BG=OP1F_COLOUR(2,4,6),WHITE=OP1F_COLOUR(247,248,249),MUTED=OP1F_COLOUR(102,113,122);
static const char* const ROT[3][4]={{"OSC1 LEVEL","OSC2 LEVEL","OSC2 SEMI","NOISE LEVEL"},{"CUTOFF","RES","KEY TRACK","EG INT"},{"DIST","","EG RESET",""}};
static const char* const SHROT[3][4]={{"OSC1 CTRL1","OSC1 CTRL2","OSC2 TUNE","FOLD"},{"FILTER TYPE","OSC1 WAVE","OSC2 MOD","OSC2 WAVE"},{"ATTACK","DECAY","SUSTAIN","RELEASE"}};
static const char* const PRESSES[3][4]={{"","","","PAGE"},{"","","","PAGE"},{"","","EG RESET","PAGE"}};
static const char* const NW1[4]={"SAW","PULSE","TRI","SINE"};
static const char* const NW2[3]={"SAW","SQUARE","TRI"};
static const char* const NMOD[4]={"OFF","RING","SYNC","BOTH"};
static const char* const NFTYPE[5]={"LPF24","LPF12","BPF","HPF","MS20"};
static int letters(const char* v){int n=0;while(v[n])++n;return n;}
static const unsigned char FONT5X7[][7]={
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'A'
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // 'B'
    {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F}, // 'C'
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // 'D'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // 'E'
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // 'F'
    {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F}, // 'G'
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'H'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}, // 'I'
    {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}, // 'J'
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // 'K'
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // 'L'
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // 'M'
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // 'N'
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'O'
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // 'P'
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // 'Q'
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // 'R'
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // 'S'
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // 'T'
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'U'
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // 'V'
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // 'W'
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // 'X'
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // 'Y'
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // 'Z'
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // '0'
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // '1'
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // '2'
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, // '3'
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // '4'
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // '5'
    {0x07,0x08,0x10,0x1E,0x11,0x11,0x0E}, // '6'
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // '7'
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // '8'
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x1C}, // '9'
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // '+'
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // '-'
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, // '/'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // '.'
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, // ':'
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // '?'
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // '('
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // ')'
    {0x00,0x1F,0x00,0x1F,0x00,0x00,0x00}, // '='
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // '_'
};
static const char GLYPHS[]=" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-/.:?()=_";
static int glyph_index(char c){
    if(c>='a' && c<='z')c=(char)(c-32);
    for(int k=0;GLYPHS[k];++k)if(GLYPHS[k]==c)return k;
    return 0;
}
// Independent, readable geometric sans font. Draw continuous pixel runs,
// reducing draw calls compared with painting individual font pixels.
static void ui_text(void* c,int x,int y,const char* str,unsigned color,int scale){
    for(int a=0;str[a];++a){
        const unsigned char* g=FONT5X7[glyph_index(str[a])];
        for(int yy=0;yy<7;++yy){
            unsigned bits=g[yy];
            for(int xx=0;xx<5;){
                if(!(bits&(16u>>xx))){++xx;continue;}
                int start=xx;
                do{++xx;}while(xx<5 && (bits&(16u>>xx)));
                op1f_paint_box(c,x+(a*6+start)*scale,y+yy*scale,(xx-start)*scale,scale,color);
            }
        }
    }
}
static void ui_center(void* c,int x,int y,const char* str,unsigned color,int scale){
    ui_text(c,x-(letters(str)*6*scale)/2,y,str,color,scale);
}
// A two-line parameter heading; never reduce the font for long names.
static void column_label(void* c,int cx,int y,const char* label,unsigned color,bool force_two){
    int n=letters(label);
    if(n<=9 && !force_two){ui_center(c,cx,y+8,label,color,2);return;}
    const char* split=0;
    for(const char* q=label;*q;++q)if(*q==' ')split=q;
    if(!split){ui_center(c,cx,y+8,label,color,1);return;}
    char a[20];int z=0;
    while(label+z<split && z<19){a[z]=label[z];++z;}
    a[z]=0;
    // Prefer splitting at the last word. Both rows retain 2x scale.
    if(z<=9 && letters(split+1)<=9){
        ui_center(c,cx,y,a,color,2);
        ui_center(c,cx,y+20,split+1,color,2);
    }else{
        // Fallback to a single clean 1x line if a word cannot fit.
        ui_center(c,cx,y+8,label,color,1);
    }
}
static int value_for(unsigned slot,int id){
    int val=0;if(id<0)return 0;
    if(id<NATIVE_COUNT){op1f_param_heard(&op1f_card_analogbeast,ASK_ENGINE,slot,(u32)id,&val);return val;}
    return extras[slot][id-NATIVE_COUNT];
}
static void rect_outline(void* c,int x,int y,int w,int h,unsigned col){
    op1f_paint_box(c,x,y,w,2,col);
    op1f_paint_box(c,x,y+h-2,w,2,col);
    op1f_paint_box(c,x,y,2,h,col);
    op1f_paint_box(c,x+w-2,y,2,h,col);
}
// Every icon represents an actual parameter value, never a fixed placeholder.
static void line_ui(void* c,int x0,int y0,int x1,int y1,unsigned color){
    op1f_paint_line(c,x0,y0,x1,y1,2,color);
}
static void draw_wave(void* c,int x,int y,int kind,int w,unsigned color){
    // The 88x38px drawing repeats the selected waveform twice.
    for(int j=0;j<2;++j){
        int ox=x+j*43;
        if(kind==0){ // saw
            line_ui(c,ox,y+33,ox+38,y+4,color);line_ui(c,ox+38,y+4,ox+38,y+33,color);
        }else if(kind==1){ // pulse / square
            line_ui(c,ox,y+33,ox,y+5,color);line_ui(c,ox,y+5,ox+19,y+5,color);
            line_ui(c,ox+19,y+5,ox+19,y+33,color);line_ui(c,ox+19,y+33,ox+38,y+33,color);
        }else if(kind==2){ // triangle
            line_ui(c,ox,y+33,ox+19,y+4,color);line_ui(c,ox+19,y+4,ox+38,y+33,color);
        }else{ // sine, piecewise approximation
            int px[9]={0,5,10,15,20,25,30,35,38};
            int py[9]={19,9,4,9,19,29,34,29,19};
            for(int n=1;n<9;++n)line_ui(c,ox+px[n-1],y+py[n-1],ox+px[n],y+py[n],color);
        }
    }
    (void)w;
}
// OSC2 mode visual (88x38): four deliberately distinct, live mode icons.
// OFF: two isolated oscillator traces. RING: intersecting oscillations (multiplication).
// SYNC: two ramps sharing a hard phase reset. BOTH: ring traces plus reset marker.
static void mod2_visual(void* c,int x,int y,int mode,unsigned col){
    mode=clamp(mode,0,3);
    if(mode==0){
        // Independent oscillators: separate traces without a joining link.
        line_ui(c,x+4,y+7,x+19,y+7,col);
        line_ui(c,x+19,y+7,x+30,y+18,col);
        line_ui(c,x+30,y+18,x+43,y+7,col);
        line_ui(c,x+47,y+30,x+61,y+30,col);
        line_ui(c,x+61,y+30,x+72,y+19,col);
        line_ui(c,x+72,y+19,x+84,y+30,col);
    }else if(mode==1 || mode==3){
        // Two interlocking oscillations, phase-offset, with crossing at center.
        for(int i=0;i<4;++i){
            int bx=x+5+i*20;
            line_ui(c,bx,y+20,bx+5,y+7,col);
            line_ui(c,bx+5,y+7,bx+10,y+20,col);
            line_ui(c,bx+10,y+20,bx+15,y+33,col);
            line_ui(c,bx+15,y+33,bx+20,y+20,col);
        }
        if(mode==1){
            // Ring product: crossing marker, not a hard-sync reset.
            line_ui(c,x+40,y+13,x+48,y+23,col);
            line_ui(c,x+48,y+13,x+40,y+23,col);
        }
    }
    if(mode==2){
        // Hard sync: two ramps reset together at x=43.
        line_ui(c,x+4,y+32,x+42,y+5,col);
        line_ui(c,x+42,y+5,x+42,y+32,col);
        line_ui(c,x+42,y+32,x+82,y+5,col);
        line_ui(c,x+4,y+22,x+42,y+10,col);
        line_ui(c,x+42,y+10,x+42,y+22,col);
        line_ui(c,x+42,y+22,x+82,y+10,col);
    }else if(mode==3){
        // Ring + sync: the shared reset edge through the interlocking waves.
        line_ui(c,x+43,y+2,x+43,y+35,col);
        line_ui(c,x+38,y+9,x+43,y+3,col);
        line_ui(c,x+48,y+9,x+43,y+3,col);
    }
}
// One-shot envelope reset: rising attack, decaying slope, and return-to-start arrow.
static void reset_visual(void* c,int x,int y,unsigned col){
    line_ui(c,x+7,y+33,x+22,y+33,col);
    line_ui(c,x+22,y+33,x+34,y+5,col);
    line_ui(c,x+34,y+5,x+52,y+24,col);
    line_ui(c,x+52,y+24,x+76,y+24,col);
    line_ui(c,x+76,y+24,x+83,y+33,col);
    // Curved return path below: next trigger jumps back to the attack.
    line_ui(c,x+72,y+36,x+51,y+38,col);
    line_ui(c,x+51,y+38,x+26,y+38,col);
    line_ui(c,x+26,y+38,x+14,y+32,col);
    line_ui(c,x+14,y+32,x+22,y+31,col);
    line_ui(c,x+14,y+32,x+16,y+24,col);
}
static void signal_visual(void* c,unsigned slot,int id,int x,int y,unsigned col){
    if(id==NATIVE_COUNT+W1 || id==NATIVE_COUNT+W2){
        int j=id-NATIVE_COUNT;
        draw_wave(c,x,y,clamp(extras[slot][j],0,(j==W1?3:2)),88,col);
        return;
    }
    // The two OSC mixer columns also show the real currently selected waves.
    if(id==MIX1 || id==CTRL1 || id==NATIVE_COUNT+CTRL2){
        draw_wave(c,x,y,clamp(extras[slot][W1],0,3),88,col);return;
    }
    // SEMI doubles as the OSC2 modulation status display: never duplicate
    // OSC2's waveform here. The icon follows OFF/RING/SYNC/BOTH live.
    if(id==SEMI || id==NATIVE_COUNT+MOD2){
        mod2_visual(c,x,y,extras[slot][MOD2],col);return;
    }
    if(id==MIX2 || id==NATIVE_COUNT+TUNE2){
        draw_wave(c,x,y,clamp(extras[slot][W2],0,2),88,col);return;
    }
    if(id==CUT || id==RES || id==NATIVE_COUNT+FTYPE || id==NATIVE_COUNT+KEYTR){
        int type=clamp(extras[slot][FTYPE],0,4);
        if(type==2){ // band pass
            line_ui(c,x+5,y+33,x+26,y+31,col);line_ui(c,x+26,y+31,x+47,y+4,col);
            line_ui(c,x+47,y+4,x+65,y+31,col);line_ui(c,x+65,y+31,x+87,y+33,col);
        }else if(type==3){ // high pass
            line_ui(c,x+5,y+33,x+33,y+33,col);line_ui(c,x+33,y+33,x+61,y+5,col);
            line_ui(c,x+61,y+5,x+87,y+5,col);
        }else{ // low pass: actual slope varies by filter type
            int bend=(type==0?19:type==4?14:25);
            line_ui(c,x+5,y+5,x+36,y+5,col);line_ui(c,x+36,y+5,x+62,y+bend,col);
            line_ui(c,x+62,y+bend,x+87,y+33,col);
        }
        return;
    }
    if(id==NATIVE_COUNT+FATT || id==NATIVE_COUNT+FDEC || id==NATIVE_COUNT+FSUS || id==NATIVE_COUNT+FREL || id==NATIVE_COUNT+EGINT){
        // ADSR shape uses the actual four filter-envelope settings.
        int a=clamp(extras[slot][FATT],0,127),d=clamp(extras[slot][FDEC],0,127);
        int sus=clamp(extras[slot][FSUS],0,127),r=clamp(extras[slot][FREL],0,127);
        int xa=x+7+(a>>3),xd=xa+8+(d>>3),xr=x+70-(r>>4),ys=y+33-(sus>>2);
        if(xd>xr-4)xd=xr-4;
        line_ui(c,x+6,y+34,xa,y+4,col);line_ui(c,xa,y+4,xd,ys,col);
        line_ui(c,xd,ys,xr,ys,col);line_ui(c,xr,ys,x+88,y+34,col);
        return;
    }
    if(id==MIXN){
        int prev=18;
        const int amp[9]={15,-9,12,-18,7,-13,17,-6,8};
        for(int i=0;i<9;++i){int yy=18+amp[i];line_ui(c,x+i*10,y+prev,x+(i+1)*10,y+yy,col);prev=yy;}
        return;
    }
    if(id==NATIVE_COUNT+DIST || id==NATIVE_COUNT+EGRESET){
        int on=extras[slot][id-NATIVE_COUNT];
        draw_wave(c,x,y,on?1:0,88,col);
        return;
    }
    // Other continuous parameters: readable parameter curve, not a fake waveform.
    int val=value_for(slot,id);
    int lo=(id<NATIVE_COUNT)?NMIN[id]:MINEX[id-NATIVE_COUNT];
    int hi=(id<NATIVE_COUNT)?NMAX[id]:MAXEX[id-NATIVE_COUNT];
    int steps=0,span=hi-lo;
    if(span>0){int n=clamp(val-lo,0,span)*16;
        for(int i=1;i<=8;++i)if(n>=(2*i-1)*span)++steps;}
    line_ui(c,x+5,y+33,x+14,y+33,col);
    line_ui(c,x+14,y+33,x+14+steps*9,y+5,col);
    line_ui(c,x+14+steps*9,y+5,x+88,y+5,col);
}
// Format only the displayed value. Control routing and stored values stay unchanged.
static const char* display_value(int id,int v,char* buf){
    if(id==NATIVE_COUNT+W1)return NW1[clamp(v,0,3)];
    if(id==NATIVE_COUNT+W2)return NW2[clamp(v,0,2)];
    if(id==NATIVE_COUNT+MOD2)return NMOD[clamp(v,0,3)];
    if(id==NATIVE_COUNT+FTYPE)return NFTYPE[clamp(v,0,4)];
    if(id==NATIVE_COUNT+DIST)return v?"ON":"OFF";
    int_text(buf,v);
    int lo=id<NATIVE_COUNT?NMIN[id]:MINEX[id-NATIVE_COUNT];
    int hi=id<NATIVE_COUNT?NMAX[id]:MAXEX[id-NATIVE_COUNT];
    if(lo<0 && hi>0 && v>0){
        char old[20];int n=0;
        while(buf[n] && n<18){old[n]=buf[n];++n;}
        old[n]=0;buf[0]='+';
        for(int i=0;i<=n;++i)buf[i+1]=old[i];
    }
    return buf;
}
static void draw_column(void* c,int page,int k,int x,unsigned slot,int shift){
    // 480x240: 14px outer margins, 10px gaps, and 20px bottom clearance.
    // The actual encoder controls (including SHIFT / push) are unchanged.
    const int w=106,h=204,y=14;
    int id=control(page,shift,k);
    const char* name=(shift && SHROT[page][k][0])?SHROT[page][k]:ROT[page][k];
    // The unassigned orange TURN on page 3 deliberately has no parameter display.
    // Grey non-SHIFT TURN is a momentary action, not a numeric setting.
    bool empty=(id<0),momentary=(id==NATIVE_COUNT+EGRESET);
    int lo=0,hi=127;
    if(id>=0 && id<NATIVE_COUNT){lo=NMIN[id];hi=NMAX[id];}
    else if(id>=NATIVE_COUNT){lo=MINEX[id-NATIVE_COUNT];hi=MAXEX[id-NATIVE_COUNT];}
    int v=empty?0:value_for(slot,id);
    op1f_paint_box(c,x,y,w,h,DARK[k]);
    rect_outline(c,x,y,w,h,COLOR[k]);
    // Big white parameter name: fits across one or two lines.
    if(!empty){
        // Explicit two-line titles: the previous three plus four SHIFT controls.
        bool force_two=(!shift && ((page==0 && k==2) || (page==1 && k==2) || (page==2 && k==2))) ||
                       (shift && ((page==0 && k==2) || (page==1 && (k==1 || k==2 || k==3))));
        column_label(c,x+w/2,y+11,name,WHITE,force_two);
    }
    // Momentary EG RESET has a distinct envelope-retrigger symbol, not a value or meter.
    if(momentary)reset_visual(c,x+9,y+70,COLOR[k]);
    if(!empty && !momentary){
    signal_visual(c,slot,id,x+9,y+70,COLOR[k]);
    // Compact meter, then a deliberately small numeric value BELOW the bar.
    const int bar_x=x+9,bar_y=y+148,bar_w=88,bar_h=8;
    op1f_paint_box(c,bar_x,bar_y,bar_w,bar_h,MUTED);
    int span=hi-lo,steps=0;
    if(lo<0 && hi>0){
        // Signed parameters: neutral center, negative fills left, positive right.
        // Integer comparisons avoid unsupported Blackfin runtime division.
        const int half=bar_w/2;
        int maximum=v<0?-lo:hi, magnitude=v<0?-v:v;
        if(maximum>0){for(int i=1;i<=half;++i)
            if(magnitude*half*2 >= (2*i-1)*maximum)++steps;}
        if(steps){
            int bx=v<0?bar_x+half-steps:bar_x+half;
            op1f_paint_box(c,bx,bar_y,steps,bar_h,COLOR[k]);
        }
        op1f_paint_box(c,bar_x+half-1,bar_y-2,2,bar_h+4,WHITE);
    }else{
        if(span>0){int n=clamp(v-lo,0,span)*44;
            for(int i=1;i<=22;++i)if(n>=(2*i-1)*span)++steps;}
        if(steps)op1f_paint_box(c,bar_x,bar_y,steps*4,bar_h,COLOR[k]);
    }
    char digits[20];const char* shown=display_value(id,v,digits);
    // Values use the same 2x font size as parameter headings.
    ui_center(c,x+w/2,y+166,shown,WHITE,2);
    }
    // No bottom labels: NEXT/PREV and RESET remain functional but invisible.
}
static void paint_help(void* c,int page){
    op1f_paint_box(c,12,15,456,209,BG);
    rect_outline(c,12,15,456,209,COLOR[0]);
    ui_center(c,240,25,"ANALOGBEAST  HELP",COLOR[0],2);
    ui_center(c,240,60,"TURN  PARAMETER",WHITE,2);
    ui_center(c,240,86,"SHIFT+TURN  ALT VALUE",WHITE,2);
    ui_center(c,240,112,"PRESS  CHANGE",WHITE,2);
    ui_center(c,240,138,"ORANGE+SHIFT PREV",WHITE,2);
    ui_center(c,240,170,"ORANGE  MENU",COLOR[3],2);
    ui_center(c,240,195,"HELP  CLOSE",WHITE,2);
    (void)page;
}
extern "C" void abfield_shown(unsigned slot,unsigned flags) OP1F_NAMED("abfield_shown");
extern "C" void abfield_shown(unsigned slot,unsigned flags){current_slot=slot;shown_flags=flags;}
extern "C" void abfield_frame(const Op1fFrame* now) OP1F_NAMED("abfield_frame");
extern "C" void abfield_frame(const Op1fFrame* now){
    if(now->bytes<FRAME_BYTES)return;
    if(!stood && op1f_page_stand_in(&op1f_card_analogbeast,ASK_ENGINE,0))stood=1;
    for(u32 s=0;s<SLOTS;++s){if(load_pending[s] || !known[s])load_extra(s);}
    unsigned slot=current_slot<SLOTS?current_slot:0;
    if(now->page==&op1f_card_analogbeast && !now->disk)events(slot);
    for(u32 s=0;s<SLOTS;++s){
        if(dirty[s] && !now->disk){char buf[192];extra_json(s,buf);
            if(op1f_value_set(&op1f_card_analogbeast,OP1F_KEEP_SYNTH,s,"abfield_extra",buf)==0)dirty[s]=0;
        }
    }
    if(now->page!=&op1f_card_analogbeast || !now->canvas || !(shown_flags&(u32)OP1F_SHOWN_UP))return;
    void* c=now->canvas;
    unsigned page=ui_page<3?ui_page:0;
    int shift=now->shift?1:0;
    op1f_paint_box(c,0,0,480,240,BG);
    // Four encoder columns. No top title, no bottom bar, no duplicate cards.
    for(int k=0;k<4;++k)draw_column(c,(int)page,k,14+k*116,slot,shift);
    if(help_open)paint_help(c,(int)page);
}
