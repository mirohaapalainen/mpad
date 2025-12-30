/* 
 * mpad - vi-like screen-oriented text editor
 * Miro Haapalainen
 */

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DEFAULT_BUF_CAP 128 	// Default buffer line count
#define DEFAULT_LINE_CAP 16 	// Default (empty) line cap
#define ESC 27 
#define ENTER 13
#define BACKSPACE 8
#define TAB 9
#define CURSOR_BUF_LEN 32	// Slightly arbitrary but whatever

/* 
 * To be clear on the nomenclature, the values stored in
 * these buffer structures (i.e. the line number and the
 * position of a character in said line) are sometimes referred
 * to in the code as virtual positions. Where the actual character
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
    int top_line;
} CurrentView;

Buffer global_buffer;

Cursor global_cursor;

struct termios orig_termios;


/* 
 * This buffer implementation is probably not the
 * most performant one, since it involves reallocating
 * a lot of memory over and over again
 */
static void line_reserve(Line *l, size_t needed) {
    if (needed <= l->cap) return;
    while (l->cap < needed) {
        l->cap *= 2;
    }
    l->data = realloc(l->data, l->cap);
}

void line_insert_char(Line *l, size_t pos, char c) {
    if (pos > l->len) {
        pos = l->len;
    }
    line_reserve(l, l->len+2);
    memmove(&l->data[pos+1], &l->data[pos], l->len - pos + 1);
    l->data[pos] = c;
    l->len++;
}

void line_delete_char(Line *l, size_t pos) {
    if (pos >= l->len) return;

    memmove(&l->data[pos], &l->data[pos+1], l->len - pos);

    l->len--;
}

void buffer_insert_line(Buffer *b, size_t row) {
    if (row > b->line_count) {
        row = b->line_count;
    }

    if (b->line_count + 1 > b->cap) {
        b->cap *= 2;
	b->lines = realloc(b->lines, b->cap * sizeof(Line));
    }

    memmove(&b->lines[row+1],
	    &b->lines[row],
	    (b->line_count - row) * sizeof(Line));

    Line *l = &b->lines[row];
    l->cap = DEFAULT_LINE_CAP;
    l->data = calloc(16, 1);
    l->len = 0;

    b->line_count++;
}

void buffer_delete_line(Buffer *b, size_t row) {
    if (b->line_count == 1) {
        b->lines[0].len = 0;
	b->lines[0].data[0] = '\0';
	return;
    }

    free(b->lines[row].data);

    memmove(&b->lines[row],
	    &b->lines[row+1],
	    (b->line_count - row - 1) * sizeof(Line));

    b->line_count--;
}

void buffer_split_line(Buffer *b, Cursor *c) {
    Line *cur = &b->lines[c->row];

    buffer_insert_line(b, c->row + 1);

    Line *next = &b->lines[c->row + 1];

    size_t tail_len = cur->len - c->col;
    line_reserve(next, tail_len + 1);

    memcpy(next->data, &cur->data[c->col], tail_len);
    next->data[tail_len] = '\0';
    next->len = tail_len;

    cur->len = c->col;
    cur->data[c->col] = '\0';

    c->row++;
    c->col = 0;
}

void buffer_append_line(Buffer *b, char **text, size_t len) {
    if (b->line_count + 1 > b->cap) {
        b->cap *= 2;
	b->lines = realloc(b->lines, b->cap * sizeof(Line));
    }

    Line *l = &b->lines[b->line_count];

    l->cap = len + 1;
    l->data = malloc(l->cap);
    memcpy(l->data, *text, len);
    l->data[len] = '\0';
    l->len = len;

    b->line_count++;
}

void buffer_init(Buffer *b) {
    b->cap = DEFAULT_BUF_CAP;
    b->line_count = 1;
    b->lines = calloc(b->cap, sizeof(Line));

    b->lines[0].cap = DEFAULT_LINE_CAP; // Start with one empty line
    b->lines[0].data = calloc(16, 1);   // Zero out memory space for line
    b->lines[0].len = 0;
}

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;

    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= (CS8);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void clear_screen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void draw_str(char* str, size_t len) {
    clear_screen();
    write(STDOUT_FILENO, str, len);
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        return -1;
    }

    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

void move_cursor(int row, int col) {
    char cursor_buf[32];
    int len = snprintf(cursor_buf, sizeof(cursor_buf), "\x1b[%d;%dH", row, col);
    write(STDOUT_FILENO, cursor_buf, len);

    global_cursor.row = row;
    global_cursor.col = col;
}

void clear_line() {
    write(STDOUT_FILENO, "\x1b[2K", 4);
}

void move_cursor_to_status_line() {
    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) {
        return;
    }

    write(STDOUT_FILENO, "\x1b[s", 3);
    move_cursor(rows, 2);
}

void draw_status_line(const char *status) {
    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) {
        return;
    }

    write(STDOUT_FILENO, "\x1b[s", 3);

    move_cursor(rows, 1);

    clear_line();
    write(STDOUT_FILENO, status, strlen(status));

    write(STDOUT_FILENO, "\x1b[u", 3);

}

// Takes a virtual x and y and returns the physical position
// on the screen it can be printed to
void virtual_to_physical_pos(int in_x, int in_y, int *out_x, int *out_y) {
    int winsize_x, winsize_y;
    if (get_window_size(&winsize_x, &winsize_y) == -1) {
        return;
    }
    
}

// vi-style editor modes. It's what I'm used to and it's
// my editor >:)
enum Mode {
    NORMAL,
    INSERT,
    COMMAND
};

// Write inputted char to screen and save to screen buffer
void write_input_at_pos(char *c, int row, int col) {
    move_cursor(row, col);

    // TODO - implement virtual buffer -> physical screen pos algorithm

    Line *l = global_buffer.lines[]

    line_insert_char(

    write(STDOUT_FILENO, c, 1);

    

}

void insert_newline() {
    char carriage_return = '\r';
    char newline = '\n';
    write(STDOUT_FILENO, &carriage_return, 1);
    
}

void write_file_content_initial(Buffer *b) {
    int rows, cols;

    get_window_size(&rows, &cols);

    for (size_t i = 0; i < b->line_count && i < (size_t)rows-2; i++) {
        write(STDOUT_FILENO, b->lines[i].data, b->lines[i].len);
        write(STDOUT_FILENO, "\r\n", 2);
    }
}

off_t get_file_size(const char* filename) {
    struct stat st;

    if (stat(filename, &st) == -1) {
        perror("Failed to stat file");
	return -1;
    }

    return st.st_size;
}

int buffer_load_file(Buffer *b, FILE *fp) {
    b->line_count = 0;

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&line, &cap, fp)) != -1) {
        if (n > 0 && line[n-1] == '\n') {
	    n--;
	}

	buffer_append_line(b, &line, (size_t)n);
    }
    free(line);
    fclose(fp);

    return 0;
}

int main(int argc, char *argv[]) {
    char c;
    enum Mode cur_mode = NORMAL;
    char file_status_line[50] = "\"";
    size_t file_size;

    buffer_init(&global_buffer);    

    if (argc == 1) {
        // TODO - You should be able to open this 
	// without providing a filename
	printf("Provide a filename\n");
	return 1;
    } else if (argc == 2) {
 	char *filename = argv[1];
	FILE *fp = fopen(filename, "r");
	if (fp != NULL) { // Opening existing file
	    buffer_load_file(&global_buffer, fp);

	    size_t file_size = get_file_size(filename);
	    char file_size_str[20];
	    strcat(file_status_line, filename);
	    strcat(file_status_line, "\", ");
	    sprintf(file_size_str, "%d", (int)file_size);
	    strcat(file_status_line, file_size_str);
	    strcat(file_status_line, "B");
	} else { // Creating new file
	    strcat(file_status_line, filename);
	    char newfile_indicator[] = "\" [NEW]";
	    strcat(file_status_line, newfile_indicator);
	}
    } else {
	// Mayhaps I will add this in later, but probably
	// not because I never use it
        printf("Invalid number of args\n");
	return 1;
    } 
    enable_raw_mode();
    clear_screen();

    write_file_content_initial(&global_buffer);

    draw_status_line(file_status_line);

    char cmd_buf = '\0';

    while (1) {
	if (read(STDIN_FILENO, &c, 1) == 1) {
	    if (cur_mode == INSERT) {
	        if (c == ESC) {
		    cur_mode = NORMAL;
		    draw_status_line("");
		} else if (c == '\r') {
		    insert_newline();
		} else {
		    write_input(&c);
		}
	    } else if (cur_mode == NORMAL) {
		if (c == 'i') {
		    cur_mode = INSERT;
		    draw_status_line("-- INSERT --");
		} else if (c == ':') {
		    cur_mode = COMMAND;
		    draw_status_line(":");
		    move_cursor_to_status_line();
		} else if (c == ESC) {
		    cur_mode = NORMAL;
		    draw_status_line(" ");
		}
	    } else if (cur_mode == COMMAND) {
		if (cmd_buf == '\0') {
		    write(STDOUT_FILENO, &c, 1);
		    cmd_buf = c;
		}

	        if (c == ENTER) {
		    if (cmd_buf == 'q') {
		        clear_screen();
			break;
		    } else {
		        draw_status_line("Unknown command");
			cmd_buf = '\0';
			cur_mode = NORMAL;
		    }
		} else if (c == BACKSPACE) {
		    draw_status_line(":  ");
		    cmd_buf = '\0';
		} else if (c == ESC) {
		    cur_mode = NORMAL;
		    cmd_buf = '\0';
		    draw_status_line(" ");
		}
	    }
	}
    }

    return 0;
}
