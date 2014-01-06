/* linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 * 
 *   http://github.com/oldium/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2013, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2013-2014, Oldrich Jedlicka <oldium dot pro at seznam dot cz>
 *
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://github.com/antirez/linenoise
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 * - http://www.ecma-international.org/publications/standards/Ecma-035.htm
 * - http://www.ecma-international.org/publications/standards/Ecma-048.htm
 *
 * Todo list:
 * - Win32 support
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * CHA (Cursor Horizontal Absolute)
 *    Sequence: ESC [ n G
 *    Effect: moves cursor to column n
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward of n chars
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * The following are used to clear the screen: ESC [ H ESC [ 2 J
 * This is actually composed of two sequences:
 *
 * cursorhome
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED2 (Clear entire screen)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 * 
 */

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <locale.h>
#include <signal.h>
#include <sys/select.h>
#include "linenoise.h"

#define RETRY(expression) \
	( { int result = (expression); while (result == -1 && errno == EINTR ) result = (expression); result; } )

typedef struct linenoiseSingleCompletion {
    char *suggestion;   /* Suggestion to display. */
    char *text;         /* Fully completed text. */
    size_t pos;         /* Cursor position. */
} linenoiseSingleCompletion;

struct linenoiseCompletions {
  bool is_initialized;  /* True if completions are initialized. */
  size_t len;           /* Current number of completions. */
  size_t max_strlen;    /* Maximum suggestion text length. */
  linenoiseSingleCompletion *cvec;  /* Array of completions. */
};

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_LINE_INIT_MAX_AND_GROW 4096
#define LINENOISE_COL_SPACING 2
#define ANSI_ESCAPE_MAX_LEN 16
#define ANSI_ESCAPE_WAIT_MS 50  /* Wait 50ms for further ANSI codes, otherwise return escape */
#define READ_BACK_MAX_LEN 32

#define MIN(a, b) ((a) < (b) ? (a) : (b))
static char *unsupported_term[] = {"dumb","cons25",NULL};
static linenoiseCompletionCallback *completionCallback = NULL;

static struct termios orig_termios; /* In order to restore at exit.*/
static int rawmode = 0; /* For atexit() function to check if restore is needed*/
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
char **history = NULL;

enum LinenoiseState {
    LS_NEW_LINE,        /* Processing new line. */
    LS_READ,            /* Reading new line. */
    LS_COMPLETION,      /* Completing with TAB. */
    LS_HISTORY_SEARCH   /* Searching with CTRL+R. */
};

enum ReadCharSpecials {
    RCS_NONE = 0,               /* No character read. */
    RCS_ERROR = -1,             /* Error. */
    RCS_CLOSED = -2,            /* Connection has been closed. */
    RCS_CANCELLED = -3,         /* Line editing has been cancelled. */
    RCS_ANSI_CURSOR_LEFT = -4,  /* Left key. */
    RCS_ANSI_CURSOR_RIGHT = -5, /* Right key. */
    RCS_ANSI_CURSOR_UP = -6,    /* Up key. */
    RCS_ANSI_CURSOR_DOWN = -7,  /* Down key. */
    RCS_ANSI_DELETE = -8,       /* Delete key. */
    RCS_ANSI_HOME = -9,         /* Home key. */
    RCS_ANSI_END = -10          /* End key. */
};

enum AnsiEscapeState {
    AES_NONE = 0,               /* Not in escape sequence. */
    AES_INTERMEDIATE = 1,       /* Intermediate sequence. */
    AES_CSI_PARAMETER = 2,      /* Inside CSI parameter sequence. */
    AES_CSI_INTERMEDIATE = 3,   /* Inside CSI intermediate sequence. */
    AES_SS_CHARACTER = 4,       /* Reading SS2 or SS3 character value. */
    AES_FINAL = 5               /* Read final character. */
};

enum AnsiSequenceMeaning {
    ASM_C1,     /** Read C1 escape sequence. */
    ASM_CSI,    /** Read CSI escape sequence. */
    ASM_G2,     /** Read SS2 escape and a character value. */
    ASM_G3      /** Read SS3 escape and a character value. */
};

enum LinenoiseResult {
    LR_HAVE_TEXT = 1,   /** Text is available to be returned. */
    LR_CLOSED = 0,      /** Connection has been closed with no text to be returned. */
    LR_ERROR = -1,      /** Error occurred. */
    LR_CANCELLED = -2,  /** Current line editing has been cancelled. */
    LR_CONTINUE = -3    /** Continue reading next character. */
};

typedef struct linenoiseAnsi {
    timer_t ansi_timer;         /* Timer to differentiate between ESC and ANSI escape sequence. */
    bool ansi_timer_is_active;  /* True if the timer is active. */
    int ansi_timer_overrun_count;   /* Overrun count of timer. */
    enum AnsiEscapeState ansi_state;    /* ANSI sequence reading state */
    char ansi_escape[ANSI_ESCAPE_MAX_LEN + 1];  /* RAW read ANSI escape sequence */
    char ansi_intermediate[ANSI_ESCAPE_MAX_LEN + 1];    /* Intermediate sequence. */
    char ansi_parameter[ANSI_ESCAPE_MAX_LEN + 1];   /* Parameter sequence. */
    char ansi_final;    /* Final character of sequence. */
    enum AnsiSequenceMeaning ansi_sequence_meaning;   /* Read sequence meaning. */
    int ansi_escape_len;                    /* Current length of sequence */
    int ansi_intermediate_len;              /* Current length of intermediate block */
    int ansi_parameter_len;                 /* Current length of parameter block */
} linenoiseAnsi;

typedef struct linenoiseHistorySearchState {
    char *hist_search_buf;          /* Text to be searched. */
    size_t hist_search_buflen;      /* Buffer length. */
    size_t hist_search_len;         /* Length of text to be searched. */
    int current_index;              /* Current history index. */
    bool found;                     /* True if current text has been fouond. */
} linenoiseHistorySearchState;

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
    int fd;             /* Terminal file descriptor. */
    char *buf;          /* Edited line buffer. */
    size_t buflen;      /* Edited line buffer size. */
    char *prompt;       /* Prompt to display. */
    size_t plen;        /* Prompt length. */
    char *tempprompt;   /* Temporary prompt to display. */
    size_t tempplen;    /* Temporary prompt length. */
    size_t pos;         /* Current cursor position. */
    size_t oldpos;      /* Previous refresh cursor position. */
    size_t oldrpos;     /* Previous cursor row position. */
    size_t len;         /* Current edited line length. */
    size_t cols;        /* Number of columns in terminal. */
    size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
    int history_index;  /* The history index we are currently editing. */
    bool is_async;      /* True when the STDIN is in O_NONBLOCK mode. */
    bool needs_refresh; /* True when the lines need to be refreshed. */
    bool is_displayed;  /* True when the prompt has been displayed. */
    bool is_cancelled;  /* True when the input has been cancelled (CTRL+C). */
    bool is_closed;     /* True once the input has been closed. */
    enum LinenoiseState state;  /* Internal state. */
    linenoiseCompletions comp;  /* Line completions. */
    bool sigint_blocked;   /* True when the SIGINT is blocked. */
    sigset_t sigint_oldmask;    /* Old signal mask. */
    linenoiseAnsi ansi; /* ANSI escape sequence state machine. */
    int read_back_char[READ_BACK_MAX_LEN];      /* Read-back buffer for characters. */
    int read_back_char_len;                     /* Number of characters in buffer. */
    linenoiseHistorySearchState hist_search;    /* History search. */
};

static struct linenoiseState state = {      /* Line editing state. */
        fd: STDIN_FILENO,
        state: LS_NEW_LINE
};
static bool initialized = false;        /* True if line editing has been initialized. */

static void linenoiseAtExit(void);
static int refreshLine(struct linenoiseState *l);
static int initialize(struct linenoiseState *l);
static int ensureBufLen(struct linenoiseState *l, size_t requestedStrLen);
static int prepareCustomOutputOnNewLine(struct linenoiseState *l);
static int prepareCustomOutputClearLine(struct linenoiseState *l);
static int freeHistorySearch(struct linenoiseState *l);

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void linenoiseSetMultiLine(int ml) {
    mlmode = ml;
}

/* Return true if the terminal is not a TTY or the name is in the list of
 * terminals we know are not able to understand basic escape sequences. */
static int isUnsupportedTerm(void) {
    if ( !isatty(STDIN_FILENO) )
        return 1;

    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term,unsupported_term[j])) return 1;
    return 0;
}

/* Raw mode: 1960 magic shit. */
static int enableRawMode(int fd) {
    if (!rawmode) {
        struct termios raw;

        if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

        raw = orig_termios;  /* modify the original mode */
        /* input modes: no CR to NL, no parity check, no strip char,
         * no start/stop output control. */
        raw.c_iflag &= ~(ICRNL | INLCR | INPCK | ISTRIP | IXON);
        /* output modes - disable post processing */
        raw.c_oflag &= ~(OPOST);
        /* control modes - set 8 bit chars */
        raw.c_cflag |= (CS8);
        /* local modes - choing off, canonical off, no extended functions,
         * no signal chars (^Z,^C) */
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
        /* control chars - set return condition: min number of bytes and timer.
         * We want read to return every single byte, without timeout. */
        raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

        /* put terminal in raw mode after flushing */
        if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
        rawmode = 1;
    }
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

/* Disable raw mode. */
static void disableRawMode(int fd) {
    int saved_errno = errno;
    /* Don't even check the return value as it's too late. */
    if (rawmode && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
        rawmode = 0;
    errno = saved_errno;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(void) {
    struct winsize ws;

    if (RETRY(ioctl(1, TIOCGWINSZ, &ws)) == -1 || ws.ws_col == 0) return 80;
    return ws.ws_col;
}

/* Block SIGINT, SIGALRM and SIGWINCH signals. */
static bool blockSignals(struct linenoiseState *ls) {
    if (!ls->sigint_blocked) {
        int old_errno = errno;
        sigset_t newset;
        sigemptyset(&newset);
        sigemptyset(&ls->sigint_oldmask);
        sigaddset(&newset, SIGINT);
        sigaddset(&newset, SIGALRM);
        sigaddset(&newset, SIGWINCH);
        pthread_sigmask(SIG_BLOCK, &newset, &ls->sigint_oldmask);
        ls->sigint_blocked = true;
        errno = old_errno;
        return true;
    } else {
        return false;
    }
}

/* Re-enable SIGINT, SIGALRM and SIGWINCH signals. */
static bool revertSignals(struct linenoiseState *ls) {
    if (ls->sigint_blocked) {
        int old_errno = errno;
        pthread_sigmask(SIG_SETMASK, &ls->sigint_oldmask, NULL);
        ls->sigint_blocked = false;
        errno = old_errno;
        return true;
    } else
        return false;
}

/* Clear the screen. Used to handle ctrl+l */
int clearScreen(struct linenoiseState *ls) {
    if (RETRY(write(STDIN_FILENO,"\x1b[H\x1b[2J",7)) <= 0) {
        /* nothing to do, just to avoid warning. */
    }
    ls->needs_refresh = true;
    ls->maxrows = 0;
    return 0;
}

/* Clear the screen. Used to handle ctrl+l */
int linenoiseClearScreen(void) {
    return clearScreen(&state);
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static int linenoiseBeep(void) {
    if (fprintf(stderr, "\x7") < 0) return -1;
    if (RETRY(fflush(stderr)) == -1) return -1;
    return 0;
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void freeCompletions(struct linenoiseState *ls) {
    size_t i;
    if (ls->comp.cvec != NULL) {
        for (i = 0; i < ls->comp.len; i++) {
            free(ls->comp.cvec[i].suggestion);
            free(ls->comp.cvec[i].text);
        }
        free(ls->comp.cvec);
    }

    ls->comp.is_initialized = false;
    ls->comp.cvec = NULL;
    ls->comp.len = 0;
    ls->comp.max_strlen = 0;
}

/* Compare completions, used for Quick sort. */
static int completitionCompare(const void *first, const void *second)
{
    linenoiseSingleCompletion *firstcomp = (linenoiseSingleCompletion *) first;
    linenoiseSingleCompletion *secondcomp = (linenoiseSingleCompletion *) second;
    return (strcoll(firstcomp->suggestion, secondcomp->suggestion));
}

/* This is an helper function for linenoiseEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 * 
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition. */
static int completeLine(struct linenoiseState *ls) {
    if (ls->comp.len == 0) {
        if (linenoiseBeep() == -1) return -1;
        if (ls->needs_refresh) {
            if (refreshLine(ls) == -1) return -1;
        }
    } else if (ls->comp.len == 1) {
        // Simple case
        size_t new_strlen = strlen(ls->comp.cvec[0].text);
        if (ensureBufLen(ls, new_strlen) == -1) return -1;

        memcpy(ls->buf, ls->comp.cvec[0].text, new_strlen+1);
        ls->pos = MIN(new_strlen, ls->comp.cvec[0].pos);
        ls->len = new_strlen;
        if (refreshLine(ls) == -1) return -1;
    } else {
        // Multiple choices - sort them and print
        if (prepareCustomOutputOnNewLine(ls) == -1) return -1;

        qsort(ls->comp.cvec, ls->comp.len, sizeof(linenoiseSingleCompletion), completitionCompare);
        size_t colSize = ls->comp.max_strlen + LINENOISE_COL_SPACING;
        size_t cols = ls->cols / colSize;
        if (cols == 0)
            cols = 1;
        size_t rows = (ls->comp.len + cols - 1) / cols;
        size_t i;

        for (i = 0; i < ls->comp.len; i++)
        {
            size_t real_index = (i % cols) * rows + i / cols;
            if (real_index < ls->comp.len)
                printf("%-*s", (int)colSize, ls->comp.cvec[real_index].suggestion);
            if ((i % cols) == (cols - 1))
                printf("\r\n");
        }
        if ((i % cols) != 0)
            printf("\r\n");
        if (refreshLine(ls) == -1) return -1;
    }

    return 0;
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
    completionCallback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *suggestion, const char *completed_text, size_t pos) {
    size_t len = strlen(suggestion);
    char *copy_suggestion = malloc(len+1);
    char *copy_text = strdup(completed_text);
    if (copy_suggestion == NULL || copy_text == NULL) goto error_cleanup;
    memcpy(copy_suggestion,suggestion,len+1);
    if (lc->len == 0 || lc->cvec != NULL) {
        linenoiseSingleCompletion *newcvec = realloc(lc->cvec,sizeof(linenoiseSingleCompletion)*(lc->len+1));;
        if (newcvec == NULL) goto error_cleanup;
        lc->cvec = newcvec;
        lc->cvec[lc->len].suggestion = copy_suggestion;
        lc->cvec[lc->len].text = copy_text;
        lc->cvec[lc->len].pos = pos;
    }
    goto end;

error_cleanup:
    free(copy_suggestion);
    free(copy_text);

    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i].text);
    free(lc->cvec);
    lc->cvec = NULL;

end:
    if (len > lc->max_strlen)
        lc->max_strlen = len;
    lc->len++;
}

/* =========================== Line editing ================================= */

/* Get the prompt to be displayed. */
char *getPrompt(struct linenoiseState *l, size_t *plen)
{
    if (l->tempprompt != NULL) {
        if (plen != NULL)
            *plen = l->tempplen;
        return l->tempprompt;
    } else {
        if (plen != NULL)
            *plen = l->plen;
        return l->prompt;
    }
}

/* Set the main prompt. */
int setPrompt(struct linenoiseState *l, const char *prompt)
{
    char *oldPrompt = getPrompt(l, NULL);
    if (oldPrompt == NULL || strcmp(oldPrompt, prompt) != 0)
        l->needs_refresh = true;
    if (l->prompt == NULL || strcmp(l->prompt, prompt) != 0) {
        free(l->prompt);
        l->prompt = strdup(prompt);
        l->plen = strlen(prompt);
        if (l->prompt == NULL) {
            errno = ENOMEM;
            return -1;
        }
    }
    return 0;
}

/* Set or reset (in case of NULL argument) the temporary prompt, which has
 * priority over the main prompt. */
int setTempPrompt(struct linenoiseState *l, const char *tempprompt)
{
    char *oldPrompt = getPrompt(l, NULL);
    if ((oldPrompt == NULL && tempprompt != NULL)
            || (oldPrompt != NULL && tempprompt == NULL)
            || (oldPrompt != NULL && tempprompt != NULL
                    && strcmp(oldPrompt, tempprompt) != 0))
        l->needs_refresh = true;

    if (l->tempprompt != NULL) {
        free(l->tempprompt);
        l->tempprompt = NULL;
    }
    if (tempprompt != NULL) {
        l->tempprompt = strdup(tempprompt);
        l->tempplen = strlen(tempprompt);
        if (l->tempprompt == NULL) {
            errno = ENOMEM;
            return -1;
        }
    }
    return 0;
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static int refreshSingleLine(struct linenoiseState *l) {
    char seq[64];
    size_t plen;
    char *prompt = getPrompt(l, &plen);
    int fd = l->fd;
    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    
    while((plen+pos) >= l->cols) {
        buf++;
        len--;
        pos--;
    }
    while (plen+len > l->cols) {
        len--;
    }

    /* Cursor to left edge */
    if (snprintf(seq,64,"\x1b[0G") < 0) return -1;
    if (RETRY(write(fd,seq,strlen(seq))) == -1) return -1;
    /* Write the prompt and the current buffer content */
    if (prompt != NULL && plen > 0 && RETRY(write(fd,prompt,plen)) == -1) return -1;
    if (RETRY(write(fd,buf,len)) == -1) return -1;
    /* Erase to right */
    if (snprintf(seq,64,"\x1b[0K") < 0) return -1;
    if (RETRY(write(fd,seq,strlen(seq))) == -1) return -1;
    /* Move cursor to original position. */
    if (pos+plen != 0) {
        if (snprintf(seq,64,"\x1b[0G\x1b[%zuC", (pos+plen)) < 0) return -1;
        if (RETRY(write(fd,seq,strlen(seq))) == -1) return -1;
    }
    return 0;
}

/* Multi-line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static int refreshMultiLine(struct linenoiseState *l) {
    char seq[64];
    size_t plen;
    char *prompt = getPrompt(l, &plen);
    int rows = (plen+l->len+l->cols-1)/l->cols; /* rows used by current buf. */
    int rpos = l->oldrpos;  /* cursor relative row. */
    int rpos2; /* rpos after refresh. */
    int old_rows = l->maxrows;
    int fd = l->fd, j;

    /* Update maxrows if needed. */
    if (rows > (int)l->maxrows) l->maxrows = rows;

#ifdef LN_DEBUG
    FILE *fp = fopen("/tmp/debug.txt","a");
    fprintf(fp,"[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d",
        (int)l->len,(int)l->pos,(int)l->oldpos,plen,rows,rpos,(int)l->maxrows,old_rows);
#endif

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    if (old_rows-rpos > 0) {
#ifdef LN_DEBUG
        fprintf(fp,", go down %d", old_rows-rpos);
#endif
        if (snprintf(seq,64,"\x1b[%dB", old_rows-rpos) < 0) return -1;
        if (RETRY(write(fd,seq,strlen(seq))) == -1) return -1;
    }

    /* Now for every row clear it, go up. */
    for (j = 0; j < old_rows-1; j++) {
#ifdef LN_DEBUG
        fprintf(fp,", clear+up");
#endif
        if (snprintf(seq,64,"\x1b[0G\x1b[0K\x1b[1A") < 0) return -1;
        if (RETRY(write(fd,seq,strlen(seq))) == -1) return -1;
    }

    /* Clean the top line. */
#ifdef LN_DEBUG
    fprintf(fp,", clear");
#endif
    if (snprintf(seq,64,"\x1b[0G\x1b[0K") < 0) return -1;
    if (RETRY(write(fd,seq,strlen(seq))) == -1) return -1;
    
    /* Write the prompt and the current buffer content */
    if (prompt != NULL && plen > 0 && RETRY(write(fd,prompt,plen)) == -1) return -1;
    if (RETRY(write(fd,l->buf,l->len)) == -1) return -1;

    /* If we are at the very end of the screen with our prompt, we need to
     * emit a newline and move the prompt to the first column. */
    if (l->pos &&
        l->pos == l->len &&
        (l->pos+plen) % l->cols == 0)
    {
#ifdef LN_DEBUG
        fprintf(fp,", <newline>");
#endif
        if (RETRY(write(fd,"\n",1)) == -1) return -1;
        if (snprintf(seq,64,"\x1b[0G") < 0) return -1;
        if (RETRY(write(fd,seq,strlen(seq))) == -1) return -1;
        rows++;
        if (rows > (int)l->maxrows) l->maxrows = rows;
    }

    /* Move cursor to right position. */
    rpos2 = (plen+l->pos+l->cols)/l->cols; /* current cursor relative row. */
#ifdef LN_DEBUG
    fprintf(fp,", rpos2 %d", rpos2);
#endif
    /* Go up till we reach the expected positon. */
    if (rows-rpos2 > 0) {
#ifdef LN_DEBUG
        fprintf(fp,", go-up %d", rows-rpos2);
#endif
        if (snprintf(seq,64,"\x1b[%dA", rows-rpos2) < 0) return -1;
        if (RETRY(write(fd,seq,strlen(seq))) == -1) return -1;
    }
    /* Set column. */
#ifdef LN_DEBUG
    fprintf(fp,", set col %zu", 1+((plen+l->pos) % l->cols));
#endif
    if (snprintf(seq,64,"\x1b[%zuG", 1+((plen+l->pos) % l->cols)) < 0) return -1;
    if (write(fd,seq,strlen(seq)) == -1) return -1;

    l->oldpos = l->pos;
    l->oldrpos = rpos2;

#ifdef LN_DEBUG
    fprintf(fp,"\n");
    fclose(fp);
#endif
    return 0;
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static int refreshLine(struct linenoiseState *l) {
    int result;
    if (mlmode)
        result = refreshMultiLine(l);
    else
        result = refreshSingleLine(l);
    if (result == 0) {
        l->needs_refresh = false;
        l->is_displayed = true;
    }
    return result;
}

/* Reset the display state when moved to new line. */
void resetOnNewline(struct linenoiseState *l)
{
    l->maxrows = 0;
    l->is_displayed = false;
    l->needs_refresh = true;
}

/* Prepare output for custom printing with no prompt; undo the raw mode.
 * Returns 0 on success, -1 on error. */
static int prepareCustomOutputClearLine(struct linenoiseState *l)
{
    if (l->is_displayed) {
        struct linenoiseState oldstate = *l;
        l->tempprompt = "";
        l->tempplen = 0;
        l->len = l->pos = 0;

        if (refreshLine(l) == -1) return -1;

        l->tempprompt = oldstate.tempprompt;
        l->tempplen = oldstate.tempplen;
        l->pos = oldstate.pos;
        l->len = oldstate.len;

        resetOnNewline(l);
    }
    return 0;
}

/* Prepare output for custom printing with no prompt; undo the raw mode. */
void linenoiseCustomOutput()
{
    (void) prepareCustomOutputClearLine(&state);
    disableRawMode(state.fd);
}

/* Prepare custom output on new line.
 * Returns 0 on success, -1 on error. */
static int prepareCustomOutputOnNewLine(struct linenoiseState *l)
{
    if (l->is_displayed) {
        struct linenoiseState oldstate = *l;
        l->pos = l->len;
        if (refreshLine(l) == -1) return -1;

        printf("\r\n");

        l->pos = oldstate.pos;

        resetOnNewline(l);
    }
    return 0;
}

/* Ensure that the read buffer is big enough.
 * Returns 0 on success, -1 on error. */
static int ensureBufLen(struct linenoiseState *l, size_t requestedStrLen)
{
    if (l->buflen < requestedStrLen) {
        size_t newlen = l->buflen + LINENOISE_LINE_INIT_MAX_AND_GROW;
        while (newlen < requestedStrLen)
            newlen += LINENOISE_LINE_INIT_MAX_AND_GROW;

        char *newbuf = realloc(l->buf, newlen);
        if (newbuf == NULL) {
            errno = ENOMEM;
            return -1;
        }
        l->buf = newbuf;
        l->buflen = newlen;
        l->buf[l->buflen-1] = '\0';
    }
    return 0;
}

/* Decodes ANSI escape sequence.
 * Returns ReadCharSpecials or RCS_NONE in case unknown sequence is read. */
int ansiDecode(struct linenoiseAnsi *la)
{
    if (la->ansi_sequence_meaning == ASM_CSI) {
        switch (la->ansi_final)
        {
        case 0x41:
            if (la->ansi_parameter_len == 0)
                return RCS_ANSI_CURSOR_UP;
            else
                break;
        case 0x42:
            if (la->ansi_parameter_len == 0)
                return RCS_ANSI_CURSOR_DOWN;
            else
                break;
        case 0x43:
            if (la->ansi_parameter_len == 0)
                return RCS_ANSI_CURSOR_RIGHT;
            else
                break;
        case 0x44:
            if (la->ansi_parameter_len == 0)
                return RCS_ANSI_CURSOR_LEFT;
            else
                break;
        case 0x46:
            if (la->ansi_parameter_len == 0)
                return RCS_ANSI_END;
            else
                break;
        case 0x48:
            if (la->ansi_parameter_len == 0)
                return RCS_ANSI_HOME;
            else
                break;
        case 0x7E: {    // 0x70 to 0x7E are fpr private use as per ECMA-048
            if (strcmp(la->ansi_parameter, "1") == 0)
                return RCS_ANSI_HOME;
            else if (strcmp(la->ansi_parameter, "3") == 0)
                return RCS_ANSI_DELETE;
            if (strcmp(la->ansi_parameter, "4") == 0)
                return RCS_ANSI_END;
            break;
        }
        default: break;
        }
    }
    return RCS_NONE;
}

/* Add ANSI character sequence and process the state.
 * Return true in case of success, false on failure. */
bool ansiAddCharacter(struct linenoiseAnsi *la, unsigned char c)
{
    if (la->ansi_escape_len == 0) {
        la->ansi_escape[la->ansi_escape_len++] = c;
        la->ansi_intermediate_len = 0;
        la->ansi_parameter_len = 0;
        la->ansi_sequence_meaning = ASM_C1;
        la->ansi_state = AES_INTERMEDIATE;
    } else {
        if (la->ansi_state == AES_INTERMEDIATE && c >= 0x20 && c <= 0x2F) {
            la->ansi_escape[la->ansi_escape_len++] = c;
            la->ansi_intermediate[la->ansi_intermediate_len++] = c;
        } else if (la->ansi_state == AES_CSI_INTERMEDIATE && c >= 0x20 && c < 0x2F) {
            la->ansi_escape[la->ansi_escape_len++] = c;
            la->ansi_intermediate[la->ansi_intermediate_len++] = c;
        } else if (la->ansi_state == AES_CSI_PARAMETER && c >= 0x20 && c < 0x2F) {
            la->ansi_state = AES_CSI_INTERMEDIATE;
            la->ansi_escape[la->ansi_escape_len++] = c;
            la->ansi_intermediate[la->ansi_intermediate_len++] = c;
        } else if (la->ansi_state == AES_CSI_PARAMETER && c >= 0x30 && c < 0x3F) {
            la->ansi_escape[la->ansi_escape_len++] = c;
            la->ansi_parameter[la->ansi_parameter_len++] = c;
        } else if (la->ansi_state == AES_INTERMEDIATE && c >= 0x30 && c < 0x7F ) {
            la->ansi_escape[la->ansi_escape_len++] = c;
            if (la->ansi_escape_len == 2 && c == 0x5B) {
                la->ansi_state = AES_CSI_PARAMETER;
                la->ansi_sequence_meaning = ASM_CSI;
            } else if (la->ansi_escape_len == 2 && c == 0x4E) {
                la->ansi_state = AES_SS_CHARACTER;
                la->ansi_sequence_meaning = ASM_G2;
            } else if (la->ansi_escape_len == 2 && c == 0x4F) {
                la->ansi_state = AES_SS_CHARACTER;
                la->ansi_sequence_meaning = ASM_G3;
            } else {
                la->ansi_final = c;
                la->ansi_state = AES_FINAL;
            }
        } else if ((la->ansi_state == AES_CSI_INTERMEDIATE || la->ansi_state == AES_CSI_PARAMETER) && c >= 0x40 && c < 0x7F) {
            la->ansi_escape[la->ansi_escape_len++] = c;
            la->ansi_final = c;
            la->ansi_state = AES_FINAL;
        } else if (la->ansi_state == AES_SS_CHARACTER) {
            la->ansi_escape[la->ansi_escape_len++] = c;
            la->ansi_final = c;
            la->ansi_state = AES_FINAL;
        } else {
            // Invalid character
            return false;
        }
    }
    if (la->ansi_state == AES_FINAL) {
        la->ansi_escape[la->ansi_escape_len] = '\0';
        la->ansi_parameter[la->ansi_parameter_len] = '\0';
        la->ansi_intermediate[la->ansi_intermediate_len] = '\0';
    }
    return la->ansi_escape_len != ANSI_ESCAPE_MAX_LEN || la->ansi_state == AES_FINAL;
}

/* Add character or ReadCharSpecials value to be first returned by next call to
 * readChar(). Returns true if successful, false if argument was RCS_NONE. */
bool pushFrontChar(struct linenoiseState *l, int c)
{
    if (c != RCS_NONE) {
        if (l->read_back_char_len == READ_BACK_MAX_LEN)
            l->read_back_char_len--;
        if (l->read_back_char_len > 0)
            memmove(l->read_back_char + 1, l->read_back_char,
                    l->read_back_char_len * sizeof(l->read_back_char[0]));
        l->read_back_char[0] = c;
        l->read_back_char_len++;
        return true;
    } else {
        return false;
    }
}

/* Read character and decode ANSI escape sequences.
 * Returns character code (value greater than zero), or one of ReadCharSpecials
 * values, but never RCS_NONE (0). */
int readChar(struct linenoiseState *l)
{
    int result = RCS_NONE;
    unsigned char c;
    int nread = -1;

    if (l->is_cancelled) {
        l->is_cancelled = false;
        return RCS_CANCELLED;
    }

    while (result == RCS_NONE) {
        if (l->needs_refresh) {
            if (refreshLine(l) == -1) return -1;
        }
        if (l->read_back_char_len > 0) {
            result = l->read_back_char[0];
            l->read_back_char_len--;
            if (l->read_back_char_len > 0) {
                memmove(l->read_back_char, l->read_back_char + 1,
                        l->read_back_char_len * sizeof(l->read_back_char[0]));
            }
        } else {
            int selectresult = 1;
            nread = -1;

            do {
                errno = 0;

                // pselect is always interrupted by EINTR in case of an interrupt
                if (l->is_async) {
                    // Temporarily restore old mask
                    (void) revertSignals(l);
                    nread = read(l->fd, &c, 1);
                    (void) blockSignals(l);
                } else {
                    fd_set fds;
                    FD_ZERO(&fds);
                    FD_SET(l->fd, &fds);

                    // Do select with enabled interrupts
                    selectresult = pselect(l->fd + 1, &fds, NULL, NULL, NULL, &l->sigint_oldmask);
                    if (selectresult == 1)
                        nread = read(l->fd, &c, 1);
                }

                if (l->is_cancelled) {
                    l->is_cancelled = false;
                    result = RCS_CANCELLED;
                }
                if (l->needs_refresh) {
                    if (refreshLine(l) == -1) return -1;
                }

                // Check expiration of ESC key and sequence recognition
                if (l->ansi.ansi_timer_is_active) {
                    int old_errno = errno;
                    struct itimerspec timerExpiry = {{0, 0}, {0, 0}};
                    if (timer_gettime(l->ansi.ansi_timer, &timerExpiry) == -1) return -1;
                    if (timerExpiry.it_value.tv_sec == 0 && timerExpiry.it_value.tv_nsec == 0) {
                        // Timer expired - it was an ESC key
                        l->ansi.ansi_timer_is_active = false;
                        if (result != RCS_NONE) {
                            pushFrontChar(l, result);
                            result = 27;
                        } else {
                            if (nread < 0 && (old_errno == EINTR || old_errno == EAGAIN || old_errno == EWOULDBLOCK)) {
                                result = 27;
                            } else {
                                pushFrontChar(l, 27);
                            }
                        }
                    } else if (nread == 1) {
                        // Timer has not expired and we received a key
                        l->ansi.ansi_timer_is_active = false;
                        (void) ansiAddCharacter(&l->ansi, 27);

                        struct itimerspec timerDisarm = {{0, 0}, {0, 0}};
                        if (timer_settime(l->ansi.ansi_timer, 0, &timerDisarm, NULL) == -1) return -1;
                    }
                    errno = old_errno;
                }
            } while (result == RCS_NONE && nread < 0 && errno == EINTR);
        }

        if (result == RCS_NONE) {
            if (nread < 0) {
                result = RCS_ERROR;
            } else if (nread == 0) {
                result = RCS_CLOSED;
            } else if (l->ansi.ansi_escape_len == 0 && c != 27) {
                result = c;
            } else if (l->ansi.ansi_escape_len == 0) {
                // ANSI escape begin, set the timer
                struct itimerspec timerNext = {it_value: {tv_nsec: ANSI_ESCAPE_WAIT_MS * 1000000L}};
                if (timer_settime(l->ansi.ansi_timer, 0, &timerNext, NULL) == -1) return -1;
                l->ansi.ansi_timer_is_active = true;
            } else {
                // ANSI escape continuation
                if (ansiAddCharacter(&l->ansi, c)) {
                    if (l->ansi.ansi_state == AES_FINAL) {
                        result = ansiDecode(&l->ansi);
                        l->ansi.ansi_escape_len = 0;
                    }
                } else {
                    if (l->read_back_char_len + l->ansi.ansi_escape_len < READ_BACK_MAX_LEN) {
                        int i;
                        for (i = 0; i < l->ansi.ansi_escape_len; i++) {
                            l->read_back_char[l->read_back_char_len++] =
                                    (unsigned char) l->ansi.ansi_escape[i];
                        }
                    }
                    l->ansi.ansi_escape_len = 0;
                }
            }
        }
    }
    return result;
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
int linenoiseEditInsert(struct linenoiseState *l, int c) {
    if (c >= 32) {
        if (l->len < l->buflen) {
            if (l->len == l->pos) {
                l->buf[l->pos] = c;
                l->pos++;
                l->len++;
                l->buf[l->len] = '\0';
                if ((!mlmode && l->plen+l->len < l->cols) /* || mlmode */) {
                    /* Avoid a full update of the line in the
                     * trivial case. */
                    if (RETRY(write(l->fd,&c,1)) == -1) return -1;
                } else {
                    if (refreshLine(l) == -1) return -1;
                }
            } else {
                memmove(l->buf+l->pos+1,l->buf+l->pos,l->len-l->pos);
                l->buf[l->pos] = c;
                l->len++;
                l->pos++;
                l->buf[l->len] = '\0';
                if (refreshLine(l) == -1) return -1;
            }
            return 0;
        } else {
            if (ensureBufLen(l, l->len + 1) == -1) return -1;
            return linenoiseEditInsert(l, c);
        }
    } else {
        return 0;
    }
}

/* Cancel the line editing. */
int cancelInternal(struct linenoiseState *l)
{
    if (l->pos + 2 <= l->len) {
        l->buf[l->pos] = '^';
        l->buf[l->pos + 1] = 'C';
        l->pos = l->len;
        if (refreshLine(l) == -1) return -1;
    } else if (l->pos + 1 == l->len) {
        l->buf[l->pos] = '^';
        l->pos = l->len;
        if (refreshLine(l) == -1) return -1;
        if (linenoiseEditInsert(l, 'C') == -1) return -1;
    } else {
        if (linenoiseEditInsert(l, '^') == -1) return -1;
        if (linenoiseEditInsert(l, 'C') == -1) return -1;
    }

    l->len = 0;
    resetOnNewline(l);

    return 0;
}

/* Move cursor on the left. */
int linenoiseEditMoveLeft(struct linenoiseState *l) {
    if (l->pos > 0) {
        l->pos--;
        return refreshLine(l);
    }
    else
        return 0;
}

/* Move cursor on the right. */
int linenoiseEditMoveRight(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos++;
        return refreshLine(l);
    }
    else
        return 0;
}

/* Update current history entry from the current buffer. */
int linenoiseEditUpdateHistoryEntry(struct linenoiseState *l) {
    if (history_len > 1) {
        free(history[history_len - 1 - l->history_index]);
        history[history_len - 1 - l->history_index] = strdup(l->buf);
        if (history[history_len - 1 - l->history_index] == NULL) {
            errno = ENOMEM;
            return -1;
        }
    }
    return 0;
}

/* Show history entry. */
int linenoiseShowHistoryEntry(struct linenoiseState *l, int index, size_t pos) {
    l->history_index = index;
    if (l->history_index < 0) {
        l->history_index = 0;
        return 0;
    } else if (l->history_index >= history_len) {
        l->history_index = history_len - 1;
        return 0;
    }
    size_t hist_strlen = strlen(history[history_len - 1 - l->history_index]);
    if (ensureBufLen(l, hist_strlen) == -1) return -1;
    memcpy(l->buf, history[history_len - 1 - l->history_index], hist_strlen+1);
    l->len = strlen(l->buf);
    l->pos = MIN(l->len, pos);
    return refreshLine(l);

}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
int linenoiseEditHistoryNext(struct linenoiseState *l, int dir) {
    if (history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        if (linenoiseEditUpdateHistoryEntry(l) == -1) return -1;

        /* Show the new entry */
        int new_index = l->history_index + ((dir == LINENOISE_HISTORY_PREV) ? 1 : -1);
        return linenoiseShowHistoryEntry(l, new_index, SIZE_MAX);
    }
    else
        return 0;
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
int linenoiseEditDelete(struct linenoiseState *l) {
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf+l->pos,l->buf+l->pos+1,l->len-l->pos-1);
        l->len--;
        l->buf[l->len] = '\0';
        return refreshLine(l);
    } else return 0;
}

/* Backspace implementation. */
int linenoiseEditBackspace(struct linenoiseState *l) {
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf+l->pos-1,l->buf+l->pos,l->len-l->pos);
        l->pos--;
        l->len--;
        l->buf[l->len] = '\0';
        return refreshLine(l);
    } else return 0;
}

/* Delete the previosu word, maintaining the cursor at the start of the
 * current word. */
int linenoiseEditDeletePrevWord(struct linenoiseState *l) {
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos--;
    while (l->pos > 0 && l->buf[l->pos-1] != ' ')
        l->pos--;
    diff = old_pos - l->pos;
    memmove(l->buf+l->pos,l->buf+old_pos,l->len-old_pos+1);
    l->len -= diff;
    return refreshLine(l);
}

/* Reset state to readling new line. */
int resetState(struct linenoiseState *l)
{
    if (l->state != LS_NEW_LINE) {
        if (l->state == LS_COMPLETION) {
            freeCompletions(l);
        } else if (l->state == LS_HISTORY_SEARCH) {
            if (freeHistorySearch(l) == -1) return -1;
        }

        history_len--;
        free(history[history_len]);

        l->state = LS_NEW_LINE;
    }
    return 0;
}

/* Set closed state. */
int setClosed(struct linenoiseState *l)
{
    l->is_closed = true;
    return resetState(l);
}

/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns one of the LinenoiseResult values to indicate the
 * line editing result. */
static enum LinenoiseResult linenoiseEdit(struct linenoiseState *l)
{
    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    if (l->state == LS_NEW_LINE) {
        /* Buffer starts empty. */
        if (l->pos != 0 || l->len != 0) {
            l->pos = l->len = 0;
            l->buf[0] = '\0';
            l->needs_refresh = true;
        }

        linenoiseHistoryAdd("");
        l->history_index = 0;
        l->state = LS_READ;
    }

    if (l->needs_refresh || !l->is_displayed)
        if (refreshLine(l) == -1) return LR_ERROR;

    int c = readChar(l);

    if (c == RCS_CLOSED) {
        if (setClosed(l) == -1) return LR_ERROR;
        return LR_CLOSED;
    }
    else if (c == RCS_ERROR) {
        return LR_ERROR;
    }

    switch(c) {
    case 13:    /* enter */
        if (resetState(l) == -1) return LR_ERROR;
        return LR_HAVE_TEXT;
    case RCS_CANCELLED:
    case 3:     /* ctrl-c */
    {
        bool doCancel = (l->len == 0);
        if (cancelInternal(l) == -1) return LR_ERROR;
        if (resetState(l) == -1) return LR_ERROR;
        if (doCancel)
            return LR_CANCELLED;
        else {
            if (printf("\r\n") < 0) return LR_ERROR;
            return LR_CONTINUE;
        }
    }
    case 127:   /* backspace */
    case 8:     /* ctrl-h */
        if (linenoiseEditBackspace(l) == -1) return LR_ERROR;
        break;
    case 4:     /* ctrl-d, remove char at right of cursor, or of the
                   line is empty, act as end-of-file. */
        if (l->len > 0) {
            if (linenoiseEditDelete(l) == -1) return LR_ERROR;
        } else {
            if (setClosed(l) == -1) return LR_ERROR;
            return LR_CLOSED;
        }
        break;
    case 20:    /* ctrl-t, swaps current character with previous. */
        if (l->pos > 0 && l->pos < l->len) {
            int aux = l->buf[l->pos-1];
            l->buf[l->pos-1] = l->buf[l->pos];
            l->buf[l->pos] = aux;
            if (l->pos != l->len-1) l->pos++;
            if (refreshLine(l) == -1) return LR_ERROR;
        }
        break;
    case 2:     /* ctrl-b */
        if (linenoiseEditMoveLeft(l) == -1) return LR_ERROR;
        break;
    case 6:     /* ctrl-f */
        if (linenoiseEditMoveRight(l) == -1) return LR_ERROR;
        break;
    case 16:    /* ctrl-p */
        if (linenoiseEditHistoryNext(l, LINENOISE_HISTORY_PREV) == -1) return LR_ERROR;
        break;
    case 14:    /* ctrl-n */
        if (linenoiseEditHistoryNext(l, LINENOISE_HISTORY_NEXT) == -1) return LR_ERROR;
        break;
    case RCS_ANSI_CURSOR_LEFT:
        if (linenoiseEditMoveLeft(l) == -1) return LR_ERROR;
        break;
    case RCS_ANSI_CURSOR_RIGHT:
        if (linenoiseEditMoveRight(l) == -1) return LR_ERROR;
        break;
    case RCS_ANSI_CURSOR_UP:
    case RCS_ANSI_CURSOR_DOWN:
        if (linenoiseEditHistoryNext(l,
                c == RCS_ANSI_CURSOR_UP ? LINENOISE_HISTORY_PREV :
                                          LINENOISE_HISTORY_NEXT) == -1) return LR_ERROR;
        break;
    case RCS_ANSI_DELETE:
        if (linenoiseEditDelete(l) == -1) return LR_ERROR;
        break;
    case RCS_ANSI_HOME:
        l->pos = 0;
        if (refreshLine(l) == -1) return LR_ERROR;
        break;
    case RCS_ANSI_END:
        l->pos = l->len;
        if (refreshLine(l) == -1) return LR_ERROR;
        break;
    default:
        if (linenoiseEditInsert(l,c) == -1) return LR_ERROR;
        break;
    case 21: /* Ctrl+u, delete the whole line. */
        l->buf[0] = '\0';
        l->pos = l->len = 0;
        if (refreshLine(l) == -1) return LR_ERROR;
        break;
    case 11: /* Ctrl+k, delete from current to end of line. */
        l->buf[l->pos] = '\0';
        l->len = l->pos;
        if (refreshLine(l) == -1) return LR_ERROR;
        break;
    case 1: /* Ctrl+a, go to the start of the line */
        l->pos = 0;
        if (refreshLine(l) == -1) return LR_ERROR;
        break;
    case 5: /* ctrl+e, go to the end of the line */
        l->pos = l->len;
        if (refreshLine(l) == -1) return LR_ERROR;
        break;
    case 12: /* ctrl+l, clear screen */
        if (clearScreen(l) == -1) return LR_ERROR;
        if (refreshLine(l) == -1) return LR_ERROR;
        break;
    case 23: /* ctrl+w, delete previous word */
        if (linenoiseEditDeletePrevWord(l) == -1) return LR_ERROR;
        break;
    case 9:  /* tab, complete */
        /* Only autocomplete when the callback is set. It returns < 0 when
         * there was an error reading from fd. Otherwise it will return the
         * character that should be handled next. */
        if (completionCallback != NULL) {
            pushFrontChar(l, c);
            l->state = LS_COMPLETION;
            return LR_CONTINUE;
        } else {
            if (linenoiseEditInsert(l,c) == -1) return LR_ERROR;
        }
        break;
    case 18: /* ctrl+r, search the history */
        if (linenoiseEditUpdateHistoryEntry(l) == -1) return -1;
        pushFrontChar(l, c);
        l->state = LS_HISTORY_SEARCH;
        return LR_CONTINUE;
    }

    return LR_CONTINUE;
}

/* Handle TAB completion.
 * Returns one of the LinenoiseResult values to indicate the completion result. */
static enum LinenoiseResult linenoiseCompletion(struct linenoiseState *l) {
    int c = readChar(l);

    switch (c) {
    case 3:
    case RCS_CANCELLED:
    case RCS_CLOSED:
    default:
        // Let the normal processing to do its job
        freeCompletions(l);
        pushFrontChar(l, c);
        l->state = LS_READ;
        return LR_CONTINUE;
    case RCS_ERROR:
        return LR_ERROR;
    case 9:
        {
            bool wasInitialized = l->comp.is_initialized;
            if (!wasInitialized || l->comp.len == 1) {
                wasInitialized = false;
                freeCompletions(l);

                completionCallback(l->buf, l->pos, &l->comp);

                // completion might call linenoiseCustomOutput()
                if (enableRawMode(l->fd) == -1) return LR_ERROR;

                if (l->comp.len > 0 && l->comp.cvec == NULL) {
                    errno = ENOMEM;
                    return LR_ERROR;
                }
                l->comp.is_initialized = true;
            }

            if (wasInitialized || l->comp.len < 2)
                completeLine(l);
            return LR_CONTINUE;
        }
    }
}

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static int freeHistorySearch(struct linenoiseState *l) {
    if (l->hist_search.hist_search_buf != NULL) {
        free(l->hist_search.hist_search_buf);
        l->hist_search.hist_search_buf = NULL;
        l->hist_search.hist_search_len = l->hist_search.hist_search_buflen = 0;
        l->hist_search.current_index = 0;
        l->hist_search.found = false;
        if (setTempPrompt(l, NULL) == -1) return -1;
    }
    return 0;
}

/* Sets the temporary history searching prompt. */
int setSearchPrompt(struct linenoiseState *l)
{
    size_t promptlen = l->hist_search.hist_search_len + 23;
    char* newprompt = calloc(1, promptlen);
    if (newprompt == NULL) {
        errno = ENOMEM;
        return -1;
    }
    snprintf(newprompt, promptlen, "(reverse-i-search`%s'): ", l->hist_search.hist_search_buf);
    newprompt[promptlen-1] = '\0';
    return setTempPrompt(l, newprompt);
}

/* Find entry in history matching the current sequence */
int linenoiseHistoryFindEntry(struct linenoiseState *l)
{
    if (l->hist_search.hist_search_len > 0) {
        int new_index = l->hist_search.current_index;
        while (new_index < history_len) {
            char *historyStr = history[history_len - 1 - new_index];
            char *found = strstr(historyStr, l->hist_search.hist_search_buf);
            char *last_found = NULL;
            while (found != NULL) {
                last_found = found;
                found = strstr(last_found + 1, l->hist_search.hist_search_buf);
            }
            if (last_found != NULL) {
                if (setSearchPrompt(l) == -1) return -1;
                l->hist_search.found = true;
                if (linenoiseShowHistoryEntry(l, new_index,
                        last_found - historyStr
                                + l->hist_search.hist_search_len) == -1) return -1;
                l->hist_search.current_index = l->history_index;
                return 0;
            }
            new_index++;
        }
        linenoiseBeep();
    }
    l->hist_search.found = false;
    return 0;
}

/* Handle the CTRL+R history searching.
 * Returns one of the LinenoiseResult values to indicate the searching result. */
static enum LinenoiseResult linenoiseHistorySearch(struct linenoiseState *l)
{
    int c = readChar(l);

    switch (c) {
    case 3:
    case RCS_CANCELLED:
        if (cancelInternal(l) == -1) return LR_ERROR;
        if (printf("\r\n") < 0) return LR_ERROR;
        if (resetState(l) == -1) return LR_ERROR;
        return LR_CONTINUE;
    case RCS_CLOSED:
        // Let the normal processing to do its job
        if (freeHistorySearch(l) == -1) return LR_ERROR;
        pushFrontChar(l, c);
        l->state = LS_READ;
        return LR_CONTINUE;
    case RCS_ERROR:
        return LR_ERROR;
    case 127:   /* backspace */
    case 8:     /* ctrl-h */
        if (l->hist_search.hist_search_len > 0) {
            l->hist_search.hist_search_len--;
            l->hist_search.hist_search_buf[l->hist_search.hist_search_len] = '\0';
            if (l->hist_search.hist_search_len == 0 && l->hist_search.found) {
                if (setSearchPrompt(l) == -1) return LR_ERROR;
                if (refreshLine(l) == -1) return LR_ERROR;
            } else {
                if (linenoiseHistoryFindEntry(l) == -1) return LR_ERROR;
            }
        }
        return LR_CONTINUE;
    default:
        if (c >= 32) {
            size_t newlen = l->hist_search.hist_search_len + 1;
            if (newlen >= l->hist_search.hist_search_buflen) {
                size_t newsize = l->hist_search.hist_search_buflen + LINENOISE_LINE_INIT_MAX_AND_GROW;
                char *newbuf = realloc(l->hist_search.hist_search_buf, newsize);
                if (newbuf == NULL) {
                    errno = ENOMEM;
                    return LR_ERROR;
                }
            }
            l->hist_search.hist_search_buf[l->hist_search.hist_search_len++] = c;
            l->hist_search.hist_search_buf[l->hist_search.hist_search_len] = '\0';
            // Find history entry
            bool wasFound = l->hist_search.found;
            if (linenoiseHistoryFindEntry(l) == -1) return LR_ERROR;
            if (wasFound && !l->hist_search.found)
                l->hist_search.current_index++;
            return LR_CONTINUE;
        } else {
            // Cancel
            if (freeHistorySearch(l) == -1) return LR_ERROR;
            pushFrontChar(l, c);
            l->state = LS_READ;
            return LR_CONTINUE;
        }
    case 18:
        if (l->hist_search.hist_search_buf == NULL) {
            // First search
            if (history_len == 1) {
                linenoiseBeep();
                l->state = LS_READ;
                return LR_CONTINUE;
            }
            l->hist_search.current_index = l->history_index;
            l->hist_search.hist_search_buf = calloc(1, LINENOISE_LINE_INIT_MAX_AND_GROW);
            if (l->hist_search.hist_search_buf == NULL) {
                errno = ENOMEM;
                return LR_ERROR;
            }
            l->hist_search.hist_search_buflen = LINENOISE_LINE_INIT_MAX_AND_GROW;
            l->hist_search.hist_search_len = 0;
            if (setSearchPrompt(l) == -1) return LR_ERROR;
            if (refreshLine(l) == -1) return LR_ERROR;
            return LR_CONTINUE;
        } else if (l->hist_search.hist_search_len > 0) {
            // Find another history entry
            if (l->hist_search.found) {
                l->hist_search.current_index++;
                if (linenoiseHistoryFindEntry(l) == -1) return LR_ERROR;
            } else {
                linenoiseBeep();
            }
            return LR_CONTINUE;
        } else {
            // Do nothing - no string to search for
            return LR_CONTINUE;
        }
    }
}

/* This function calls the line editing function corresponding to the state,
 * it is either linenoiseEdit(), linenoiseCompletion() or
 * linenoiseHistorySearch(). The terminal is put to raw mode. */
static enum LinenoiseResult linenoiseRaw(struct linenoiseState *l) {
    if (l->is_closed) {
        return LR_CLOSED;
    } else if (l->buflen == 0) {
        errno = EINVAL;
        return LR_ERROR;
    }

    if (enableRawMode(l->fd) == -1) return LR_ERROR;

    bool gotBlocked = blockSignals(l);

    enum LinenoiseResult result = LR_CONTINUE;
    while (result == LR_CONTINUE) {
        switch (l->state) {
        case LS_NEW_LINE:
        case LS_READ:
        default:
            result = linenoiseEdit(l);
            break;
        case LS_COMPLETION:
            result = linenoiseCompletion(l);
            break;
        case LS_HISTORY_SEARCH:
            result = linenoiseHistorySearch(l);
            break;
        }
    }

    int savederrno = errno;

    if (!l->is_async) disableRawMode(l->fd);
    if (gotBlocked) revertSignals(l);

    errno = savederrno;
    return result;
}

/* Ensure the fields are initialized. */
int ensureInitialized(struct linenoiseState *l)
{
    if (isUnsupportedTerm()) {
        errno = EBADF;
        return -1;
    } else {
        if ( !initialized )
        {
            if (initialize(l) == -1) return -1;
            initialized = true;
        }
        else
        {
            linenoiseUpdateSize();
        }

        int flagsRead = fcntl(STDIN_FILENO, F_GETFL, 0);
        l->is_async = (flagsRead & O_NONBLOCK) != 0;
        return 0;
    }
}

int linenoiseSetPrompt(const char *prompt)
{
    if (ensureInitialized(&state) == -1) return -1;
    return setPrompt(&state, prompt);
}

/* Show the prompt. */
int linenoiseShowPrompt()
{
    if (ensureInitialized(&state) == -1) return -1;
    if (enableRawMode(state.fd) == -1) return -1;
    int result = 0;
    if (state.needs_refresh || !state.is_displayed)
        result = refreshLine(&state);
    if (result == -1 || !state.is_async)
        disableRawMode(state.fd);
    return result;
}

/* Check if there is anything to process. */
int linenoiseHasPendingChar()
{
    return state.read_back_char_len != 0 || state.is_cancelled;
}

int linenoiseCleanup()
{
	if (prepareCustomOutputOnNewLine(&state) == -1) return -1;
	disableRawMode(state.fd);
	return 0;
}

/* The high level function that is the main API of the linenoise library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *linenoise() {
    if (isUnsupportedTerm()) {
        char buf[LINENOISE_LINE_INIT_MAX_AND_GROW];
        size_t len;

        printf("%s",state.prompt);
        fflush(stdout);
        if (fgets(buf,LINENOISE_LINE_INIT_MAX_AND_GROW,stdin) == NULL) return NULL;
        len = strlen(buf);
        while(len && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            len--;
            buf[len] = '\0';
        }
        return strdup(buf);
    } else {
        int saved_errno = errno;

        if (ensureInitialized(&state) == -1) return NULL;

        enum LinenoiseResult result = linenoiseRaw(&state);

        if (result == LR_CANCELLED) {
            resetOnNewline(&state);
            printf("\r\n");
            errno = EINTR;
            return NULL;
        } else if (result == LR_CLOSED && state.len == 0) {
            resetOnNewline(&state);
            printf("\r\n");
            errno = 0;
            return NULL;
        } else if (result == LR_ERROR) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                resetOnNewline(&state);
                printf("\r\n");
                if (state.is_async) {
                    disableRawMode(state.fd);
                }
            }
            return NULL;
        } else if (result == LR_HAVE_TEXT) {
            resetOnNewline(&state);
            printf("\r\n");
        }

        // Have some text
        char* copy = strndup(state.buf, state.len);

        state.pos = state.len = 0;
        state.buf[0] = '\0';

        if (copy == NULL) errno = ENOMEM;
        else errno = saved_errno;

        return copy;
    }
}

/* Cancel the current line editing. */
void linenoiseCancel() {
    state.is_cancelled = true;
}

/* Update the size. */
void linenoiseUpdateSize() {
    size_t newCols = getColumns();
    if ( newCols != state.cols )
    {
        state.cols = newCols;
        state.needs_refresh = true;
    }
}

/* Initialize the state */
static int initialize(struct linenoiseState *l)
{
    atexit(linenoiseAtExit);

    struct sigevent ev = {sigev_notify: SIGEV_SIGNAL, sigev_signo: SIGALRM };
    if (timer_create(CLOCK_MONOTONIC, &ev, &l->ansi.ansi_timer) == -1)
        return -1;

    /* Populate the linenoise state that we pass to functions implementing
     * specific editing functionalities. */
    l->buf = calloc(1, LINENOISE_LINE_INIT_MAX_AND_GROW);
    if (l->buf == NULL) {
        errno = ENOMEM;
        return -1;
    }

    l->buflen = LINENOISE_LINE_INIT_MAX_AND_GROW;
    l->cols = getColumns();

    return 0;
}

/* Free the state. */
static void freeState()
{
    timer_delete(state.ansi.ansi_timer);
    free(state.prompt);
    free(state.buf);
    freeHistorySearch(&state);
    freeCompletions(&state);
}

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void freeHistory(void) {
    if (history) {
        int j;

        for (j = 0; j < history_len; j++)
            free(history[j]);
        free(history);
    }
}

/* At exit we'll try to fix the terminal to the initial conditions. */
static void linenoiseAtExit(void) {
    disableRawMode(STDIN_FILENO);
    freeHistory();
    freeState();
}

/* Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line) {
    char *linecopy;

    if (history_max_len == 0) return 0;
    if (history == NULL) {
        history = malloc(sizeof(char*)*history_max_len);
        if (history == NULL) return 0;
        memset(history,0,(sizeof(char*)*history_max_len));
    }
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history,history+1,sizeof(char*)*(history_max_len-1));
        history_len--;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(int len) {
    char **new;

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;

        new = malloc(sizeof(char*)*len);
        if (new == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {
            int j;

            for (j = 0; j < tocopy-len; j++) free(history[j]);
            tocopy = len;
        }
        memset(new,0,sizeof(char*)*len);
        memcpy(new,history+(history_len-tocopy), sizeof(char*)*tocopy);
        free(history);
        history = new;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(char *filename) {
    FILE *fp = fopen(filename,"w");
    int j;
    
    if (fp == NULL) return -1;
    for (j = 0; j < history_len; j++)
        fprintf(fp,"%s\n",history[j]);
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(char *filename) {
    FILE *fp = fopen(filename,"r");
    char buf[LINENOISE_LINE_INIT_MAX_AND_GROW];
    
    if (fp == NULL) return -1;

    while (fgets(buf,LINENOISE_LINE_INIT_MAX_AND_GROW,fp) != NULL) {
        char *p;
        
        p = strchr(buf,'\r');
        if (!p) p = strchr(buf,'\n');
        if (p) *p = '\0';
        linenoiseHistoryAdd(buf);
    }
    fclose(fp);
    return 0;
}
