/* Read-only access to the generated tables and the engine's admission rules. */
#define main engine_main
#include "../engine/engine.c"
#undef main

static void print_stats(const S8Species *form) {
    for (int i = 0; i < 6; ++i) printf("\t%d", form->base_stats[i]);
}

static void profiles(int normal_id, char sex) {
    const S8Species *normal = &S8_SPECIES[normal_id];
    const int nonstone_count = S8_NON_STONE_ITEMS_COUNT;
    for (int i = 0; i < normal->move_count; ++i) {
        int move = S8_LEARNSETS[normal->move_offset + i];
        if (move != S8_STRUGGLE && move_executable(move))
            printf("move\t%c\t%s\n", sex, S8_MOVES[move].id);
    }

    for (int i = -1; i < nonstone_count + normal->mega_count; ++i) {
        int stone = i >= nonstone_count;
        const S8MegaLink *link = stone ? &S8_MEGA_LINKS[normal->mega_offset + i - nonstone_count] : NULL;
        int item = stone ? link->stone : i;
        if (s8_item_projection(item) < 0) continue;
        const S8Species *target = stone ? &S8_SPECIES[link->target] : NULL;
        for (int mode = 0; mode <= (stone ? 2 : 0); ++mode) {
            if (mode == 2 && s8_ability_projection(S8_SPECIES_ABILITIES[target->ability_offset]) < 0)
                continue;
            const S8Species *start = mode == 1 ? target : normal;
            for (int j = 0; j < start->ability_count; ++j) {
                int ability = S8_SPECIES_ABILITIES[start->ability_offset + j];
                if (s8_ability_projection(ability) < 0) continue;
                printf("profile\t%c\t%s\t%d\t%s", sex, item < 0 ? "none" : S8_ITEMS[item].id,
                       mode, S8_ABILITIES[ability].id);
                print_stats(start);
                if (mode == 2) print_stats(target);
                /* A discarded point must also be wasted in reachable battle forms. */
                for (int k = 0; k < S8_SPECIES_ROWS_COUNT; ++k) {
                    const S8Species *form = &S8_SPECIES[k];
                    if (form->base_id == normal->base_id && form->form_kind == S8_FORM_BATTLE &&
                        form->reachable_1on1)
                        print_stats(form);
                }
                putchar('\n');
            }
        }
    }
}

static int catalog(const char *key) {
    int normal = s8_species_id(key);
    if (normal < 0 || !S8_SPECIES[normal].initial || S8_SPECIES[normal].form_kind != S8_FORM_NORMAL)
        s8_error("expected legal normal-form species key", key);

    const char sexes[] = "mfn";
    for (int i = 0; i < 3; ++i) {
        int form = i == 1 && !strcmp(key, "meowstic") ? s8_species_id("meowsticf") : normal;
        if (form >= 0 && (S8_SPECIES[form].sex_mask & (1 << i)))
            profiles(form, sexes[i]);
    }
    return ferror(stdout) ? 1 : 0;
}

static int check(void) {
    char line[513];
    size_t count = 0;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strchr(line, ':')) s8_error("expected v3 with obsh4", line);
        Mon mon;
        init_mon(&mon, line);
        if (mon.policy != POL_OBSH4) s8_error("expected v3 with obsh4", line);
        s8_require_battle(&mon, &mon);
        ++count;
    }
    if (ferror(stdin)) return 1;
    printf("%zu\n", count);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "catalog")) return catalog(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "check")) return check();
    fprintf(stderr, "usage: population3_catalog catalog <species> | check < candidates.txt\n");
    return 2;
}
