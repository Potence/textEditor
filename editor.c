/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

struct termios orig_termios;

/** terminal ***/

/**
 * outputs cause of error and then exits with a error code 1
*/
void die(char * errorSource) {
    perror(errorSource);
    exit(1);
}

/**
 * Set the configuration of terminal attributes to orig_terminos from 
 * before program execution.
*/
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) die("tcsetattr");
}

/**
 * Enables raw mode such that input is not echoed to output, and is read
 * byte-wise, and control signals are ignored.
*/
void enableRawMode() {
    // STDIN_FILENO = 0
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // necessary for old terminals
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_oflag &= ~(OPOST); // disables addition of carriage return to output
    raw.c_cflag |= (CS8); // necessary for old terminals
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // timeout for read
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // ECHO: disables echo, ICANON: enables byte-wise reasing, 
    // ISIG: ignore ctrl-c,z..., IEXTEN: disables ctrl-v,o
    // IXON: disables ctrl-s,q
    // ICRNL: allows us to read carriage return lines

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/*** init ***/

int main() {
    enableRawMode();

    char c;
    while (1) {
        c = '\0';
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");
        if (iscntrl(c)) {
            printf("%d\r\n", c);
        } else {
            printf("%d: ('%c')\r\n", c, c);
        }

        if (c == 'q') break;
    }
    return 0;
}
