/*
Functions that control the flow of information of Matilda as a complete Go
playing program. Allows executing strategies with some abstraction, performing
maintenance if needed.
*/

#include "matilda.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include "board.h"
#include "cfg_board.h"
#include "engine.h"
#include "flog.h"
#include "mcts.h"
#include "timem.h"
#include "opening_book.h"
#include "pat3.h"
#include "game_record.h"
#include "randg.h"
#include "random_play.h"
#include "scoring.h"
#include "transpositions.h"
#include "types.h"
#include "buffer.h"

static bool use_opening_book = true;

extern s16 komi_offset; /* only reset between matches */

bool tt_requires_maintenance = false; /* set after MCTS start/resume call */


static char _data_folder[MAX_PATH_SIZ] = DEFAULT_DATA_PATH;

/*
Obtains the current data folder path. It may be absolute or relative and ends
with a path separator.
RETURNS folder path
*/
const char * get_data_folder()
{
    return _data_folder;
}

/*
Sets the new data folder path. If the path is too long, short, or otherwise
invalid, nothing is changed and false is returned.
RETURNS true on success
*/
bool set_data_folder(
    const char * s
){
    u32 l = strlen(s);
    if(l < 2 || l >= MAX_PATH_SIZ - 1)
        return false;

    if(s[l - 1] == '/')
        snprintf(_data_folder, MAX_PATH_SIZ, "%s", s);
    else
        snprintf(_data_folder, MAX_PATH_SIZ, "%s/", s);
    return true;
}

/*
Set whether to attempt to use, or not, opening books prior to MCTS.
*/
void set_use_of_opening_book(
    bool use_ob
){
    use_opening_book = use_ob;
}

/*
Evaluates the position given the time available to think, by using a number of
strategies in succession.
RETURNS false if the player can resign
*/
bool evaluate_position(
    const board * b,
    bool is_black,
    out_board * out_b,
    u64 stop_time,
    u64 early_stop_time
){
    board tmp;
    memcpy(&tmp, b, sizeof(board));

    if(use_opening_book)
    {
        s8 reduction = reduce_auto(&tmp, is_black);
        if(opening_book(&tmp, out_b))
        {
            oboard_revert_reduce(out_b, reduction);
            return true;
        }
        memcpy(&tmp, b, sizeof(board));
    }

    double win_rate = mcts_start(&tmp, is_black, out_b, stop_time,
        early_stop_time);
    tt_requires_maintenance = true;
    return (win_rate >= UCT_MIN_WINRATE);
}

/*
Inform that we are currently between matches and proceed with the maintenance
that is suitable at the moment.
*/
void new_match_maintenance()
{
    transpositions_table_init();

    tt_clean_all();
    tt_requires_maintenance = false;
    komi_offset = 0;

    char * buf = get_buffer();
    snprintf(buf, MAX_PAGE_SIZ, "%s: mcts: freed all states between matches\n",
        timestamp());
    fprintf(stderr, "%s", buf);
    flog_info(buf);
}

/*
Perform between-turn maintenance. If there is any information from MCTS-UCT that
can be freed, it will be done to the states not reachable from state b played by
is_black.
*/
void opt_turn_maintenance(
    const board * b,
    bool is_black
){
    if(tt_requires_maintenance)
    {
        u32 freed = tt_clean_outside_tree(b, is_black);
        tt_requires_maintenance = false;

        char * buf = get_buffer();
        snprintf(buf, MAX_PAGE_SIZ, "%s: mcts: freed %u states (%lu MiB)\n",
            timestamp(), freed, (freed * sizeof(tt_stats)) / 1048576);
        fprintf(stderr, "%s", buf);
        flog_info(buf);
    }
}

/*
Asserts the ./data folder exists, closing the program and warning the user if it
doesn't.
*/
void assert_data_folder_exists()
{
    DIR * dir = opendir(get_data_folder());
    if(dir == NULL)
    {
        char * buf = get_buffer();
        snprintf(buf, MAX_PAGE_SIZ,
            "error: data folder %s does not exist or is unavailable\n",
            get_data_folder());
        fprintf(stderr, "%s", buf);
        flog_crit(buf);
        exit(EXIT_FAILURE);
    }else
        closedir(dir);
}






