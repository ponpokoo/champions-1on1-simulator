#define OBSH4_DIAG_USAGE \
    "  engine diagnose-obsh4 <specA> <specB> <games> <seed> <A|B> [turn]\n" \
    "  engine compare-obsh4 <specA> <specB> <games> <seed> <A|B> [stat-drop|substitute|setup|setup-tempo|opening]\n"

static int obsh4_parse_number(const char *arg,uint64_t *value) {
    char *end;
    if(*arg<'0' || *arg>'9') return 0;
    errno=0;
    *value=strtoull(arg,&end,10);
    return !errno && !*end;
}

static const char *obsh4_action_name(const Observation3 *o,int slot) {
    return slot<0 || slot==4?"struggle":S8_MOVES[o->v[H4_SELF_SLOTS+4*slot+1]].id;
}

static const char *obsh4_outcome(int result,int seat) {
    return result==2?"draw":result==seat?"win":"loss";
}

static void obsh4_print_branch(long game,int turn,int seat,const Obsh4Turn *normal,
                              int slot,const Obsh4Turn *branch,int normal_result,int branch_result,
                              int normal_turns,int branch_turns) {
    const Observation3 *o=&normal->observation[seat];
    const ActionMask *mask=&normal->masks[seat];
    printf("%ld,%d,",game+1,turn);
    const char *separator="";
    for(int i=0;i<5;i++) if(mask->legal[i]) {
        printf("%s%s",separator,obsh4_action_name(o,i));
        separator="|";
    }
    const char *choice=obsh4_action_name(o,normal->choice[seat]);
    const char *alternative=slot<0?"":obsh4_action_name(o,slot);
    printf(",%s,%s,%s,%s,%s,%s,%d,%d\n",choice,alternative,
           normal->attempted<0?"":S8_MOVES[normal->attempted].id,
           branch->attempted<0?"":S8_MOVES[branch->attempted].id,
           obsh4_outcome(normal_result,seat),obsh4_outcome(branch_result,seat),
           normal_turns,branch_turns);
}

static int obsh4_diagnostic_cli(int argc,char **argv) {
    int compare=!strcmp(argv[1],"compare-obsh4");
    uint64_t games,seed,turn=0;
    int candidate=OBSH4_STAT_DROP, baseline=0;
    if(compare && argc==8) {
        if(!strcmp(argv[7],"substitute")) candidate=OBSH4_SUBSTITUTE;
        else if(!strcmp(argv[7],"opening")) {
            baseline=OBSH4_SETUP_BASELINE;
            candidate=baseline|OBSH4_OPENING;
        }
        else if(!strcmp(argv[7],"setup") || !strcmp(argv[7],"setup-tempo")) {
            baseline=OBSH4_SETUP_BASELINE;
            candidate=baseline|OBSH4_SETUP_RACE;
            if(!strcmp(argv[7],"setup-tempo")) candidate|=OBSH4_SETUP_TEMPO;
        }
        else if(strcmp(argv[7],"stat-drop")) candidate=0;
    }
    if(argc<7 || argc>8 || !candidate ||
       !obsh4_parse_number(argv[4],&games) || !games || games>LONG_MAX ||
       !obsh4_parse_number(argv[5],&seed) ||
       (strcmp(argv[6],"A") && strcmp(argv[6],"B")) ||
       (!compare && argc==8 && (!obsh4_parse_number(argv[7],&turn) || turn<1 || turn>OBSH4_DIAG_TURNS))) {
        fputs(OBSH4_DIAG_USAGE,stderr);
        return 2;
    }
    int seat=argv[6][0]=='B';
    Mon A,B;
    init_mon(&A,argv[2]);
    init_mon(&B,argv[3]);
    if(!A.spec_v3 || !B.spec_v3 || A.policy!=POL_OBSH4 || B.policy!=POL_OBSH4 ||
       A.mega_mode || B.mega_mode || A.table_base_id!=B.table_base_id || getenv("ENGINE_SEQ")) {
        fputs("obsh4 diagnostics require a simultaneous v3 mirror with two obsh4 builds and mega=0\n",stderr);
        return 2;
    }

    puts(compare?"game,baseline_result,candidate_result,baseline_turns,candidate_turns":
         "game,turn,legal,selected,alternative,baseline_attempt,alternate_attempt,"
         "baseline_result,alternate_result,baseline_turns,alternate_turns");
    for(long g=0;g<(long)games;g++) {
        Obsh4Turn normal[OBSH4_DIAG_TURNS],branch[OBSH4_DIAG_TURNS];
        Obsh4Audit audit={.seat=seat,.candidate=compare?baseline:-1,.baseline=baseline,.turn=(int)turn,
                         .record=compare?NULL:normal};
        init_mon(&A,argv[2]);
        init_mon(&B,argv[3]);
        uint64_t start=matchup_seed(seed,g);
        Rng rng={start};
        int normal_turns,branch_turns;
        int normal_result=battle_with_audit(&A,&B,&rng,&normal_turns,&audit);
        if(compare || g==0) {
            init_mon(&A,argv[2]);
            init_mon(&B,argv[3]);
            rng.s=start;
            if(compare) {
                audit.candidate=candidate;
                int result=battle_with_audit(&A,&B,&rng,&branch_turns,&audit);
                printf("%ld,%s,%s,%d,%d\n",g+1,obsh4_outcome(normal_result,seat),
                       obsh4_outcome(result,seat),normal_turns,branch_turns);
            } else {
                int result=battle(&A,&B,&rng,&branch_turns);
                if(result!=normal_result || branch_turns!=normal_turns) {
                    fputs("obsh4 diagnostic recording changed the outcome or length\n",stderr);
                    return 2;
                }
            }
        }
        if(compare) continue;

        int first=turn?(int)turn:1, last=turn?(int)turn:OBSH4_DIAG_TURNS;
        for(int t=first;t<=last && t<=normal_turns;t++) {
            Obsh4Turn *original=&normal[t-1];
            obsh4_print_branch(g,t,seat,original,-1,original,normal_result,normal_result,
                               normal_turns,normal_turns);
            for(int slot=0;slot<5;slot++) {
                if(!original->masks[seat].legal[slot] ||
                   slot==(original->choice[seat]<0?4:original->choice[seat])) continue;
                init_mon(&A,argv[2]);
                init_mon(&B,argv[3]);
                rng.s=start;
                audit.turn=t;
                audit.slot=slot;
                audit.record=branch;
                audit.expected=original;
                branch[t-1].attempted=-2;
                int result=battle_with_audit(&A,&B,&rng,&branch_turns,&audit);
                if(branch[t-1].attempted==-2) {
                    fprintf(stderr,"obsh4 diagnostic replay did not reach turn %d\n",t);
                    return 2;
                }
                obsh4_print_branch(g,t,seat,original,slot,&branch[t-1],normal_result,result,
                                   normal_turns,branch_turns);
            }
        }
    }
    return ferror(stdout)?2:0;
}
