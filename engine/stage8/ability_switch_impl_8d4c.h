// Public mechanics messages and private owner knowledge; no MGO schema change.
enum { S8RK_MOVE, S8RK_ITEM, S8RK_TRANSFORM, S8RK_CURE, S8RK_HEAL, S8RK_CLEAR };
enum { S8_SWITCH_TRAPPED_IMMUNITY=200 };
typedef struct {
    unsigned char moves[S8_MOVES_COUNT];
    int item_known, item;
} S8RevealKnowledge;
static S8RevealKnowledge S8_REVEAL_KNOWLEDGE[2][2];
static void s8_switch_reset(void) { memset(S8_REVEAL_KNOWLEDGE,0,sizeof(S8_REVEAL_KNOWLEDGE)); }
static void s8_reveal_event(int kind,const Mon *holder,const Mon *target,int value,int detail,int silent) {
#ifdef ENGINE_REVEAL_EVENT_SINK
    ENGINE_REVEAL_EVENT_SINK(kind,holder,target,value,detail,silent);
#endif
    if(S8_ABILITY_TRACE) {
        if(kind==S8RK_TRANSFORM) printf("-transform|%c|%c%s\n",holder==FIELD.seat[1]?'B':'A',target==FIELD.seat[1]?'B':'A',detail?"|[from] ability: Imposter":"");
        else printf("reveal public %d %c %c %d %d %d\n",kind,holder==FIELD.seat[1]?'B':'A',target==FIELD.seat[1]?'B':'A',value,detail,silent);
    }
    if(field_member(target)) {
        int seat=target==FIELD.seat[1];
        for(int v=0;v<2;v++) {
            S8RevealKnowledge *k=&S8_REVEAL_KNOWLEDGE[v][seat];
            if(kind==S8RK_MOVE && value>=0 && value<S8_MOVES_COUNT) k->moves[value]=1;
            if(kind==S8RK_ITEM) { k->item_known=1;k->item=value; }
        }
    }
    (void)kind;(void)holder;(void)target;(void)value;(void)detail;(void)silent;
}
static void s8_transform_knowledge(const Mon *m) {
    if(!field_member(m)) return;
    int seat=m==FIELD.seat[1];
    // Retain historical reveals; replacement slots themselves are known only
    // to the owner. There is no inference about the target's other build data.
    for(int i=0;i<m->nmoves;i++) S8_REVEAL_KNOWLEDGE[seat][seat].moves[m->moves[i]]=1;
}
static const char *s8_switch_species(const Mon *m) {
    const char *id=S8_SPECIES[m->table_species_id].id;
#ifdef ENGINE_SWITCH_SPECIES_ID
    return ENGINE_SWITCH_SPECIES_ID(m,id);
#else
    return id;
#endif
}
static int s8_can_switch(const Mon *m) {
    int side=s8_resist_side(m);if(side<0) return 0;
    for(int i=0;i<FIELD.party_count[side];i++) {
        const Mon *p=FIELD.party[side][i];
        if(p && p!=FIELD.seat[side] && p->hp>0) return 1;
    }
    return 0;
}
static int s8_side_left(int side) {
    int count=0;
    for(int i=0;i<FIELD.party_count[side];i++) if(FIELD.party[side][i] && FIELD.party[side][i]->hp>0) count++;
    return count;
}
static void s8_after_faint(void) {
    int length=FIELD.faint_count-FIELD.faint_processed;
    if(length<=0) return;
    FaintEntry last=FIELD.faint_queue[FIELD.faint_count-1];FIELD.faint_processed=FIELD.faint_count;
    // checkWin precedes AfterFaint. The admitted one-member final KO never
    // grants Moxie/Battle Bond, including recoil/double-faint endings.
    if(!s8_side_left(0) || !s8_side_left(1) || !last.source) return;
    s8_ability_event(last.source,S8AE_SOURCE_AFTER_FAINT,last.source,FIELD.seat[last.seat],
        last.move,length,0);
}
static void s8_switch_traps(Mon *m) {
    if(!field_member(m)) return;
    Mon *foe=FIELD.seat[1-(m==FIELD.seat[1])];
    m->switch_trapped=!!(m->trapped || m->partial_trap || m->ingrain || m->no_retreat);m->maybe_trapped=0;
    if(foe && foe->hp>0) {
        s8_ability_event(foe,S8AE_FOE_TRAP,foe,m,NULL,1,0);
        s8_ability_event(foe,S8AE_FOE_MAYBE_TRAP,foe,m,NULL,1,0);
    }
    // priority -10: self Run Away follows the opposing priority-0 trap.
    s8_ability_event(m,S8AE_TRAP,m,m,NULL,1,0);
    s8_ability_event(m,S8AE_MAYBE_TRAP,m,m,NULL,1,0);
}
static int s8_drag_out(Mon *source,Mon *target,const MoveDef *move) {
    if(source->hp<=0 || target->hp<=0 || !s8_can_switch(target)) return 0;
    if(!s8_ability_event(target,S8AE_DRAG_OUT,source,target,move,1,0)) return 0;
    target->force_switch_flag=1;return 1;
}
static void s8_switch_out(Mon *m) {
    s8_ability_event(m,S8AE_SWITCH_OUT,m,m,NULL,1,0);s8_ability_end(m);
}
static void s8_process_switches(void) {
    for(int side=0;side<2;side++) {
        Mon *m=FIELD.seat[side];
        if(!m || (!m->switch_flag && !m->force_switch_flag) || !s8_can_switch(m)) continue;
        Mon *next=NULL;
        for(int i=0;i<FIELD.party_count[side];i++) if(FIELD.party[side][i] && FIELD.party[side][i]!=m && FIELD.party[side][i]->hp>0) { next=FIELD.party[side][i];break; }
        if(!next) continue;
        s8_switch_out(m);m->switch_flag=m->force_switch_flag=0;
        FIELD.seat[side]=next;s8_ability_new_state(next);
        s8_ability_event(next,S8AE_BEFORE_SWITCH_IN,next,next,NULL,1,0);
        s8_ability_event(next,S8AE_SWITCH_IN,next,next,NULL,1,0);
        if(!next->transformed) s8_ability_start(next);
    }
}
static void s8_switch_callback(const Mon *holder,S8AbilityEvent *e) {
    Mon *m=(Mon *)holder;int id=s8_current_ability(m);
    if(e->event==S8AE_START) {
        int side=s8_resist_side(m);
        if(id==S8SC_CURIOUSMEDICINE || id==S8SC_HOSPITALITY) {
            if(side<0) return;
            for(int i=0;i<FIELD.power_ally_count[side];i++) {
                Mon *ally=FIELD.power_allies[side][i];if(!ally || ally==m || ally->hp<=0) continue;
                if(id==S8SC_CURIOUSMEDICINE) {
                    memset(ally->st,0,sizeof(ally->st));ally->acc=ally->evasion=0;
                    s8_ability_record(m,e->event,S8A_PUBLIC);s8_reveal_event(S8RK_CLEAR,m,ally,0,0,0);
                } else {
                    int base=ally->stats[0]>0?ally->stats[0]:ally->maxhp;
                    if(s8_heal_hp(ally,base/4>0?base/4:1)) { // Battle.heal truncates, minimum one
                        s8_ability_record(m,e->event,S8A_PUBLIC);s8_reveal_event(S8RK_HEAL,m,ally,0,0,0);
                    }
                }
            }
            return;
        }
        if(!field_member(m)) return;
        Mon *foe=FIELD.seat[1-(m==FIELD.seat[1])];if(!foe || foe->hp<=0) return;
        if(id==S8SC_FRISK) {
            if(foe->item) { s8_ability_record(m,e->event,S8A_PUBLIC);s8_reveal_event(S8RK_ITEM,m,foe,foe->item,0,0); }
        } else if(id==S8SC_ANTICIPATION) {
            for(int i=0;i<foe->nmoves;i++) {
                int mv=foe->moves[i];const S8Move *move=&S8_MOVES[mv];
                if(move->category==CAT_STATUS) continue;
                double eff=1;
                for(int j=0;j<3;j++) eff*=eff_vs(move->type,s8_effective_type(m,j));
                if(eff>1 || S8_REVEAL_MOVES[mv].ohko) { s8_ability_record(m,e->event,S8A_PUBLIC);break; }
            }
        } else if(id==S8SC_FOREWARN) {
            int choices[5],n=0,best=1;
            for(int i=0;i<foe->nmoves;i++) {
                int mv=foe->moves[i],bp=S8_REVEAL_MOVES[mv].warn;
                if(bp>best) { n=0;best=bp; }
                if(bp==best) choices[n++]=mv;
            }
            if(n) {
                int selected=choices[s8_hit_random(n)];
                s8_ability_record(m,e->event,S8A_PUBLIC);s8_reveal_event(S8RK_MOVE,m,foe,selected,0,0);
            }
        }
    } else if(e->event==S8AE_SWITCH_OUT) {
        if(id==S8SC_NATURALCURE && m->hp>0 && (m->status || m->sleep)) {
            s8_ability_record(m,e->event,S8A_PUBLIC);s8_reveal_event(S8RK_CURE,m,m,m->status,m->sleep>0,1);clear_major_status(m);
        } else if(id==S8SC_REGENERATOR && m->hp>0) {
            int before=m->hp,base=m->stats[0]>0?m->stats[0]:m->maxhp;
            // Pokemon.heal floors and bypasses TryHeal/Heal Block.
            m->hp+=base/3;if(m->hp>m->maxhp) m->hp=m->maxhp;
            if(m->hp>before) { obs_hp(m,before,HP_HEAL);s8_ability_record(m,e->event,S8A_PUBLIC);s8_reveal_event(S8RK_HEAL,m,m,0,0,1); }
        }
    } else if(e->event==S8AE_EMERGENCY_EXIT) {
        if(m->hp>0 && 2*m->hp<=m->maxhp && 2*e->value>m->maxhp && s8_can_switch(m) && !m->switch_flag && !m->force_switch_flag) {
            m->switch_flag=1;s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    } else if(e->event==S8AE_TRAP) m->switch_trapped=0;
    else if(e->event==S8AE_MAYBE_TRAP) m->maybe_trapped=0;
    else if(e->event==S8AE_FOE_TRAP || e->event==S8AE_FOE_MAYBE_TRAP) {
        Mon *target=(Mon *)e->target;
        if(target && target!=m && !s8_resist_allies(target,m)) {
            if(!s8_has_ability(target,S8SC_SHADOWTAG)) {
                if(e->event==S8AE_FOE_MAYBE_TRAP) target->maybe_trapped=1;
                else if(!has_type(target,T_GHOST) && s8_ability_event(target,S8AE_STATUS_IMMUNITY,m,target,NULL,1,S8_SWITCH_TRAPPED_IMMUNITY)) target->switch_trapped=1;
            }
        }
    } else if(e->event==S8AE_DRAG_OUT) { s8_ability_record(m,e->event,S8A_PUBLIC);e->value=0; }
    else if(e->event==S8AE_SOURCE_AFTER_FAINT) {
        if(!e->move) return;
        if(id==S8SC_MOXIE) { int boost[7]={e->value,0,0,0,0,0,0};s8_change_boost(m,m,boost); }
        else if(!m->bond_triggered && m->hp>0 && !m->transformed &&
                !strcmp(s8_switch_species(m),"greninjabond") &&
                field_member(m) && s8_side_left(1-(m==FIELD.seat[1]))) {
            const int boost[7]={1,0,1,0,1,0,0};s8_change_boost(m,m,boost);
            m->bond_triggered=1;s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    }
}
