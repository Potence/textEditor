/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define EDITOR_VERSION "0.1"
#define EDITOR_TAB_LEN 8
#define EDITOR_QUIT_TIMES 1

#define CTRL_KEY(k) (k & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

typedef struct erow {
    int size;
    int rsize;
    char * chars;
    char * render;
} erow;

struct editorConfig {
    int cx, cy;  // cursor x and y position in row[_].chars
    int rx;  // cursor x position in row[_].render
    int rowoff;  // row offset in doc
    int coloff;  // column offset in doc
    int screenrows;
    int screencols; 
    int numrows;
    erow * row;
    int dirty;  // is there any dif from original file
    char * filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

/** terminal ***/

/**
 * die:
 * 
 * Clears screen and outputs cause of error and then exits program with a 
 * error code 1.
*/
void die(char * errorSource) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(errorSource);
    exit(1);
}

/**
 * disableRawMode:
 * 
 * Set the configuration of terminal attributes to orig_terminos from 
 * before program execution.
*/
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

/**
 * enableRawMode:
 * 
 * Enables raw mode such that input is not echoed to output, and is read
 * byte-wise, and control signals are ignored.
*/
void enableRawMode() {
    // STDIN_FILENO = 0
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // necessary for old terminals
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_oflag &= ~(OPOST); // disables addition of carriage return to output
    raw.c_cflag |= (CS8); // necessary for old terminals
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // timeout for read, tenths of seconds
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // ECHO: disables echo, ICANON: enables byte-wise reasing, 
    // ISIG: ignore ctrl-c,z..., IEXTEN: disables ctrl-v,o
    // IXON: disables ctrl-s,q
    // ICRNL: allows us to read carriage return lines

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/**
 * editorReadKey:
 * 
 * Read a single character from the terminal and return it.
*/
int editorReadKey() {
    int nread; // number of bytes read from STDIN
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        // exit if failed to read bytes
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        // escape character for special keys
        char seq[3];

        // make sure there are all bytes for input to be arrow key
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

                if (seq[2] == '~') {
                    // '\x1b[0~' format
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    // '\x1b[Z' format
                    case 'A': return ARROW_UP;  // '\x1b[A'
                    case 'B': return ARROW_DOWN;  // '\x1b[B'
                    case 'C': return ARROW_RIGHT;  // '\x1b[C'
                    case 'D': return ARROW_LEFT;  // '\x1b[D'
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        // standard key
        return (int) c;
    }
}

/**
 * getCursorPosition:
 * 
 * Get the position of the cursor and place it into rows, cols pointers
 * if successful otherwise do nothing.
 * 
 * returns:
 *  0   on success
 *  -1   on failure
*/
int getCursorPosition(int *rows, int *cols) {
    int buf_len = 32;
    char buf[buf_len];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < (unsigned int) buf_len - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0'; // null terminate string

    // printf("\r\n&buf[2]: '%s'\r\n", &buf[2]); // TODO: test remove

    if (buf[0] != '\x1b' || buf[1] != '[') return -1; // could not read rows, cols
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

/**
 * getWindowSize:
 * 
 * Get the window and puts it into the row and col input pointers on success.
 * 
 * Returns:
 *  0   On success.
 *  -1  On failure.
*/
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // \x1b[C moves the cursor forward, \x1b[B moves cursor down
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

/**
 * editorRowCxToRx:
 * 
 * Convert the text documents row->chars cx index to row->render rx index.
*/
int editorRowCxToRx(erow * row, int cx) {
    int rx = 0;

    for (int i = 0; i < cx; i++) {
        if (row->chars[i] == '\t') {
            rx += (EDITOR_TAB_LEN - 1) - (rx % EDITOR_TAB_LEN);
        }
        rx++;
    }
    return rx;
}

/**
 * editorUpdateRow:
 * 
 * Update row->render and row->rsize to contain the representative string of
 * row->chars and row->size.
*/
void editorUpdateRow(erow * row) {
    int tabs = 0;

    for (int i = 0; i < row->size; i++) {
        if (row->chars[i] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc((row->size + tabs * (EDITOR_TAB_LEN - 1) + 1) * sizeof(char));

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % EDITOR_TAB_LEN != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

/**
 * editorInsertRow:
 * 
 * Inserts a new row which will be at E.row[at], will contain len bytes from s,
 * and updates all other vairables in E.row[at].
*/
void editorInsertRow(int at, char * s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    // TODO: add error handling of failed realloc
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(E.row + at + 1, E.row + at, sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(E.row + at);

    E.numrows++;
    E.dirty++;
}

/**
 * editorAppendRow:
 * 
 * Add new row to E.row's with the inputted string s with atmost len bytes 
 * copied. Wrapper for editorInsertRow.
*/
void editorAppendRow(char * s, size_t len) {
    editorInsertRow(E.numrows, s, len);
}

/**
 * editorFreeRow:
 * 
 * Free all allocated memory withing the erow, does not free the erow itself.
*/
void editorFreeRow(erow * row) {
    free(row->chars);
    free(row->render);
}

/**
 * editorDelRow:
 * 
 * Delete row <int at> in E.row.
*/
void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(E.row + at);
    memmove(E.row + at, E.row + at + 1, sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

/**
 * editorRowInsertChar:
 * 
 * Insert a character c into row->chars[at], expands row->chars, and calls 
 * editorUpdateRow to update row->render.
*/
void editorRowInsertChar(erow * row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);  // TODO: failed realloc?
    memmove(row->chars + at + 1, row->chars + at, row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

/**
 * editorRowAppendString:
 * 
 * Append <size_t len> bytes from string <char * s> to row->chars and update
 * row->size, row->render calling editorUpdateRow. 
*/
void editorRowAppendString(erow * row, char * s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(row->chars + row->size, s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

/**
 * editorRowDeleteChar:
 * 
 * Delete a character in row at position <int at>.
*/
void editorRowDeleteChar(erow * row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(row->chars + at, row->chars + at + 1, row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/***  editor operations  ***/

/**
 * editorInsertChar:
 * 
 * Insert charcter c into a current cursor x,y positions.
*/
void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorAppendRow("", 0);
    }
    editorRowInsertChar(E.row + E.cy, E.cx, c);
    E.cx++;
}

/**
 * editorDelChar:
 * 
 * Deleted the character before the current cursor position and decrements
 * the cursor 1 to the left. Calls editorRowDeleteChar. If at end of file
 * does nothing.
*/
void editorDelChar() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow * row = E.row + E.cy;
    if (E.cx > 0) {
        editorRowDeleteChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(E.row + E.cy - 1, row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/**
 * editorInsertNewLine:
 * 
 * Insert a NewLine at the current cursor positon and move to start of newline.
*/
void editorInsertNewLine() {
    if (E.cx == 0) {
        /* start of row*/
        editorInsertRow(E.cy, "", 0);
    } else {
        /* split row */
        erow * row = E.row + E.cy;
        editorInsertRow(E.cy + 1, row->chars + E.cx, row->size - E.cx);
        row = E.row + E.cy;  // editorInsertRow realloc may change row address
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

/***  file i/o  ***/

/**
 * editorRowsToString:
 * 
 * Put all rows into a string with '\n' seperating rows and return. Place 
 * total length of constructed string into <int * buflen>.
*/
char * editorRowsToString(int * buflen) {
    int totlen = 0;
    
    for (int i = 0; i < E.numrows; i++) {
        totlen += E.row[i].size + 1;  //TODO: should buflen be here
    }
    *buflen = totlen;

    char * buf = malloc(totlen);  // TODO error handling
    char * cur = buf;
    for (int i = 0; i < E.numrows; i++) {
        memcpy(cur, E.row[i].chars, E.row[i].size);
        cur += E.row[i].size;
        *cur = '\n';
        cur++;
    }

    return buf;
}

/**
 * editorOpen:
 * 
 * Opens a file and reads the first line of the file and stores it in the row
 * in editorConfig E.
*/
void editorOpen(char * filename) {
    free(E.filename);
    E.filename = strndup(filename, 20);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char * line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

/**
 * editorSave:
 * 
 * Save all rows in E.row into E.filename using editorRowsToString.
*/
void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as : %s (ESC to cancel)");
        if (E.filename == NULL) {
            editorSetStatusMessage("Save Aborted");
            return;
        }
    }

    int len;
    char * buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // open or create file
    if (fd != -1) {
        if ((ftruncate(fd, len)) != -1) {
            if ((write(fd, buf, len)) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

/**
 * abAppend:
 * 
 * Takes an <abuf ab> and appends a string <char * s> of length <int len>
*/
void abAppend(struct abuf *ab, const char * s, int len) {
    char * new = realloc(ab->b, (ab->len + len) * sizeof(char));

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len); // append s to ab->b
    ab->b = new;
    ab->len += len;
}

/**
 * abFree:
 * 
 * Free string b associated with input <struct abuf ab>. 
*/
void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

/**
 * editorScroll:
 * 
 * Updates E.rowoff and E.coloff offsets to allow scrolling.
*/
void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        // TODO: could optimize and keep track of E.rx instead of finding it
        E.rx = editorRowCxToRx(E.row + E.cy, E.cx);
    }

    if (E.cy < E.rowoff) {
        // scrolls up
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        // scrolls down
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        // scrolls left
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        // scrolls right
        E.coloff = E.rx - E.screencols + 1;
    }
}

/**
 * editorDrawRows:
 * 
 * Draws '~' next to every line in editor.
*/
void editorDrawRows(struct abuf *ab) {
    // draw first n-1 rows
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows){
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(
                    welcome, 
                    sizeof(welcome),
                    "Text editor -- version %s", 
                    EDITOR_VERSION
                );
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding) {
                    abAppend(ab, " ", 1);
                    padding--;
                }
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

/**
 * editorDrawStatusBar:
 * 
 * Draws a status bar at the bottom of the terminal screen containing filename
 * of currently worked on file. Takes in output buffer <struct abuf * ab> and 
 * adds status bar to bottom of it. Assumes ab is on a new line already.
*/
void editorDrawStatusBar(struct abuf * ab) {
    abAppend(ab, "\x1b[7m", 4);  // invert colors of text
    char status[80], rstatus[80];  // TODO: should this be expanded
    int len, rlen;

    len = snprintf(status, sizeof(status), "%.20s - %d lines %s", 
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");

    rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cy + 1, E.numrows);

    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);  // reset to default color of text
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf * ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strnlen(E.statusmsg, sizeof(E.statusmsg));
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        // display message for 5 seconds only
        abAppend(ab, E.statusmsg, msglen);
    }
}

/**
 * editorRefreshScreen:
 * 
 * Clears the screen and redraw the rows calling editorDrawRows() function.
*/
void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // hide cursor
    // abAppend(&ab, "\x1b[2J", 4); // clear screen
    abAppend(&ab, "\x1b[H", 3); // move cursor to 1,1

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // move cursor to E.cx, E.cy
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
    (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/**
 * editorSetStatusMessage:
 * 
 * Add formated status to E.statusmsg and update E.statusmsglen accordingly.
*/
void editorSetStatusMessage(const char * fmt, ...) {
    va_list ap;  // holds args passed?
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

char * editorPrompt(char * prompt) {
    size_t bufsize = 128;
    char * buf = malloc(bufsize);  // TODO:erorr handling

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) { // TODO: should set limit on  buflen?
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == BACKSPACE || c == CTRL_KEY('h') || c == DEL_KEY) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            /* escape */
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

/**
 * editorMoveCursor:
 * 
 * Move the cursor in editorConfigs E.cx and E.cy based on movement direction
 * given in input <char key>.
*/
void editorMoveCursor(int key) {
    erow * row = (E.cy >= E.numrows) ? NULL : E.row + E.cy;

    switch (key) {
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) E.cy++;
            break;
        case ARROW_RIGHT:
            // if (E.cx != E.screencols - 1) 
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
    }
    row = (E.cy >= E.numrows) ? NULL : E.row + E.cy;
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

/**
 * editorProcessKeypress:
 * 
 * Process keypress and do special functions if it is a special charcater
 * > ctrl+q/q:      Quit program
 * > ctrl+s:        Save file
 * > arrow keys:    Move Cursor
 * > pgUp:          Move cursor to top of screen
 * > pgDn:          Move cursor to bottom of screen
 * > home:          Move cursor to left of screen
 * > end:           Move cursor to right of screen
 * > del:           Move cursor to 0,0
*/
void editorProcessKeypress() {
    static int quit_times = EDITOR_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewLine();
            break;
        case CTRL_KEY('q'):
        case CTRL_KEY('c'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            editorSave();
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case BACKSPACE:
        case CTRL_KEY('h'): // original backspace character
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

                int times = E.screenrows;
                while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case ARROW_UP:
        case ARROW_RIGHT:
        case ARROW_DOWN:
        case ARROW_LEFT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }
    quit_times = EDITOR_QUIT_TIMES;
}

/*** init ***/

/**
 * initEditor:
 * 
 * Update the editorConfig global varibales screenrows and screencols
*/
void initEditor() {
    // initalize cursor to 0,0
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char * argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: CTRL+S = save | Ctrl+Q/C = quit");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
