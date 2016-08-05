/*
Program for the evaluation of positions from game records and the production of
a state->play file (.spb), to be used for further play suggestions besides
(Fuego-style) opening books.
*/


#include "matilda.h"

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "crc32.h"
#include "engine.h"
#include "file_io.h"
#include "mcts.h"
#include "opening_book.h"
#include "randg.h"
#include "sgf.h"
#include "state_changes.h"
#include "stringm.h"
#include "hash_table.h"
#include "timem.h"
#include "transpositions.h"
#include "zobrist.h"
#include "buffer.h"
#include "flog.h"

#define SECS_PER_TURN 30

#define MAX_FILES 500000
#define TABLE_BUCKETS 4957

static char * filenames[MAX_FILES];

static bool relax_komi = true;
static d32 ob_depth = (BOARD_SIZ * BOARD_SIZ) / 2;
static d32 minimum_samples = 32;


typedef struct __simple_state_transition_ {
    u8 p[PACKED_BOARD_SIZ];
    move play;
    u32 popularity;
    u32 hash;
} simple_state_transition;


static u32 hash_function(
    void * o
){
    simple_state_transition * s = (simple_state_transition *)o;
    return s->hash;
}

static d32 compare_function(
    void * o1,
    void * o2
){
    simple_state_transition * s1 = (simple_state_transition *)o1;
    simple_state_transition * s2 = (simple_state_transition *)o2;
    return memcmp(s1->p, s2->p, PACKED_BOARD_SIZ);
}


int main(int argc, char * argv[]){
    for(int i = 1; i < argc; ++i){
        if(strcmp(argv[i], "-version") == 0){
            printf("matilda %u.%u\n", VERSION_MAJOR, VERSION_MINOR);
            exit(EXIT_SUCCESS);
        }
        if(i < argc - 1 && strcmp(argv[i], "-max_depth") == 0){
            d32 a;
            if(!parse_int(argv[i + 1], &a) || a < 1)
                goto usage;
            ++i;
            ob_depth = a;
            continue;
        }
        if(i < argc - 1 && strcmp(argv[i], "-min_samples") == 0){
            d32 a;
            if(!parse_int(argv[i + 1], &a) || a < 1)
                goto usage;
            ++i;
            minimum_samples = a;
            continue;
        }
        if(strcmp(argv[i], "-relax_komi") == 0){
            relax_komi = true;
            continue;
        }

usage:
        printf("Usage: %s [options]\n", argv[0]);
        printf("Options:\n");
        printf("-max_depth number - Maximum turn depth of the openings. (defaul\
t: %u)\n", ob_depth);
        printf("-min_samples - Minimum number of samples for a rule to be saved\
. (default: %u)\n", minimum_samples);
        printf("-relax_komi - Allow games with uncommon komi values.\n");
        printf("-version - Print version information and exit.\n");
        exit(EXIT_SUCCESS);
    }

    timestamp();
    config_logging(DEFAULT_LOG_MODES);
    rand_init();
    assert_data_folder_exists();
    cfg_board_init();
    zobrist_init();
    transpositions_table_init();

    printf("%s: Creating table...\n", timestamp());
    hash_table * table = hash_table_create(TABLE_BUCKETS,
        sizeof(simple_state_transition), hash_function, compare_function);


    u32 games_used = 0;
    u32 unique_states = 0;

    printf("%s: Searching game record files (%s*.sgf)...\n", timestamp(),
        get_data_folder());
    u32 filenames_found = recurse_find_files(get_data_folder(), ".sgf",
        filenames, MAX_FILES);
    if(filenames_found == 0)
        printf("%s: No SGF files found.\n", timestamp());
    else
        printf("%s: Found %u SGF files.\n", timestamp(), filenames_found);

    printf("%s: Loading game states\n", timestamp());
    u32 fid;
    for(fid = 0; fid < filenames_found; ++fid)
    {
        if((fid % 512) == 0)
        {
            printf("\r%u%%", ((fid + 1) * 100) / filenames_found);
            fflush(stdout);
        }

        char * buf = get_buffer();
        d32 r = read_ascii_file(filenames[fid], buf, MAX_PAGE_SIZ);
        if(r <= 0 || r >= MAX_PAGE_SIZ)
        {
            fprintf(stderr, "\rerror: unexpected file size or read error\n");
            exit(EXIT_FAILURE);
        }
        buf[r] = 0;

        move plays[MAX_GAME_LENGTH];

        bool black_won;
        bool _ignored;
        bool normal_komi;

        if(!sgf_info(buf, &black_won, &_ignored, &_ignored, &normal_komi)){
            continue;
        }
        if(!relax_komi && !normal_komi){
            continue;
        }
        bool irregular_play_order;

        d16 plays_count = sgf_to_boards(buf, plays, &irregular_play_order);
        ++games_used;

        board b;
        clear_board(&b);

        d16 k;
        for(k = 0; k < MIN(ob_depth, plays_count); ++k)
        {
            if(plays[k] == PASS)
            {
                pass(&b);
                continue;
            }

            if(b.p[plays[k]] != EMPTY)
            {
                fprintf(stderr, "\rerror: file contains plays over stones\n");
                exit(EXIT_FAILURE);
            }

            bool is_black = (k & 1) == 0;

            board b2;
            memcpy(&b2, &b, sizeof(board));

            if(!attempt_play_slow(&b, plays[k], is_black))
            {
                fprintf(stderr, "\rerror: file contains illegal plays\n");
                exit(EXIT_FAILURE);
            }

            if(b.last_eaten != NONE)
                continue;

            d8 reduction = reduce_auto(&b2, is_black);
            plays[k] = reduce_move(plays[k], reduction);

            simple_state_transition stmp;
            memset(&stmp, 0, sizeof(simple_state_transition));
            pack_matrix(b2.p, stmp.p);
            stmp.hash = crc32(stmp.p, PACKED_BOARD_SIZ);

            simple_state_transition * entry =
                (simple_state_transition *)hash_table_find(table, &stmp);

            if(entry == NULL) /* new state */
            {
                entry = (simple_state_transition *)malloc(
                    sizeof(simple_state_transition));
                if(entry == NULL){
                    fprintf(stderr, "\rerror: new sst: system out of memory\n");
                    exit(EXIT_FAILURE);
                }
                memset(entry, 0, sizeof(simple_state_transition));
                memcpy(entry->p, stmp.p, PACKED_BOARD_SIZ);
                entry->hash = stmp.hash;
                entry->play = plays[k];
                entry->popularity = 1;

                hash_table_insert(table, entry);
                ++unique_states;
            }
            else
                entry->popularity++;
        }
    }

    printf("\nFound %u unique game states from %u games.\n", unique_states,
        games_used);
    if(unique_states == 0)
        return EXIT_SUCCESS;

    printf("\n%s: Evaluating game states and saving best play\n",
        timestamp());

    char * filename = get_buffer();
    snprintf(filename, MAX_PAGE_SIZ, "%soutput.spb", get_data_folder());
    printf("%s: Created output file %s\n\n\n", timestamp(), filename);
    FILE * fp = fopen(filename, "w");


    board b;
    clear_board(&b);
    out_board out_b;
    opening_book(&b, &out_b);

    u32 evaluated = 0;

    simple_state_transition ** ssts =
        (simple_state_transition **)hash_table_export_to_array(table);

    for(u32 idx = 0; ssts[idx]; ++idx)
    {
        simple_state_transition * sst = ssts[idx];

        if(sst->popularity >= (u32)minimum_samples)
        {
            evaluated++;

            unpack_matrix(b.p, sst->p);
            b.last_eaten = b.last_played = NONE;

            if(opening_book(&b, &out_b))
            {
                printf("%s: State already present in opening books.\n",
                    timestamp());
                continue;
            }

            u64 curr_time = current_time_in_millis();
            u64 stop_time = curr_time + SECS_PER_TURN * 1000;
            mcts_start(&b, true, &out_b, stop_time, stop_time);

            move best = select_play_fast(&out_b);
            tt_clean_all();

            if(!is_board_move(best))
            {
                printf("%s: Best play was to pass; ignored.\n", timestamp());
                continue;
            }

            char * str = get_buffer();
            u32 idx = 0;
            idx += snprintf(str + idx, MAX_PAGE_SIZ - idx, "%u ", BOARD_SIZ);

            for(move m = 0; m < BOARD_SIZ * BOARD_SIZ; ++m)
            {
                if(b.p[m] == BLACK_STONE)
                    idx += snprintf(str + idx, MAX_PAGE_SIZ - idx, "X");
                else
                    if(b.p[m] == WHITE_STONE)
                        idx += snprintf(str + idx, MAX_PAGE_SIZ - idx, "O");
                    else
                        idx += snprintf(str + idx, MAX_PAGE_SIZ - idx, ".");
            }

            snprintf(str + idx, MAX_PAGE_SIZ - idx, " %s\n",
                coord_to_alpha_num(best));


            size_t w = fwrite(str, strlen(str), 1, fp);
            if(w != 1)
            {
                fprintf(stderr, "error: write failed\n");
                exit(EXIT_FAILURE);
            }
            fflush(fp);
            fprintf(stderr, "%s", str);

            printf("%s: Best play: %s", timestamp(), coord_to_alpha_num(best));
            printf(" Actual play: %s\n\n\n", coord_to_alpha_num(sst->play));
        }
    }
    fclose(fp);
    printf("Evaluated %u unique states with enough samples.\n", evaluated);

    hash_table_destroy(table, true);

    printf("%s: Job done.\n", timestamp());
    return EXIT_SUCCESS;
}
