#include <stdlib.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define DEFAULT_BUF_CAP 128 	// Default buffer line count
#define DEFAULT_LINE_CAP 16 	// Default (empty) line cap
#define ESC 27 
#define ENTER 13
#define BACKSPACE 8
#define TAB 9
#define CURSOR_BUF_LEN 32	// Slightly arbitrary but whatever
#define TAB_WIDTH 4

/* 
 * To be clear on the nomenclature, the values stored in
 * these buffer structures (i.e. the line number and the
 * position of a character in said line) are sometimes referred
 * to in the code as logical positions. Where the actual character
 * is printed on the screen is the physical position
 */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Line;

typedef struct {
    Line *lines;
    size_t line_count;
    size_t cap;
} Buffer;

typedef struct {
    size_t row;
    size_t col;
} Cursor;


// The current window/viewport of
// the text editor; i.e. what is the topmost
// visible row?
typedef struct {
    size_t top_line;
    size_t top_rowoff;
} CurrentView;

enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN
};

// vi-style editor modes. It's what I'm used to and it's
// my editor >:)
enum Mode {
    NORMAL,
    INSERT,
    COMMAND
};