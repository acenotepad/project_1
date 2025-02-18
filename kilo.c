/*** Programmer Notes ***/
// Must use \r\n for any new line - \r is carriage return



/*** includes  ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>




/*** defines ***/
#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

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




/*** data  ***/
struct editorConfig {
    // These integers will allow the user to move the cursors around
    int cx, cy;
    int screenrows;
    int screencols;
    struct termios orig_termios;
};

struct editorConfig E;




/*** terminal  ***/
// 'die' function prints an error and exits thr program
void die(const char *s) {
    perror(s);
    exit(1);
}

void disableRawMode() {
    // Applies the original terminal state upon exiting
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) 
        die("tcsetattr");
}

void enableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");    
    // atexit is called automatically when the program exits; this ensures raw mode is disabled no matter how the program exits
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    // Disables XOFF and XON in the terminal
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // Disables all output processing
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    // Echo lets the user see what they're typing in the terminal; this command turns it off
    // ICANON turns off canonical mode (this reads line by line - we want byte by byte); this command turns it off
    // ISIG turns off SIGINT and SIGTSTP (ctl-c and ctl-z)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0; //Minimum number of bytes to be read before read() can return
    raw.c_cc[VTIME] = 1; //Maximum amount of time to wait before read() returns

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// editorReadKey waits for one keypress and return it
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die ("read");
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
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
            }   else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }   else if (seq[0] == '0') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    
        return '\x1b';
    }   else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    
    buf[i] = "\0";

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize (int *rows, int *cols) {
    struct winsize ws;

    // the 1 || is for testing the fallback branch being developed; it is temporary
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }   else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}




/*** append buffer ***/
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    // Makes sure there is enough memory to allocate to the new string (current length + new section length)
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    // Copies string 's' to the end of the current buffer data
    memcpy(&new[ab->len], s, len);
    // updates the pointer and length
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}




/*** output ***/
void editorDrawRows(struct abuf *ab){
    int y;
    for (y = 0; y < E.screenrows; y++) {
        if (y == E.screenrows / 3) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
            if (welcomelen > E.screencols) welcomelen = E.screencols;

            // divides the screen width by 2 and then subtracts half of the string's length
            int padding = (E.screencols - welcomelen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }
            while (padding--) abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcomelen);
        }   else {
            abAppend(ab, "~", 1);
        }

        // The 'K' command is the Erase In Line command
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

// Renders the editor's user interface to the screen after each keypress
void editorRefreshScreen() {
    struct abuf ab = ABUF_INIT;
    
    // Note: \x1b is the escape character (27 in decimal)
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];
    // Make sure to add the +1 so it lines up with the 1-indexed terminal values
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));
    
    abAppend(&ab, "\x1b[?25h", 6);

    // Writes the buffer's contents to standard output
    write(STDOUT_FILENO, ab.b, ab.len);
    // Frees the memory
    abFree(&ab);
}




/*** input ***/
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screencols - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.screenrows - 1) {
                E.cy++;
            }
            break;
    }
}

// editorProcessKeypress waits for a keypress and then handles it
void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
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
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}




/*** init  ***/
// Initializes all the fields in the E struct
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
    enableRawMode();
    initEditor();

    char c;
    // While the program hasn't timed out, it reads in characters.
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}