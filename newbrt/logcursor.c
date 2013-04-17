/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id: logcursor.c 13196 2009-07-10 14:41:51Z zardosht $"
#ident "Copyright (c) 2007, 2008, 2009 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

#include "includes.h"

struct toku_logcursor {
    char *logdir;
    char **logfiles;
    int n_logfiles;
    int cur_logfiles_index;
    FILE *cur_fp;
    BOOL is_open;
    struct log_entry entry;
    BOOL entry_valid;
};

static int lc_close_cur_logfile(TOKULOGCURSOR lc) {
    int r=0;
    if ( lc->is_open ) {
        r = fclose(lc->cur_fp);
        assert(0==r);
        lc->is_open = FALSE;
    }
    return 0;
}
static int lc_open_logfile(TOKULOGCURSOR lc, int index) {
    int r=0;
    assert( !lc->is_open );
    lc->cur_fp = fopen(lc->logfiles[index], "r");
    if ( lc->cur_fp == NULL ) 
        return DB_NOTFOUND;
    // position fp past header
    unsigned int version=0;
    r = toku_read_logmagic(lc->cur_fp, &version);
    if (r!=0) 
        return DB_BADFORMAT;
    if (version != 1)
        return DB_BADFORMAT;
    // mark as open
    lc->is_open = TRUE;
    return r;
}

// toku_logcursor_create()
//   - returns a pointer to a logcursor
int toku_logcursor_create(TOKULOGCURSOR *lc, const char *log_dir) {
    int failresult=0;
    int r=0;

    // malloc a cursor
    TOKULOGCURSOR cursor = (TOKULOGCURSOR) toku_malloc(sizeof(struct toku_logcursor));
    if ( NULL==cursor ) 
        return ENOMEM;
    // find logfiles in logdir
    cursor->is_open = FALSE;
    cursor->cur_logfiles_index = 0;
    cursor->entry_valid = FALSE;
    cursor->logdir = (char *) toku_malloc(strlen(log_dir)+1);
    if ( NULL==cursor->logdir ) 
        return ENOMEM;
    strcpy(cursor->logdir, log_dir);
    r = toku_logger_find_logfiles(cursor->logdir, &(cursor->logfiles), &(cursor->n_logfiles));
    if (r!=0) {
        failresult=r;
        goto fail;
    }
    *lc = cursor;
    return r;
 fail:
    toku_logcursor_destroy(&cursor);
    *lc = NULL;
    return failresult;
}

int toku_logcursor_destroy(TOKULOGCURSOR *lc) {
    int r=0;
    if ( (*lc)->entry_valid )
        toku_log_free_log_entry_resources(&((*lc)->entry));
    r = lc_close_cur_logfile(*lc);
    int lf;
    for(lf=0;lf<(*lc)->n_logfiles;lf++) {
        toku_free((*lc)->logfiles[lf]);
    }
    toku_free((*lc)->logfiles);
    toku_free((*lc)->logdir);
    toku_free(*lc);
    *lc = NULL;
    return r;
}

int toku_logcursor_next(TOKULOGCURSOR lc, struct log_entry **le) {
    int r=0;
    if ( lc->entry_valid ) 
        toku_log_free_log_entry_resources(&(lc->entry));
    if ( !lc->entry_valid ) {
        r = toku_logcursor_first(lc, le);
        return r;
    }
    r = toku_log_fread(lc->cur_fp, &(lc->entry));
    while ( EOF == r ) { 
        // move to next file
        r = lc_close_cur_logfile(lc);
        if (r!=0) 
            return r;
        if ( lc->cur_logfiles_index == lc->n_logfiles-1) 
            return DB_NOTFOUND;
        lc->cur_logfiles_index++;
        r = lc_open_logfile(lc, lc->cur_logfiles_index);
        if (r!= 0) 
            return r;
        r = toku_log_fread(lc->cur_fp, &(lc->entry));
    }
    if (r!=0) {
        if (r==DB_BADFORMAT) {
            fprintf(stderr, "Bad log format in %s\n", lc->logfiles[lc->cur_logfiles_index]);
            return r;
        } else {
            fprintf(stderr, "Unexpected log format error '%s' in %s\n", strerror(r), lc->logfiles[lc->cur_logfiles_index]);
            return r;
        }
    }
    *le = &(lc->entry);
    return r;
}

int toku_logcursor_prev(TOKULOGCURSOR lc, struct log_entry **le) {
    int r=0;
    if ( lc->entry_valid ) 
        toku_log_free_log_entry_resources(&(lc->entry));
    if ( !lc->entry_valid ) {
        r = toku_logcursor_last(lc, le);
        return r;
    }
    r = toku_log_fread_backward(lc->cur_fp, &(lc->entry));
    while ( -1 == r) { // if within header length of top of file
        // move to previous file
        r = lc_close_cur_logfile(lc);
        if (r!=0) 
            return r;
        if ( lc->cur_logfiles_index == 0 ) 
            return DB_NOTFOUND;
        lc->cur_logfiles_index--;
        r = lc_open_logfile(lc, lc->cur_logfiles_index);
        if (r!=0) 
            return r;
        r = toku_log_fread_backward(lc->cur_fp, &(lc->entry));
    }
    if (r!=0) {
        if (r==DB_BADFORMAT) {
            fprintf(stderr, "Bad log format in %s\n", lc->logfiles[lc->cur_logfiles_index]);
            return r;
        } else {
            fprintf(stderr, "Unexpected log format error '%s' in %s\n", strerror(r), lc->logfiles[lc->cur_logfiles_index]);
            return r;
        }
    }
    *le = &(lc->entry);
    return r;
}

int toku_logcursor_first(TOKULOGCURSOR lc, struct log_entry **le) {
    int r=0;
    if ( lc->entry_valid ) 
        toku_log_free_log_entry_resources(&(lc->entry));
    // close any but the first log file
    if ( lc->cur_logfiles_index != 0 ) {
        lc_close_cur_logfile(lc);
    }
    // open first log file if needed
    if ( !lc->is_open ) {
        r = lc_open_logfile(lc, 0);
        if (r!=0) 
            return r;
        lc->cur_logfiles_index = 0;
    }    
    r = toku_log_fread(lc->cur_fp, &(lc->entry));
    if (r!=0) 
        return r;
    lc->entry_valid = TRUE;
    *le = &(lc->entry);
    return r;
}

int toku_logcursor_last(TOKULOGCURSOR lc, struct log_entry **le) {
    int r=0;
    if ( lc->entry_valid ) 
        toku_log_free_log_entry_resources(&(lc->entry));
    // close any but last log file
    if ( lc->cur_logfiles_index != lc->n_logfiles-1 ) {
        lc_close_cur_logfile(lc);
    }
    // open last log file if needed
    if ( !lc->is_open ) {
        r = lc_open_logfile(lc, lc->n_logfiles-1);
        if (r!=0)
            return r;
        lc->cur_logfiles_index = lc->n_logfiles-1;
    }
    // seek to end
    r = fseek(lc->cur_fp, 0, SEEK_END);
    assert(0==r);
    // read backward
    r = toku_log_fread_backward(lc->cur_fp, &(lc->entry));
    if (r!=0) 
        return r;
    lc->entry_valid = TRUE;
    *le = &(lc->entry);
    return r;
}