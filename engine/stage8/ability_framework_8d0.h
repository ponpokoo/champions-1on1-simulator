// Stage 8d-0. Ownership, activation state and Pokemon lifetime are separate.
// No new abilities are admitted by this registry. Queries never consume RNG.
typedef enum {
    S8AE_SWITCH_IN, S8AE_START, S8AE_END, S8AE_MODIFY_MOVE,
    S8AE_PRIORITY, S8AE_SPEED, S8AE_TARGET, S8AE_ACCURACY,
    S8AE_BEFORE_HIT, S8AE_AFTER_HIT, S8AE_AFTER_MOVE, S8AE_RESIDUAL,
    S8AE_TRY_BOOST, S8AE_WEIGHT, S8AE_BASE_POWER,
    S8AE_STATUS, S8AE_ITEM, S8AE_GROUNDED, S8AE_WEATHER, S8AE_TYPE,
    S8AE_REVEAL, S8AE_SUPPRESS, S8AE_RESTORE
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
static int damage_chain(int modifier,int next);
static double eff_speed(const Mon *m);
static void s8_ability_record(const Mon *m,S8AbilityEventId event,int visibility) {
    int identity=event==S8AE_SUPPRESS && visibility==S8A_PUBLIC?-1:s8_current_ability(m);
    // Internal lifecycle notifications are available to fixtures/debug sinks.
    // Only explicit battle messages reveal an ability. Passive power/weight
    // queries and Start of these three passive abilities reveal nothing.
#ifdef ENGINE_ABILITY_EVENT_SINK
    ENGINE_ABILITY_EVENT_SINK(event,m,identity,visibility);
#endif
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
static const S8AbilityHandler S8_ABILITY_HANDLERS[]={
    {S8TA_AB_CLEARBODY,S8AE_TRY_BOOST,0,0,0,s8_ability_clearbody},
    {S8TA_AB_LIGHTMETAL,S8AE_WEIGHT,0,0,0,s8_ability_lightmetal},
    {S8TA_AB_TOUGHCLAWS,S8AE_BASE_POWER,0,21,0,s8_ability_toughclaws},
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
        chosen.handler->callback(chosen.holder,e);
    }
    return e->value;
}
static int s8_ability_dispatch_rows(const Mon *holder,S8AbilityEvent *e,
                                   const S8AbilityHandler *rows,int count) {
    S8AbilityCall calls[count>0?count:1];int used=0;
    for(int i=0;i<count;i++) if(rows[i].event==e->event && s8_has_ability(holder,rows[i].ability))
        calls[used++]=(S8AbilityCall){holder,&rows[i],0,0};
    return s8_ability_run_queue(calls,used,e,NULL);
}
static int s8_ability_event(const Mon *holder,S8AbilityEventId event,const Mon *source,
                           const Mon *target,const MoveDef *move,int value,int detail) {
    S8AbilityEvent e={event,source,target,move,value,detail};
#ifdef ENGINE_ABILITY_DISPATCH_SINK
    ENGINE_ABILITY_DISPATCH_SINK(holder,event,detail);
#endif
    return s8_ability_dispatch_rows(holder,&e,S8_ABILITY_HANDLERS,
        (int)(sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS)));
}
static int s8_ability_boost(const Mon *target,const Mon *source,int delta) {
    return s8_ability_event(target,S8AE_TRY_BOOST,source,target,NULL,delta,0);
}
static void s8_ability_phase(const Mon *source,const Mon *target,S8AbilityEventId event,int mv,int detail) {
    const MoveDef *move=mv>=0?move_def(mv):NULL;
    s8_ability_event(source,event,source,target,move,1,detail);
    if(target && target!=source) s8_ability_event(target,event,source,target,move,1,detail);
}
static void s8_ability_new_state(Mon *m) {
    unsigned generation=m->ability_state.generation+1;
    m->ability_state=(S8AbilityEffectState){.id=s8_current_ability(m),.generation=generation};
}
static void s8_ability_start(Mon *m) {
    int id=s8_current_ability(m);
    if(id<0 || m->hp<=0 || !s8_has_ability(m,id)) return;
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
    int first=eff_speed(b)>eff_speed(a);
    for(int i=0;i<2;i++) {
        Mon *m=ms[first^i];s8_ability_new_state(m);
        s8_ability_record(m,S8AE_SWITCH_IN,S8A_INTERNAL);
        s8_ability_event(m,S8AE_SWITCH_IN,m,m,NULL,1,0);
        s8_ability_start(m);
    }
}
