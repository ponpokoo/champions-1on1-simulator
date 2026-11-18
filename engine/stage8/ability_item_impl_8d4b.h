// Stage 8d-4b. Item effects remain the admitted 33-item vocabulary.
// All callbacks below are collected through the 8d-0 event registry.
static int s8_items_blocked(const Mon *m) {
    if(!field_member(m)) return 0;
    Mon *foe=FIELD.seat[m==FIELD.seat[0]];
    return foe && foe->hp>0 && !s8_ability_event(foe,S8AE_FOE_TRY_EAT,m,m,NULL,1,0);
}
static int s8_items_try_eat(Mon *m) {
    if(s8_items_blocked(m)) return 0;
    return s8_ability_event(m,S8AE_TRY_EAT,m,m,NULL,1,s8_item_table(m));
}
static int s8_items_transfer(Mon *from,Mon *to,const MoveDef *move) {
    if(!from || !to || to->hp<=0 || to->item || !s8_item_takeable(from)) return 0;
    int item=from->item,table=s8_item_table(from);
    if(s8_stone_matches(to,table)) return 0;
    if(!s8_item_take(from,to,move)) return 0;
    s8_item_set(to,item,table);obs_emit(EV_TRANSFER,from,ITEM_NONE,1,0);return 1;
}
static void s8_items_callback(const Mon *holder,S8AbilityEvent *e) {
    Mon *m=(Mon *)holder;int id=s8_current_ability(m);
    if(e->event==S8AE_START) {
        if(id==S8I_GLUTTONY) m->ability_state.gluttony=1;
        if(id==S8I_PRESSURE) s8_ability_record(m,e->event,S8A_PUBLIC);
        if(id==S8I_UNNERVE && !m->ability_state.unnerved) {
            m->ability_state.unnerved=1;s8_ability_record(m,e->event,S8A_PUBLIC);
        }
        // Klutz's singleEvent End on the held item has no effect for the
        // admitted items (their only volatile, Choice lock, is suspended by hasItem).
    } else if(e->event==S8AE_END) {
        if(id==S8I_UNNERVE) m->ability_state.unnerved=0;
        if(id==S8I_UNBURDEN) m->unburden=0;
    } else if(e->event==S8AE_ITEM) {
        e->value=ITEM_NONE; // none of the admitted effect items has ignoreKlutz
    } else if(e->event==S8AE_WEIGHT) {
        e->value*=2;
    } else if(e->event==S8AE_SPEED) {
        if(m->unburden && !m->item) e->value=damage_chain(e->value,8192);
    } else if(e->event==S8AE_HP_DAMAGE || e->event==S8AE_BEFORE_DAMAGE) {
        m->ability_state.gluttony=1;
    } else if(e->event==S8AE_FOE_TRY_EAT) {
        if(m->ability_state.unnerved) e->value=0;
    } else if(e->event==S8AE_TRY_EAT) {
        if(e->value) s8_ability_record(m,e->event,S8A_PUBLIC);
    } else if(e->event==S8AE_EAT_ITEM) {
        if(id==S8I_CHEEKPOUCH) {
            int amount=m->maxhp/3;if(amount<1) amount=1;
            s8_heal_hp(m,amount);
        } else if(id==S8I_CUDCHEW && S8_ITEM_OPERATIONS[e->detail].berry &&
                  (!e->move || (strcmp(s8_power_move(e->move)->id,"bugbite") && strcmp(s8_power_move(e->move)->id,"pluck")))) {
            m->ability_state.berry=e->detail;m->ability_state.counter=2-FIELD.item_residual;
        } else if(id==S8I_RIPEN) {
            m->ability_state.berry_weaken=e->detail==ITEM_SHUCA || e->detail==ITEM_CHOPLE ||
                e->detail==ITEM_KASIB || e->detail==ITEM_PAYAPA;
        }
    } else if(e->event==S8AE_ITEM_HEAL) {
        if(S8_ITEM_OPERATIONS[e->detail].berry) e->value*=2;
        else if(e->detail==ITEM_LEFTOVERS) s8_ability_record(m,e->event,S8A_PUBLIC);
    } else if(e->event==S8AE_CHANGE_BOOST) {
        if(S8_ITEM_EFFECT && S8_ITEM_OPERATIONS[S8_ITEM_EFFECT].berry) e->value*=2;
    } else if(e->event==S8AE_DAMAGE) {
        // Estimates never consume the effectState. Actual numeric damage runs
        // with the berry bit, including a berry just eaten for this hit.
        if(m->ability_state.berry_weaken || (e->detail&2)) {
            if(S8_ITEM_DAMAGE_EXEC) m->ability_state.berry_weaken=0;
            e->value=damage_chain(e->value,2048);
        }
    } else if(e->event==S8AE_TAKE_ITEM) {
        if(id==S8I_UNBURDEN) m->unburden=1;
        if(id==S8I_STICKYHOLD && m->hp>0 &&
           ((e->source && e->source!=m) || (e->move && !strcmp(s8_power_move(e->move)->id,"knockoff")))) {
            e->value=0;s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    } else if(e->event==S8AE_AFTER_USE_ITEM) {
        if(id==S8I_UNBURDEN && e->source==m) m->unburden=1;
        if(id==S8I_SYMBIOSIS && e->source && e->source!=m && s8_resist_allies(m,e->source)) {
            if(s8_items_transfer(m,(Mon *)e->source,NULL)) s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    } else if(e->event==S8AE_DEDUCT_PP) {
        if(!s8_resist_allies(m,e->source)) e->value++;
    } else if(e->event==S8AE_AFTER_SECONDARY) {
        if(e->source && e->source!=m && e->move && s8_hit_contact(e->move,e->source) && m->hp>0 && !m->item)
            if(s8_items_transfer((Mon *)e->source,m,e->move)) s8_ability_record(m,e->event,S8A_PUBLIC);
    } else if(e->event==S8AE_AFTER_SECONDARY_SELF) {
        if(e->source==m && e->target && e->target!=m && e->move && e->move->cat!=CAT_STATUS &&
           strcmp(s8_power_move(e->move)->id,"fling") && e->value && !m->item && m->hp>0 &&
           !(S8_PROPERTY && S8_PROPERTY->sheer) && !CUR_FUTURE_IMPACT)
            if(s8_items_transfer((Mon *)e->target,m,e->move)) s8_ability_record(m,e->event,S8A_PUBLIC);
    } else if(e->event==S8AE_RESIDUAL) {
        if(id==S8I_HARVEST) {
            // The chance is drawn BEFORE checking HP, held item and lastItem.
            if(s8_field_weather(m)==WEATHER_SUN || (BATTLE_RNG && (int)(rng_dbl(BATTLE_RNG)*2)==0)) {
                if(m->hp>0 && !m->item && S8_ITEM_OPERATIONS[m->last_item].berry) {
                    int item=m->last_item,table=m->last_item_table;
                    s8_item_set(m,item,table);m->last_item=0;m->last_item_table=-1;
                    s8_ability_record(m,e->event,S8A_PUBLIC);
                }
            }
        } else if(id==S8I_CUDCHEW) {
            if(!m->ability_state.berry || m->hp<=0) return;
            if(--m->ability_state.counter<=0) {
                int item=m->ability_state.berry;
                s8_ability_record(m,e->event,S8A_PUBLIC);
                s8_item_eat_effect(m,item);
                s8_ability_event(m,S8AE_EAT_ITEM,NULL,m,NULL,1,item);m->ate_berry=1;
                m->ability_state.berry=m->ability_state.counter=0;
            }
        } else if(id==S8I_PICKUP && !m->item) {
            Mon *targets[2];int n=0;
            for(int i=0;i<2;i++) {
                Mon *other=FIELD.seat[i];
                if(other && other!=m && other->hp>0 && other->last_item && other->used_item_this_turn) targets[n++]=other;
            }
            if(!n) return;
            // sample() draws even for a single eligible adjacent opponent.
            Mon *other=targets[BATTLE_RNG?(int)(rng_dbl(BATTLE_RNG)*n):0];
            int item=other->last_item,table=other->last_item_table;
            other->last_item=0;other->last_item_table=-1;s8_item_set(m,item,table);
            s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    }
}
static void s8_items_pressure(Mon *me,Mon *foe,int mv) {
    if(CUR_FUTURE_IMPACT || S8_BOUNCED || !S8_ITEM_PRESSURE_TARGET[mv] || foe->hp<=0) return;
    int root=S8_HIT_CONTEXT?S8_HIT_CONTEXT->used_move-1:mv;
    int slot=find_move(me,root);
    if(slot<0) return; // Struggle and calls without a PP-bearing caller
    int extra=s8_ability_event(foe,S8AE_DEDUCT_PP,me,foe,move_def(mv),0,0);
    if(extra>0) { me->pp[slot]=me->pp[slot]>extra?me->pp[slot]-extra:0;leppa_check(me,slot); }
}
static void s8_items_residual(Mon *a,Mon *b,int order) {
    Mon *ms[]={a,b};S8AbilityCall calls[12];int n=0;
    const S8AbilityHandler uproar={-1,S8AE_RESIDUAL,28,0,1,s8_weather_uproar};
    for(int k=0;k<2;k++) if(ms[k]->hp>0) {
        if(ms[k]->uproar) calls[n++]=(S8AbilityCall){ms[k],&uproar,FIELD.ability_residual_speed[k],0};
        for(unsigned i=0;i<sizeof(S8_ABILITY_HANDLERS)/sizeof(*S8_ABILITY_HANDLERS);i++) {
            const S8AbilityHandler *row=&S8_ABILITY_HANDLERS[i];
            if(row->event==S8AE_RESIDUAL && row->order==order &&
               (s8_items_implemented(row->ability) || s8_weather_implemented(row->ability)) && s8_has_ability(ms[k],row->ability))
                calls[n++]=(S8AbilityCall){ms[k],row,FIELD.ability_residual_speed[k],0};
        }
    }
    S8AbilityEvent e={S8AE_RESIDUAL,NULL,NULL,NULL,1,0};s8_ability_run_queue(calls,n,&e,BATTLE_RNG);
}
