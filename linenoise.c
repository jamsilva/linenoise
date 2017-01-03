/* linenoise.c -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 *
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
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
 * When linenoiseClearScreen() is called, two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "linenoise.h"

#define LINENOISE_MAX_LINE 4096
#define UNUSED(x) (void)(x)
static char *unsupported_term[] = {"dumb","cons25","emacs",NULL};
static linenoiseCompletionCallback *completionCallback = NULL;

static struct termios orig_termios; /* In order to restore at exit.*/
static int rawmode = 0; /* For atexit() function to check if restore is needed*/
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int atexit_registered = 0; /* Register atexit just 1 time. */
static linenoiseHistoryCallback *historyCallback = NULL;

static int INPUT_FD = STDIN_FILENO;
static int OUTPUT_FD = STDOUT_FILENO;
static int ERROR_FD = STDERR_FILENO;


/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
    int ifd;            /* Terminal stdin file descriptor. */
    int ofd;            /* Terminal stdout file descriptor. */
    char *buf;          /* Edited line buffer. */
    size_t buflen;      /* Edited line buffer size. */
    const char *prompt; /* Prompt to display. */
    size_t plen;        /* Prompt length. */
    size_t pos;         /* Current cursor position. */
    size_t oldcolpos;   /* Previous refresh cursor column position. */
    size_t len;         /* Current edited line length. */
    size_t cols;        /* Number of columns in terminal. */
    size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
    int history_index;  /* The history index we are currently editing. */
};

enum KEY_ACTION{
	KEY_NULL = 0,	    /* NULL */
	CTRL_A = 1,         /* Ctrl+a */
	CTRL_B = 2,         /* Ctrl-b */
	CTRL_C = 3,         /* Ctrl-c */
	CTRL_D = 4,         /* Ctrl-d */
	CTRL_E = 5,         /* Ctrl-e */
	CTRL_F = 6,         /* Ctrl-f */
	CTRL_H = 8,         /* Ctrl-h */
	TAB = 9,            /* Tab */
	CTRL_K = 11,        /* Ctrl+k */
	CTRL_L = 12,        /* Ctrl+l */
	ENTER = 13,         /* Enter */
	CTRL_N = 14,        /* Ctrl-n */
	CTRL_P = 16,        /* Ctrl-p */
	CTRL_T = 20,        /* Ctrl-t */
	CTRL_U = 21,        /* Ctrl+u */
	CTRL_W = 23,        /* Ctrl+w */
	ESC = 27,           /* Escape */
	BACKSPACE =  127    /* Backspace */
};

static void linenoiseAtExit(void);
int linenoiseEditInsert(struct linenoiseState *l, const char *cbuf, int clen);
static void refreshLine(struct linenoiseState *l);

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (lndebug_fp == NULL) { \
            lndebug_fp = fopen("/tmp/lndebug.txt","a"); \
            fprintf(lndebug_fp, \
            "[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d\n", \
            (int)l->len,(int)l->pos,(int)l->oldcolpos,plen,rows,rpos, \
            (int)l->maxrows,old_rows); \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(fmt, ...)
#endif

/* ========================== Encoding functions ============================= */

/* Get byte length and column length of the previous character */
static size_t defaultPrevCharLen(const char *buf, size_t buf_len, size_t pos, size_t *col_len) {
    UNUSED(buf); UNUSED(buf_len); UNUSED(pos);
    if (col_len != NULL) *col_len = 1;
    return 1;
}

/* Get byte length and column length of the next character */
static size_t defaultNextCharLen(const char *buf, size_t buf_len, size_t pos, size_t *col_len) {
    UNUSED(buf); UNUSED(buf_len); UNUSED(pos);
    if (col_len != NULL) *col_len = 1;
    return 1;
}

/* Read bytes of the next character */
static size_t defaultReadCode(int fd, char *buf, size_t buf_len, int* c) {
    if (buf_len < 1) return -1;
    int nread = read(fd,&buf[0],1);
    if (nread == 1) *c = buf[0];
    return nread;
}

/* Set default encoding functions */
static linenoisePrevCharLen *prevCharLen = defaultPrevCharLen;
static linenoiseNextCharLen *nextCharLen = defaultNextCharLen;
static linenoiseReadCode *readCode = defaultReadCode;
static linenoiseStrLen *strLen = strlen;

/* Set used defined encoding functions */
void linenoiseSetEncodingFunctions(
    linenoisePrevCharLen *prevCharLenFunc,
    linenoiseNextCharLen *nextCharLenFunc,
    linenoiseReadCode *readCodeFunc,
    linenoiseStrLen *strLenFunc) {
    prevCharLen = prevCharLenFunc;
    nextCharLen = nextCharLenFunc;
    readCode = readCodeFunc;
    strLen = strLenFunc;
}

/* Get column length from begining of buffer to current byte position */
static size_t columnPos(const char *buf, size_t buf_len, size_t pos) {
    size_t ret = 0;
    size_t off = 0;
    while (off < pos) {
        size_t col_len;
        size_t len = nextCharLen(buf,buf_len,off,&col_len);
        off += len;
        ret += col_len;
    }
    return ret;
}

/* Get column length from begining of buffer to current byte position for multiline mode*/
static size_t columnPosForMultiLine(const char *buf, size_t buf_len, size_t pos, size_t cols, size_t ini_pos) {
    size_t ret = 0;
    size_t colwid = ini_pos;

    size_t off = 0;
    while (off < buf_len) {
        size_t col_len;
        size_t len = nextCharLen(buf,buf_len,off,&col_len);

        int dif = (int)(colwid + col_len) - (int)cols;
        if (dif > 0) {
            ret += dif;
            colwid = col_len;
        } else if (dif == 0) {
            colwid = 0;
        } else {
            colwid += col_len;
        }

        if (off >= pos) break;
        off += len;
        ret += col_len;
    }

    return ret;
}

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void linenoiseSetMultiLine(int ml) {
    mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int isUnsupportedTerm(void) {
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term,unsupported_term[j])) return 1;
    return 0;
}

/* Raw mode: 1960 magic shit. */
static int enableRawMode(int fd) {
    struct termios raw;

    if (!isatty(INPUT_FD)) goto fatal;
    if (!atexit_registered) {
        atexit(linenoiseAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char
     */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

static void disableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    if (rawmode && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
        rawmode = 0;
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
static int getCursorPosition(int ifd, int ofd) {
    char buf[32];
    int cols, rows;
    unsigned int i = 0;

    /* Report cursor location */
    if (write(ofd, "\x1b[6n", 4) != 4) return -1;

    /* Read the response: ESC [ rows ; cols R */
    while (i < sizeof(buf)-1) {
        if (read(ifd,buf+i,1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    /* Parse it. */
    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf+2,"%d;%d",&rows,&cols) != 2) return -1;
    return cols;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(int ifd, int ofd) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        int start, cols;

        /* Get the initial position so we can restore it later. */
        start = getCursorPosition(ifd,ofd);
        if (start == -1) goto failed;

        /* Go to right margin and get position. */
        if (write(ofd,"\x1b[999C",6) != 6) goto failed;
        cols = getCursorPosition(ifd,ofd);
        if (cols == -1) goto failed;

        /* Restore position. */
        if (cols > start) {
            char seq[32];
            snprintf(seq,32,"\x1b[%dD",cols-start);
            if (write(ofd,seq,strlen(seq)) == -1) {
                /* Can't recover... */
            }
        }
        return cols;
    } else {
        return ws.ws_col;
    }

failed:
    return 80;
}

/* Clear the screen. Used to handle ctrl+l */
void linenoiseClearScreen(void) {
    if (write(OUTPUT_FD,"\x1b[H\x1b[2J",7) <= 0) {
        /* nothing to do, just to avoid warning. */
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void linenoiseBeep(void) {
    write(ERROR_FD, "\x7", strlen("\x7"));
    fsync(ERROR_FD);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void freeCompletions(linenoiseCompletions *lc) {
    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);
    if (lc->cvec != NULL)
        free(lc->cvec);
}

#if 0
static FILE *logfp = NULL;
#define logprintf(fmt, ...) \
do { \
    if(logfp == NULL) {logfp = fopen("/dev/ttys001", "w"); } \
    fprintf(logfp, fmt, ## __VA_ARGS__);\
} while(0)
#else
#define logprintf(fmt, ...)
#endif

static void showAllCandidates(size_t cols, linenoiseCompletions *lc) {
    unsigned int *sizeTable = malloc(sizeof(unsigned int) * lc->len);

    // compute maximum length of candidate
    size_t maxSize = 0;
    size_t index;
    for(index = 0; index < lc->len; index++) {
        size_t s = strLen(lc->cvec[index]);
        if(s > maxSize) {
            maxSize = s;
        }
        sizeTable[index] = s;
    }

    maxSize += 2;
    const unsigned int columCount = cols / maxSize;

    logprintf("cols: %lu\n", cols);
    logprintf("maxSize: %lu\n", maxSize);
    logprintf("columCount: %u\n", columCount);

    // compute raw size
    size_t rawSize;
    for(rawSize = 1; rawSize < lc->len; rawSize++) {
        size_t a = lc->len / rawSize;
        size_t b = lc->len % rawSize;
        size_t c = b == 0 ? 0 : 1;
        if(a + c <= columCount) {
            break;
        }
    }

    logprintf("rawSize: %zu\n", rawSize);

    // show candidates
    write(OUTPUT_FD, "\r\n", strlen("\r\n"));
    for(index = 0; index < rawSize; index++) {
        size_t cadidateIndex = 0;
        size_t j = 0;
        while(1) {
            cadidateIndex = j * rawSize + index;
            if(cadidateIndex >= lc->len) {
                break;
            }

            // print candidate
            const char *c = lc->cvec[cadidateIndex];
            write(OUTPUT_FD, c, strlen(c));

            // print spaces
            unsigned int s;
            for(s = 0; s < maxSize - sizeTable[cadidateIndex]; s++) {
                write(OUTPUT_FD, " ", 1);
            }

            j++;
        }
        write(OUTPUT_FD, "\r\n", strlen("\r\n"));
    }

    free(sizeTable);
}

static char *computeCommonPrefix(const linenoiseCompletions *lc, size_t *len) {
    if(lc->len == 0) {
        *len = 0;
        return NULL;
    }
    if(lc->len == 1) {
        *len = strlen(lc->cvec[0]);
        return strdup(lc->cvec[0]);
    }

    size_t prefixSize;
    for(prefixSize = 0; ; prefixSize++) {
        int stop = 0;
        const char ch = lc->cvec[0][prefixSize];
        size_t i;
        for(i = 0; i < lc->len; i++) {
            const char *str = lc->cvec[i];
            if(str[0] == '\0' || prefixSize >= strlen(str) || ch != str[prefixSize]) {
                stop = 1;
                break;
            }
        }

        if(stop) {
            break;
        }
    }

    if(prefixSize == 0) {
        *len = 0;
        return NULL;
    }

    char *prefix = malloc(sizeof(char) * (prefixSize + 1));
    memcpy(prefix, lc->cvec[0], sizeof(char) * prefixSize);
    prefix[prefixSize] = '\0';
    *len = prefixSize;
    return prefix;
}

/**
 * return token start cursor.
 */
static size_t insertEstimatedSuffix(struct linenoiseState *ls, const linenoiseCompletions *lc) {
    size_t len;
    char *prefix = computeCommonPrefix(lc, &len);
    if(prefix == NULL) {
        return ls->pos;
    }

    logprintf("#prefix: %s\n", prefix);
    logprintf("pos: %ld\n", ls->pos);


    // compute suffix
    const char oldCh = ls->buf[ls->pos];
    ls->buf[ls->pos] = '\0';
    int matched = 0;

    size_t offset = 0;
    if(ls->pos > 0) {
        for(offset = ls->pos - 1; ; offset--) {
            const char *curStr = ls->buf + offset;
            logprintf("curStr: %s\n", curStr);
            const char *ptr = strstr(prefix, curStr);
            if(ptr == NULL) {
                offset++;
                break;
            }
            if(ptr == prefix) {
                matched = 1;
            }

            if(offset == 0) {
                break;
            }
        }
    }

    logprintf("offset: %ld\n", offset);
    if(matched) {
        size_t suffixSize = len - (ls->pos - offset);
        logprintf("suffix size: %ld\n", suffixSize);
        char *inserting = malloc(sizeof(char) * suffixSize);
        memcpy(inserting, prefix + (len - suffixSize), suffixSize);
        linenoiseEditInsert(ls, inserting, suffixSize);
        free(inserting);
    } else if(lc->len == 1) {   // if candidate dose not match previous token, insert it.
        linenoiseEditInsert(ls, prefix, len);
    }

    ls->buf[ls->pos] = oldCh;
    free(prefix);

    return matched ? offset : ls->pos;
}

/* This is an helper function for linenoiseEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition. */
static int completeLine(struct linenoiseState *ls, char cbuf[32]) {
    linenoiseCompletions lc = { 0, NULL };
    int nread;
    int c = 0;

    completionCallback(ls->buf, ls->pos, &lc);
    int offset = insertEstimatedSuffix(ls, &lc);
    if(lc.len == 0) {
        linenoiseBeep();
    } else if(lc.len == 1) {
        size_t csize = strlen(lc.cvec[0]);
        if(lc.cvec[0][csize - 1] != '/') {
            linenoiseEditInsert(ls, " ", 1);
        }
    } else {
        nread = readCode(ls->ifd, cbuf, 32, &c);
        if (nread <= 0) {
            c = -1;
            goto END;
        }
        if(c != TAB) {
            goto END;
        }

        int show = 1;
        if(lc.len >= 100) {
            char msg[256];
            snprintf(msg, 256, "\r\nDisplay all %zu possibilities? (y or n) ", lc.len);
            write(OUTPUT_FD, msg, strlen(msg));

            while(1) {
                nread = readCode(ls->ifd, cbuf, 32, &c);
                if (nread <= 0) {
                    c = -1;
                    goto END;
                }
                if(c == 'y') {
                    break;
                } else if(c == 'n') {
                    write(OUTPUT_FD, "\r\n", strlen("\r\n"));
                    show = 0;
                    break;
                } else if(c == CTRL_C) {
                    goto END;
                } else {
                    linenoiseBeep();
                }
            }
        }

        if(show) {
            showAllCandidates(ls->cols, &lc);
        }
        refreshLine(ls);

        // rotate candidates
        size_t rotateIndex = 0;
        while(1) {
            nread = readCode(ls->ifd, cbuf, 32, &c);
            if (nread <= 0) {
                c = -1;
                goto END;
            }
            if(c != TAB) {
                goto END;
            }

            int written = snprintf(ls->buf + offset, ls->buflen - offset, "%s", lc.cvec[rotateIndex]);
            if(written >= 0) {
                ls->len = ls->pos = offset + written;
            }
            refreshLine(ls);

            if(rotateIndex == lc.len - 1) {
                rotateIndex = 0;
                continue;
            }
            rotateIndex++;
        }
    }

    END:
    freeCompletions(&lc);
    return c; /* Return last read character */
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
    completionCallback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str) {
    size_t len = strlen(str);
    char *copy, **cvec;

    copy = malloc(len+1);
    if (copy == NULL) return;
    memcpy(copy,str,len+1);
    cvec = realloc(lc->cvec,sizeof(char*)*(lc->len+1));
    if (cvec == NULL) {
        free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
};

static void abInit(struct abuf *ab) {
    ab->b = NULL;
    ab->len = 0;
}

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = NULL;
    if (ab->len + len > 0) {
        new = realloc(ab->b,ab->len+len);
    }
    if (new == NULL) return;
    memcpy(new+ab->len,s,len);
    ab->b = new;
    ab->len += len;
}

static void abFree(struct abuf *ab) {
    free(ab->b);
}

/* Check if text is an ANSI escape sequence
 */
static int isAnsiEscape(const char *buf, size_t buf_len, size_t* len) {
    if (buf_len > 2 && !memcmp("\033[", buf, 2)) {
        size_t off = 2;
        while (off < buf_len) {
            switch (buf[off++]) {
            case 'A': case 'B': case 'C': case 'D': case 'E':
            case 'F': case 'G': case 'H': case 'J': case 'K':
            case 'S': case 'T': case 'f': case 'm':
                *len = off;
                return 1;
            }
        }
    }
    return 0;
}

/* Get column length of prompt text
 */
static size_t promptTextColumnLen(const char *prompt, size_t plen) {
    char buf[LINENOISE_MAX_LINE];
    size_t buf_len = 0;
    size_t off = 0;
    while (off < plen) {
        size_t len;
        if (isAnsiEscape(prompt + off, plen - off, &len)) {
            off += len;
            continue;
        }
        buf[buf_len++] = prompt[off++];
    }
    return columnPos(buf,buf_len,buf_len);
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshSingleLine(struct linenoiseState *l) {
    char seq[64];
    size_t pcollen = promptTextColumnLen(l->prompt,strlen(l->prompt));
    int fd = l->ofd;
    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    struct abuf ab;

    while((pcollen+columnPos(buf,len,pos)) >= l->cols) {
        int chlen = nextCharLen(buf,len,0,NULL);
        buf += chlen;
        len -= chlen;
        pos -= chlen;
    }
    while (pcollen+columnPos(buf,len,len) > l->cols) {
        len -= prevCharLen(buf,len,len,NULL);
    }

    abInit(&ab);
    /* Cursor to left edge */
    snprintf(seq,64,"\r");
    abAppend(&ab,seq,strlen(seq));
    /* Write the prompt and the current buffer content */
    abAppend(&ab,l->prompt,strlen(l->prompt));
    abAppend(&ab,buf,len);
    /* Erase to right */
    snprintf(seq,64,"\x1b[0K");
    abAppend(&ab,seq,strlen(seq));
    /* Move cursor to original position. */
    snprintf(seq,64,"\r\x1b[%dC", (int)(columnPos(buf,len,pos)+pcollen));
    abAppend(&ab,seq,strlen(seq));
    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    abFree(&ab);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshMultiLine(struct linenoiseState *l) {
    char seq[64];
    size_t pcollen = promptTextColumnLen(l->prompt,strlen(l->prompt));
    int colpos = columnPosForMultiLine(l->buf, l->len, l->len, l->cols, pcollen);
    int colpos2; /* cursor column position. */
    int rows = (pcollen+colpos+l->cols-1)/l->cols; /* rows used by current buf. */
    int rpos = (pcollen+l->oldcolpos+l->cols)/l->cols; /* cursor relative row. */
    int rpos2; /* rpos after refresh. */
    int col; /* colum position, zero-based. */
    int old_rows = l->maxrows;
    int fd = l->ofd, j;
    struct abuf ab;

    /* Update maxrows if needed. */
    if (rows > (int)l->maxrows) l->maxrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    abInit(&ab);
    if (old_rows-rpos > 0) {
        lndebug("go down %d", old_rows-rpos);
        snprintf(seq,64,"\x1b[%dB", old_rows-rpos);
        abAppend(&ab,seq,strlen(seq));
    }

    /* Now for every row clear it, go up. */
    for (j = 0; j < old_rows-1; j++) {
        lndebug("clear+up");
        snprintf(seq,64,"\r\x1b[0K\x1b[1A");
        abAppend(&ab,seq,strlen(seq));
    }

    /* Clean the top line. */
    lndebug("clear");
    snprintf(seq,64,"\r\x1b[0K");
    abAppend(&ab,seq,strlen(seq));

    /* Write the prompt and the current buffer content */
    abAppend(&ab,l->prompt,strlen(l->prompt));
    abAppend(&ab,l->buf,l->len);

    /* Get column length to cursor position */
    colpos2 = columnPosForMultiLine(l->buf,l->len,l->pos,l->cols,pcollen);

    /* If we are at the very end of the screen with our prompt, we need to
     * emit a newline and move the prompt to the first column. */
    if (l->pos &&
        l->pos == l->len &&
        (colpos2+pcollen) % l->cols == 0)
    {
        lndebug("<newline>");
        abAppend(&ab,"\n",1);
        snprintf(seq,64,"\r");
        abAppend(&ab,seq,strlen(seq));
        rows++;
        if (rows > (int)l->maxrows) l->maxrows = rows;
    }

    /* Move cursor to right position. */
    rpos2 = (pcollen+colpos2+l->cols)/l->cols; /* current cursor relative row. */
    lndebug("rpos2 %d", rpos2);

    /* Go up till we reach the expected positon. */
    if (rows-rpos2 > 0) {
        lndebug("go-up %d", rows-rpos2);
        snprintf(seq,64,"\x1b[%dA", rows-rpos2);
        abAppend(&ab,seq,strlen(seq));
    }

    /* Set column. */
    col = (pcollen + colpos2) % l->cols;
    lndebug("set col %d", 1+col);
    if (col)
        snprintf(seq,64,"\r\x1b[%dC", col);
    else
        snprintf(seq,64,"\r");
    abAppend(&ab,seq,strlen(seq));

    lndebug("\n");
    l->oldcolpos = colpos2;

    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    abFree(&ab);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void refreshLine(struct linenoiseState *l) {
    if (mlmode)
        refreshMultiLine(l);
    else
        refreshSingleLine(l);
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
int linenoiseEditInsert(struct linenoiseState *l, const char *cbuf, int clen) {
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            memcpy(&l->buf[l->pos],cbuf,clen);
            l->pos+=clen;
            l->len+=clen;;
            l->buf[l->len] = '\0';
            if ((!mlmode && promptTextColumnLen(l->prompt,l->plen)+columnPos(l->buf,l->len,l->len) < l->cols) /* || mlmode */) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                if (write(l->ofd,cbuf,clen) == -1) return -1;
            } else {
                refreshLine(l);
            }
        } else {
            memmove(l->buf+l->pos+clen,l->buf+l->pos,l->len-l->pos);
            memcpy(&l->buf[l->pos],cbuf,clen);
            l->pos+=clen;
            l->len+=clen;
            l->buf[l->len] = '\0';
            refreshLine(l);
        }
    }
    return 0;
}

/* Move cursor on the left. */
void linenoiseEditMoveLeft(struct linenoiseState *l) {
    if (l->pos > 0) {
        l->pos -= prevCharLen(l->buf,l->len,l->pos,NULL);
        refreshLine(l);
    }
}

/* Move cursor on the right. */
void linenoiseEditMoveRight(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos += nextCharLen(l->buf,l->len,l->pos,NULL);
        refreshLine(l);
    }
}

/* Move cursor to the start of the line. */
void linenoiseEditMoveHome(struct linenoiseState *l) {
    if (l->pos != 0) {
        l->pos = 0;
        refreshLine(l);
    }
}

/* Move cursor to the end of the line. */
void linenoiseEditMoveEnd(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos = l->len;
        refreshLine(l);
    }
}

void linenoiseSetHistoryCallback(linenoiseHistoryCallback *callback) {
    historyCallback = callback;
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
void linenoiseEditHistoryNext(struct linenoiseState *l, int dir) {
    historyOp op = dir == LINENOISE_HISTORY_NEXT ? LINENOISE_HISTORY_OP_NEXT : LINENOISE_HISTORY_OP_PREV;
    const char *ret = historyCallback(l->buf, &l->history_index, op);
    if(ret) {
        strncpy(l->buf, ret, l->buflen);
        l->buf[l->buflen-1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

static void doHistoryOp(historyOp op) {
    historyCallback(NULL, NULL, op);
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
void linenoiseEditDelete(struct linenoiseState *l) {
    if (l->len > 0 && l->pos < l->len) {
        int chlen = nextCharLen(l->buf,l->len,l->pos,NULL);
        memmove(l->buf+l->pos,l->buf+l->pos+chlen,l->len-l->pos-chlen);
        l->len-=chlen;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Backspace implementation. */
void linenoiseEditBackspace(struct linenoiseState *l) {
    if (l->pos > 0 && l->len > 0) {
        int chlen = prevCharLen(l->buf,l->len,l->pos,NULL);
        memmove(l->buf+l->pos-chlen,l->buf+l->pos,l->len-l->pos);
        l->pos-=chlen;
        l->len-=chlen;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previosu word, maintaining the cursor at the start of the
 * current word. */
void linenoiseEditDeletePrevWord(struct linenoiseState *l) {
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos--;
    while (l->pos > 0 && l->buf[l->pos-1] != ' ')
        l->pos--;
    diff = old_pos - l->pos;
    memmove(l->buf+l->pos,l->buf+old_pos,l->len-old_pos+1);
    l->len -= diff;
    refreshLine(l);
}

/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */
static int linenoiseEdit(int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt)
{
    struct linenoiseState l;

    /* Populate the linenoise state that we pass to functions implementing
     * specific editing functionalities. */
    l.ifd = stdin_fd;
    l.ofd = stdout_fd;
    l.buf = buf;
    l.buflen = buflen;
    l.prompt = prompt;
    l.plen = strlen(prompt);
    l.oldcolpos = l.pos = 0;
    l.len = 0;
    l.cols = getColumns(stdin_fd, stdout_fd);
    l.maxrows = 0;
    l.history_index = 0;

    /* Buffer starts empty. */
    l.buf[0] = '\0';
    l.buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
//    linenoiseHistoryAdd("");
    doHistoryOp(LINENOISE_HISTORY_OP_INIT);

    if (write(l.ofd,prompt,l.plen) == -1) return -1;
    while(1) {
        int c;
        char cbuf[32]; // large enough for any encoding?
        int nread;
        char seq[3];

        nread = readCode(l.ifd,cbuf,sizeof(cbuf),&c);
        if (nread <= 0) return l.len;

        /* Only autocomplete when the callback is set. It returns < 0 when
         * there was an error reading from fd. Otherwise it will return the
         * character that should be handled next. */
        if (c == 9 && completionCallback != NULL) {
            c = completeLine(&l, cbuf);
            /* Return on errors */
            if (c < 0) return l.len;
            /* Read next character when 0 */
            if (c == 0) continue;
        }

        switch(c) {
        case ENTER:    /* enter */
//            history_len--;
//            free(history[history_len]);
            doHistoryOp(LINENOISE_HISTORY_OP_DELETE);
            if (mlmode) linenoiseEditMoveEnd(&l);
            return (int)l.len;
        case CTRL_C:     /* ctrl-c */
            errno = EAGAIN;
            return -1;
        case BACKSPACE:   /* backspace */
        case 8:     /* ctrl-h */
            linenoiseEditBackspace(&l);
            break;
        case CTRL_D:     /* ctrl-d, remove char at right of cursor, or if the
                            line is empty, act as end-of-file. */
            if (l.len > 0) {
                linenoiseEditDelete(&l);
            } else {
                doHistoryOp(LINENOISE_HISTORY_OP_DELETE);
                return -1;
            }
            break;
        case CTRL_T:    /* ctrl-t, swaps current character with previous. */
            if (l.pos > 0 && l.pos < l.len) {
                int aux = buf[l.pos-1];
                buf[l.pos-1] = buf[l.pos];
                buf[l.pos] = aux;
                if (l.pos != l.len-1) l.pos++;
                refreshLine(&l);
            }
            break;
        case CTRL_B:     /* ctrl-b */
            linenoiseEditMoveLeft(&l);
            break;
        case CTRL_F:     /* ctrl-f */
            linenoiseEditMoveRight(&l);
            break;
        case CTRL_P:    /* ctrl-p */
            linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_PREV);
            break;
        case CTRL_N:    /* ctrl-n */
            linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_NEXT);
            break;
        case ESC:    /* escape sequence */
            /* Read the next two bytes representing the escape sequence.
             * Use two calls to handle slow terminals returning the two
             * chars at different times. */
            if (read(l.ifd,seq,1) == -1) break;
            if (read(l.ifd,seq+1,1) == -1) break;

            /* ESC [ sequences. */
            if (seq[0] == '[') {
                if (seq[1] >= '0' && seq[1] <= '9') {
                    /* Extended escape, read additional byte. */
                    if (read(l.ifd,seq+2,1) == -1) break;
                    if (seq[2] == '~') {
                        switch(seq[1]) {
                        case '3': /* Delete key. */
                            linenoiseEditDelete(&l);
                            break;
                        }
                    }
                } else {
                    switch(seq[1]) {
                    case 'A': /* Up */
                        linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_PREV);
                        break;
                    case 'B': /* Down */
                        linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_NEXT);
                        break;
                    case 'C': /* Right */
                        linenoiseEditMoveRight(&l);
                        break;
                    case 'D': /* Left */
                        linenoiseEditMoveLeft(&l);
                        break;
                    case 'H': /* Home */
                        linenoiseEditMoveHome(&l);
                        break;
                    case 'F': /* End*/
                        linenoiseEditMoveEnd(&l);
                        break;
                    }
                }
            }

            /* ESC O sequences. */
            else if (seq[0] == 'O') {
                switch(seq[1]) {
                case 'H': /* Home */
                    linenoiseEditMoveHome(&l);
                    break;
                case 'F': /* End*/
                    linenoiseEditMoveEnd(&l);
                    break;
                }
            }
            break;
        default:
            if (c < 32 || (c >= 0x80 && c < 0xA0)) continue;   /* skip unhandled control character (include C1) */
            if (linenoiseEditInsert(&l,cbuf,nread)) return -1;
            break;
        case CTRL_U: /* Ctrl+u, delete the whole line. */
            buf[0] = '\0';
            l.pos = l.len = 0;
            refreshLine(&l);
            break;
        case CTRL_K: /* Ctrl+k, delete from current to end of line. */
            buf[l.pos] = '\0';
            l.len = l.pos;
            refreshLine(&l);
            break;
        case CTRL_A: /* Ctrl+a, go to the start of the line */
            linenoiseEditMoveHome(&l);
            break;
        case CTRL_E: /* ctrl+e, go to the end of the line */
            linenoiseEditMoveEnd(&l);
            break;
        case CTRL_L: /* ctrl+l, clear screen */
            linenoiseClearScreen();
            refreshLine(&l);
            break;
        case CTRL_W: /* ctrl+w, delete previous word */
            linenoiseEditDeletePrevWord(&l);
            break;
        }
    }
    return l.len;
}

#if 0
/* This special mode is used by linenoise in order to print scan codes
 * on screen for debugging / development purposes. It is implemented
 * by the linenoise_example program using the --keycodes option. */
void linenoisePrintKeyCodes(void) {
    char quit[4];

    printf("Linenoise key codes debugging mode.\n"
            "Press keys to see scan codes. Type 'quit' at any time to exit.\n");
    if (enableRawMode(STDIN_FILENO) == -1) return;
    memset(quit,' ',4);
    while(1) {
        char c;
        int nread;

        nread = read(STDIN_FILENO,&c,1);
        if (nread <= 0) continue;
        memmove(quit,quit+1,sizeof(quit)-1); /* shift string to left. */
        quit[sizeof(quit)-1] = c; /* Insert current char on the right. */
        if (memcmp(quit,"quit",sizeof(quit)) == 0) break;

        printf("'%c' %02x (%d) (type quit to exit)\n",
            isprint((int)c) ? c : '?', (int)c, (int)c);
        printf("\r"); /* Go left edge manually, we are in raw mode. */
        fflush(stdout);
    }
    disableRawMode(STDIN_FILENO);
}
#endif

/* This function calls the line editing function linenoiseEdit() using
 * the STDIN file descriptor set in raw mode. */
static int linenoiseRaw(char *buf, size_t buflen, const char *prompt) {
    int count;

    if (buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(INPUT_FD)) {
        /* Not a tty: read from file / pipe. */
        int rlen;
        if((rlen = read(INPUT_FD, buf, buflen)) <= 0)
            return -1;
        buf[(unsigned int)rlen < buflen ? rlen : rlen - 1] = '\0';
        count = (unsigned int)rlen < buflen ? rlen : rlen - 1;
        if (count && buf[count-1] == '\n') {
            count--;
            buf[count] = '\0';
        }
    } else {
        /* Interactive editing. */
        if (enableRawMode(INPUT_FD) == -1) return -1;
        count = linenoiseEdit(INPUT_FD, OUTPUT_FD, buf, buflen, prompt);
        disableRawMode(INPUT_FD);
        write(OUTPUT_FD, "\n", 1);
    }
    return count;
}

/* The high level function that is the main API of the linenoise library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *linenoise(const char *prompt) {
    char buf[LINENOISE_MAX_LINE];
    int count;

    if (isUnsupportedTerm()) {
        size_t len;

        write(OUTPUT_FD, prompt, strlen(prompt));
        fsync(OUTPUT_FD);
        int rlen;
        if((rlen = read(INPUT_FD, buf, LINENOISE_MAX_LINE)) <= 0)
            return NULL;
        buf[rlen < LINENOISE_MAX_LINE ? rlen : rlen - 1] = '\0';
        len = rlen < LINENOISE_MAX_LINE ? rlen : rlen - 1;
        while(len && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            len--;
            buf[len] = '\0';
        }
        return strdup(buf);
    } else {
        count = linenoiseRaw(buf,LINENOISE_MAX_LINE,prompt);
        if (count == -1) return NULL;
        return strdup(buf);
    }
}

/* ================================ History ================================= */

/* At exit we'll try to fix the terminal to the initial conditions. */
static void linenoiseAtExit(void) {
    disableRawMode(INPUT_FD);
}

int *linenoiseInputFD() {
    return &INPUT_FD;
}

int *linenoiseOutputFD() {
    return &OUTPUT_FD;
}

int *linenoiseErrorFD() {
    return &ERROR_FD;
}
