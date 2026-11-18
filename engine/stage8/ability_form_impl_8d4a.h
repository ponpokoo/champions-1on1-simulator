// Stage 8d-4a: form, transformation and ability ownership share the event layer.
static int s8_form_species(const char *id) {
    for(int i=0;i<S8_SPECIES_ROWS_COUNT;i++) if(!strcmp(S8_SPECIES[i].id,id)) return i;
    return -1;
}
static const char *s8_form_family(const Mon *m,int base) {
    int id=base?m->table_base_id:m->table_species_id;
    if(!m->spec_v3) id=m->mega?S8_METAGROSS_MEGA:S8_METAGROSS;
    return S8_SPECIES[S8_SPECIES[id].base_id].id;
}
static int s8_form_change(Mon *m,const char *id,int permanent) {
    int next=s8_form_species(id);
    if(m->transformed || next<0) return 0;
    int stats[6],ability=s8_current_ability(m),changed=m->ability_changed;
    s8_calculate_stats(m,next,stats);
    s8_set_form(m,next,ability);
    m->ability_changed=changed;
    memcpy(m->stats+1,stats+1,5*sizeof(int)); // setSpecies never replaces existing HP
    if(permanent) m->table_base_id=next;
    m->boost_stats_active=1;m->state_features|=MON_POST_V1_STATE;
    if(!permanent) s8_ability_record(m,S8AE_REVEAL,S8A_PUBLIC);
#ifdef ENGINE_FORM_EVENT_SINK
    ENGINE_FORM_EVENT_SINK(m,next,permanent);
#endif
    if(S8_ABILITY_TRACE) printf("form public %c %s\n",m==FIELD.seat[1]?'B':'A',S8_SPECIES[next].id);
    // Disguise preserves ability and effectState on permanent change.
    if(permanent && ability!=S8F_DISGUISE && m->hp>0) {
        int new_ability=S8_SPECIES_ABILITIES[S8_SPECIES[next].ability_offset];
        s8_ability_end(m);s8_assign_ability(m,new_ability);s8_ability_start(m);
    }
    return 1;
}
static int s8_swap_abilities(Mon *source,Mon *target) {
    if(source->hp<=0 || target->hp<=0 || source->dynamax || target->dynamax ||
       (s8_ability_flags(source)&S8TA_FAILSKILLSWAP) || (s8_ability_flags(target)&S8TA_FAILSKILLSWAP) ||
       s8_hit_has_item(source,"abilityshield") || s8_hit_has_item(target,"abilityshield")) return 0;
    int a=s8_current_ability(source),b=s8_current_ability(target);
    s8_ability_end(source);s8_ability_end(target);
    s8_assign_ability(source,b);s8_assign_ability(target,a);
    s8_ability_record(source,S8AE_REVEAL,S8A_PUBLIC);s8_ability_record(target,S8AE_REVEAL,S8A_PUBLIC);
    s8_ability_start(target);s8_ability_start(source);return 1;
}
static int s8_transform_into(Mon *m,const Mon *target) {
    int species=target->spec_v3?target->table_species_id:target->mega?S8_METAGROSS_MEGA:S8_METAGROSS;
    const char *family=s8_form_family(target,0);
    if(m->hp<=0 || target->hp<=0 || m->illusion || target->illusion || target->sub_hp ||
       m->transformed || target->transformed || !strcmp(S8_SPECIES[species].id,"eternatuseternamax") ||
       ((!strcmp(family,"ogerpon") || !strcmp(family,"terapagos")) && (m->terastallized || target->terastallized)) ||
       m->terastallized==19) return 0; // Stellar; tera has no admitted producer
    // The transform message reveals its initiating ability, never a passive
    // copied ability. setAbility(isTransform) has no public -ability message.
    if(s8_current_ability(m)==S8F_IMPOSTER) s8_ability_record(m,S8AE_SWITCH_IN,S8A_PUBLIC);
    m->table_species_id=species;m->transformed=1;
    m->weight_hg=target->weight_hg;m->type1=target->type1;m->type2=target->type2;m->added_type=target->added_type;
    memcpy(m->stats+1,target->stats+1,5*sizeof(int));
    memcpy(m->st,target->st,sizeof(m->st));m->acc=target->acc;m->evasion=target->evasion;
    m->nmoves=target->nmoves;
    for(int i=0;i<m->nmoves;i++) {
        m->moves[i]=target->moves[i];m->table_moves[i]=target->moves[i];
        int pp=move_def(m->moves[i])->pp;m->pp[i]=pp<5?pp:5;
    }
    m->used_mask=0; // the copied, unused slots are known to their owner
    m->times_attacked=target->times_attacked;
    m->control[S8C_FOCUSENERGY]=target->control[S8C_FOCUSENERGY];
    m->boost_stats_active=1;m->state_features|=MON_POST_V1_STATE;
    int old=s8_current_ability(m),next=s8_current_ability(target);
    // Transform bypasses cantsuppress and SetAbility, and does not restart a
    // same-named ability. notransform suppression still applies to the copy.
    s8_ability_end(m);s8_assign_ability(m,next);
    if(old!=next) s8_ability_start(m);
    if(S8_ABILITY_TRACE) printf("transform public %c %s\n",m==FIELD.seat[1]?'B':'A',S8_SPECIES[species].id);
    return 1;
}
static int s8_form_move_index(const MoveDef *move) {
    for(int i=0;i<S8_MOVES_COUNT;i++) if(move && !strcmp(move->code,S8_MOVES[i].code)) return i;
    return -1;
}
static int s8_form_body(const Mon *m,const MoveDef *move) {
    if(!move || m->transformed || !m->spec_v3 || strcmp(S8_SPECIES[m->table_species_id].id,"mimikyu")) return 0;
    int mv=s8_form_move_index(move);
    return mv>=0 && !(s8_sub_blocks(m,mv) && !(S8_MOVES[mv].flags&S8_FLAG_BYPASSSUB) && !s8_property_sound(mv));
}
static void s8_form_faint(Mon *m) {
    s8_ability_event(m,S8AE_FAINT,m,m,NULL,1,0);
    int side=s8_resist_side(m);
    if(side<0) return;
    Mon *allies[6];int n=0;
    if(FIELD.seat[side]!=m) allies[n++]=FIELD.seat[side];
    for(int i=0;i<FIELD.power_ally_count[side];i++) if(FIELD.power_allies[side][i]!=m) allies[n++]=FIELD.power_allies[side][i];
    for(int i=0;i<n;i++) s8_ability_event(allies[i],S8AE_ALLY_FAINT,m,m,NULL,1,0);
}
static void s8_form_callback(const Mon *holder,S8AbilityEvent *e) {
    Mon *m=(Mon *)holder;int id=s8_current_ability(m);
    if(id==S8F_DISGUISE) {
        if(e->event==S8AE_UPDATE) {
            if((m->ability_state.flags&1) && !strcmp(S8_SPECIES[m->table_species_id].id,"mimikyu") &&
               s8_form_change(m,"mimikyubusted",1)) s8_hit_retaliate(m,m->maxhp/8>0?m->maxhp/8:1);
        } else if(e->event==S8AE_HP_DAMAGE && s8_form_body(m,e->move)) {
            m->ability_state.flags|=1;e->value=0;s8_ability_record(m,e->event,S8A_PUBLIC);
        } else if(s8_form_body(m,e->move)) {
            // runImmunity: the effectiveness query keeps true zero immunity.
            int mv=s8_form_move_index(e->move);
            if(e->event==S8AE_EFFECTIVENESS) e->value=4096;
            else if(damage_effectiveness(m,mv)>0) e->value=0;
        }
    } else if(id==S8F_FORECAST) {
        if(!strcmp(s8_form_family(m,1),"castform") && !m->transformed && field_member(m)) {
            int w=current_weather(m);
            const char *form=w==WEATHER_SUN?"castformsunny":w==WEATHER_RAIN?"castformrainy":
                (w==WEATHER_SNOW || w==WEATHER_HAIL)?"castformsnowy":"castform";
            if(strcmp(S8_SPECIES[m->table_species_id].id,form)) s8_form_change(m,form,0);
        }
    } else if(id==S8F_HUNGERSWITCH) {
        if(!strcmp(s8_form_family(m,0),"morpeko") && !m->terastallized)
            s8_form_change(m,!strcmp(S8_SPECIES[m->table_species_id].id,"morpeko")?"morpekohangry":"morpeko",0);
    } else if(id==S8F_ILLUSION) {
        if(e->event==S8AE_BEFORE_SWITCH_IN) {
            m->illusion=NULL;int side=s8_resist_side(m),position=-1;
            if(side>=0) {
                for(int i=0;i<FIELD.party_count[side];i++) if(FIELD.party[side][i]==m) position=i;
                for(int i=FIELD.party_count[side]-1;i>position && position>=0;i--) {
                    const Mon *p=FIELD.party[side][i];if(!p || p->hp<=0) continue;
                    const char *family=s8_form_family(p,0);
                    if(!m->terastallized || (strcmp(family,"ogerpon") && strcmp(family,"terapagos"))) m->illusion=p;
                    break;
                }
            }
        } else if(e->event==S8AE_FAINT) m->illusion=NULL;
        else if(m->illusion && !m->being_called_back) {
            m->illusion=NULL;s8_ability_record(m,S8AE_END,S8A_PUBLIC);
        }
    } else if(id==S8F_IMPOSTER) {
        if(field_member(m)) s8_transform_into(m,FIELD.seat[1-(m==FIELD.seat[1])]);
    } else if(id==S8F_LIBERO || id==S8F_PROTEAN) {
        if(!e->move || (m->ability_state.flags&1) || S8_BOUNCED || CUR_FUTURE_IMPACT) return;
        int mv=s8_form_move_index(e->move);if(mv<0) return;
        const S8Move *move=&S8_MOVES[mv];
        if((move->flags&S8_FLAG_FUTUREMOVE) || S8_FORM_CALLS_MOVE[mv]) return;
        int type=e->move->type;
        if(type>=0 && !(s8_effective_type(m,0)==type && s8_effective_type(m,1)<0 && !m->added_type) && s8_set_types(m,type,-1,0)) {
            m->ability_state.flags|=1;s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    } else if(id==S8F_MIMICRY) {
        int terrain=field_member(m)?FIELD.terrain:TERRAIN_NONE;
        const S8Species *base=&S8_SPECIES[m->table_base_id];
        int first=terrain==TERRAIN_ELECTRIC?T_ELEC:terrain==TERRAIN_GRASSY?T_GRASS:terrain==TERRAIN_MISTY?T_FAIRY:terrain==TERRAIN_PSYCHIC?T_PSY:base->types[0];
        int mapped=terrain==TERRAIN_ELECTRIC || terrain==TERRAIN_GRASSY || terrain==TERRAIN_MISTY || terrain==TERRAIN_PSYCHIC;
        int second=mapped?-1:base->types[1];
        if((s8_effective_type(m,0)!=first || s8_effective_type(m,1)!=second || m->added_type) && s8_set_types(m,first,second,0))
            s8_ability_record(m,e->event,S8A_PUBLIC);
    } else if(id==S8F_MUMMY) {
        if(e->source && !(s8_ability_flags(e->source)&S8TA_CANTSUPPRESS) && s8_current_ability(e->source)!=S8F_MUMMY && s8_hit_contact(e->move,e->source))
            s8_set_ability((Mon *)e->source,S8F_MUMMY);
    } else if(id==S8F_WANDERINGSPIRIT) {
        if(e->source && s8_hit_contact(e->move,e->source)) s8_swap_abilities((Mon *)e->source,m);
    } else if(id==S8F_RECEIVER) {
        int next=e->target?s8_current_ability(e->target):-1;
        if(m->hp>0 && next>=0 && !(S8_FORM_FLAGS[next]&S8F_NORECEIVER)) s8_set_ability(m,next);
    } else if(id==S8F_TRACE) {
        const Mon *target=field_member(m)?FIELD.seat[1-(m==FIELD.seat[1])]:NULL;
        int next=target?s8_current_ability(target):-1;
        if(e->event==S8AE_START) m->ability_state.flags=(target && next<0) || s8_hit_has_item(m,"abilityshield")?0:1;
        if((m->ability_state.flags&1) && target && target->hp>0 && next>=0 && !(S8_FORM_FLAGS[next]&S8F_NOTRACE)) {
            (void)s8_hit_random(1); // sample the single adjacent foe, including its RNG draw
            s8_set_ability(m,next);
        }
    } else if(id==S8F_STALWART) {
        if(S8_PROPERTY && e->move) S8_PROPERTY->tracks_target=strcmp(s8_power_move(e->move)->target,"scripted")!=0;
    } else if(id==S8F_STANCECHANGE) {
        if(e->detail==1 && S8_PROPERTY && e->move && !m->transformed && !strcmp(s8_form_family(m,0),"aegislash")) {
            int shield=!strcmp(S8_MOVES[s8_form_move_index(e->move)].id,"kingsshield");
            if(e->move->cat!=CAT_STATUS || shield) {
                const char *form=shield?"aegislash":"aegislashblade";
                if(strcmp(S8_SPECIES[m->table_species_id].id,form)) s8_form_change(m,form,0);
            }
        }
    } else if(id==S8F_ZEROTOHERO && !strcmp(s8_form_family(m,1),"palafin")) {
        if(e->event==S8AE_SWITCH_OUT && strcmp(S8_SPECIES[m->table_species_id].id,"palafinhero")) {
            if(s8_form_change(m,"palafinhero",1)) m->hero_message_displayed=0;
        } else if(e->event==S8AE_SWITCH_IN && !m->hero_message_displayed && !strcmp(S8_SPECIES[m->table_species_id].id,"palafinhero")) {
            m->hero_message_displayed=1;s8_ability_record(m,e->event,S8A_PUBLIC);
        }
    }
}
