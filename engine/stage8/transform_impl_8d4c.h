// All eligibility guards share this predicate, including identities absent from
// the admitted species closure (tested through this same predicate).
static int s8_transform_eligible(const Mon *m,const Mon *target,const char *species,const char *family) {
    if(m->hp<=0 || target->hp<=0 || m->illusion || target->illusion || target->sub_hp ||
       m->transformed || target->transformed || !strcmp(species,"eternatuseternamax") ||
       ((!strcmp(family,"ogerpon") || !strcmp(family,"terapagos")) && (m->terastallized || target->terastallized)) ||
       m->terastallized==19) return 0;
    return 1;
}
// One transformInto core for the move and SwitchIn-only Imposter.
static int s8_transform_into_effect(Mon *m,const Mon *target,int imposter) {
    int species=target->spec_v3?target->table_species_id:target->mega?S8_METAGROSS_MEGA:S8_METAGROSS;
    const char *family=s8_form_family(target,0);
    if(!s8_transform_eligible(m,target,S8_SPECIES[species].id,family)) return 0;
    if(imposter) s8_ability_record(m,S8AE_SWITCH_IN,S8A_PUBLIC);
    m->table_species_id=species;m->transformed=1;
    m->weight_hg=target->weight_hg;m->type1=target->type1;m->type2=target->type2;m->added_type=target->added_type;
    // Roost keeps permanent types in type1/type2; Burn Up/Double Shock and
    // added types already live in the current type fields. Never copy Roost.
    memcpy(m->stats+1,target->stats+1,5*sizeof(int));
    memcpy(m->st,target->st,sizeof(m->st));m->acc=target->acc;m->evasion=target->evasion;
    m->nmoves=target->nmoves;
    for(int i=0;i<m->nmoves;i++) {
        m->moves[i]=target->moves[i];m->table_moves[i]=target->moves[i];
        int pp=S8_MOVES[m->moves[i]].base_pp;m->pp[i]=pp<5?pp:5;
    }
    m->used_mask=0;m->times_attacked=target->times_attacked;
    m->control[S8C_FOCUSENERGY]=target->dragon_cheer?0:target->control[S8C_FOCUSENERGY];
    m->dragon_cheer=target->dragon_cheer;m->gmax_chi_layers=target->gmax_chi_layers;m->laser_focus=target->laser_focus?2:0;
    m->boost_stats_active=1;m->state_features|=MON_POST_V1_STATE;
    // A public transform reveals its target, while copied unused moves are
    // private knowledge of the transforming owner. It reveals no private build.
    s8_reveal_event(S8RK_TRANSFORM,m,target,species,imposter,0);
    s8_transform_knowledge(m);
    int old=s8_current_ability(m),next=s8_current_ability(target);
    s8_ability_end(m);s8_assign_ability(m,next);
    if(old!=next) s8_ability_start(m);
    return 1;
}
static int s8_transform_into(Mon *m,const Mon *target) {
    return s8_transform_into_effect(m,target,0);
}
