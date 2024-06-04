/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
// #include "getLine.h"

/*** defines ***/

#define CTRL_KEY(k) (k & 0x1f)
#define EDITOR_VERSION "0.1"

enum editorKey {
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
    char * chars;
} erow;

struct editorConfig {
    int cx, cy; // cursor x and y pointer
    int screenrows;
    int screencols;
    int numrows;
    erow * row;  // just 1 row for now
    struct termios orig_termios;
};

struct editorConfig E;

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

void editorAppendRow(char * s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/***  file i/o  ***/

/**
 * editorOpen:
 * 
 * Opens a file and reads the first line of the file and stores it in the row
 * in editorConfig E.
*/
void editorOpen(char * filename) {
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
 * editorDrawRows:
 * 
 * Draws '|' next to every line in editor, DEFAULT_ROWS number of rowss
*/
void editorDrawRows(struct abuf *ab) {
    // draw first n-1 rows
    for (int y = 0; y < E.screenrows; y++) {
        if (y >= E.numrows){
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
            int len = E.row[y].size;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, E.row[y].chars, len);
        }

        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

/**
 * editorRefreshScreen:
 * 
 * Clears the screen and redraw the rows calling editorDrawRows() function.
*/
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // hide cursor
    // abAppend(&ab, "\x1b[2J", 4); // clear screen
    abAppend(&ab, "\x1b[H", 3); // move cursor to 1,1

    editorDrawRows(&ab);

    // move cursor to E.cx, E.cy
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

/**
 * editorMoveCursor:
 * 
 * Move the cursor in editorConfigs E.cx and E.cy based on movement direction
 * given in input <char key>.
*/
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_LEFT:
            if (E.cx != 0) E.cx--;
            break;
        case ARROW_DOWN:
            if (E.cy != E.screenrows - 1) E.cy++;
            break;
        case ARROW_RIGHT:
            if (E.cy != E.screencols - 1) E.cx++;
            break;
    }
}

/**
 * editorProcessKeypress:
 * 
 * Process keypress and do special functions if it is a special charcater
 * > ctrl+q/q:      Quit program
 * > arrow keys:    Move Cursor
 * > pgUp:          Move cursor to top of screen
 * > pgDn:          Move cursor to bottom of screen
 * > home:          Move cursor to left of screen
 * > end:           Move cursor to right of screen
 * > del:           Move cursor to 0,0
*/
void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('c'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case DEL_KEY:
            E.cx = 0;
            E.cy = 0;
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screencols - 1;
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
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
    }
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
    E.numrows = 0;
    E.row = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char * argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
