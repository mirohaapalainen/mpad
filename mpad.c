/* 
 * mpad - vi-like screen-oriented text editor
 * Miro Haapalainen
 */

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mpad.h"


Buffer global_buffer;
Cursor global_cursor;
CurrentView global_view = {0, 0}; 
struct termios orig_termios;

int  get_window_size(int *rows, int *cols);
void term_move_cursor(int row, int col);

void buffer_to_screen_unclipped(
        const Buffer *b,
        size_t target_line,
        size_t target_col,
        const CurrentView *view,
        int screen_cols,
        int *out_row, int *out_col);

static bool buffer_to_screen(
        const Buffer *b,
        size_t target_line,
        size_t target_col,
        const CurrentView *view,
        int screen_cols,
        int text_rows,
        int *out_row, int *out_col);

static void view_scroll_by_rows(CurrentView *view, const Buffer *b, int screen_cols, int delta_rows);
static void virtual_to_physical_pos(size_t logical_row, size_t logical_col, int *phys_row, int *phys_col);
static void write_input(char *c);
static void write_input_at_pos(char c, size_t logical_row, size_t logical_col);


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

    // Apparently this is needed for
    // handling the escape key. Idk

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void editor_scroll_to_cursor(void) {
    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) return;
    int text_rows = rows - 1;

    int cr, cc;
    buffer_to_screen_unclipped(&global_buffer, global_cursor.row, global_cursor.col, &global_view, cols, &cr, &cc);
    int delta = 0;
    if (cr < 0) delta = cr;
    else if (cr >= text_rows) delta = cr - (text_rows - 1);

    if (delta != 0) view_scroll_by_rows(&global_view, &global_buffer, cols, delta);
}

static void editor_place_terminal_cursor(void) {
    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) return;
    int text_rows = rows - 1;

    int r, c;
    if (!buffer_to_screen(&global_buffer, global_cursor.row, global_cursor.col, &global_view, cols, text_rows, &r, &c)) {
        editor_scroll_to_cursor();
        buffer_to_screen(&global_buffer, global_cursor.row, global_cursor.col, &global_view, cols, text_rows, &r, &c);
    }

    term_move_cursor(r+1, c+1);
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

void term_move_cursor(int row, int col) {
    char cursor_buf[32];
    int len = snprintf(cursor_buf, sizeof(cursor_buf), "\x1b[%d;%dH", row, col);
    write(STDOUT_FILENO, cursor_buf, len);

    //global_cursor.row = row;
    //global_cursor.col = col;
}

void editor_move_cursor(int key) {
    if (global_buffer.line_count == 0) return;

    Line *line = &global_buffer.lines[global_cursor.row];

    switch (key) {
        case ARROW_LEFT:
            if (global_cursor.col > 0) {
                global_cursor.col--;
            } else if (global_cursor.row > 0) {
                global_cursor.row--;
                global_cursor.col = global_buffer.lines[global_cursor.row].len;
            }
            break;
        
        case ARROW_RIGHT:
            if (global_cursor.col < line->len) {
                global_cursor.col++;
            } else if (global_cursor.row + 1 < global_buffer.line_count) {
                global_cursor.row++;
                global_cursor.col = 0;
            }
            break;

        case ARROW_UP:
            if (global_cursor.row > 0) global_cursor.row--;
            break;

        case ARROW_DOWN:
            if (global_cursor.row + 1 < global_buffer.line_count) global_cursor.row++;
            break;
    }

    line = &global_buffer.lines[global_cursor.row];
    if (global_cursor.col > line->len) global_cursor.col = line->len;

    editor_scroll_to_cursor();
    editor_place_terminal_cursor();
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
    term_move_cursor(rows, 2);
}

void draw_status_line(const char *status) {
    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) {
        return;
    }

    write(STDOUT_FILENO, "\x1b[s", 3);

    term_move_cursor(rows, 1);

    clear_line();
    write(STDOUT_FILENO, status, strlen(status));

    write(STDOUT_FILENO, "\x1b[u", 3);

}

// How many terminal columns does this prefix of the
// line occupy?
int visual_width_upto(const Line *l, size_t upto_col) {
    int width = 0;
    size_t end = MIN(upto_col, l->len);

    for (size_t i = 0; i < end; i++) {
        if (l->data[i] == '\t') {
            int add = TAB_WIDTH - (width % TAB_WIDTH);
	        width += add;
	    } else {
	        width += 1;
	    }
    }
    return width;
}

// How many screen rows does a line occupy?
int screen_rows_for_line(const Line *l, int screen_cols) {
    int vwidth = visual_width_upto(l, l->len);
    return MAX(1, (vwidth + screen_cols - 1) / screen_cols); 
}

// Takes a logical x and y and returns the physical position
// on the screen it can be printed to
void buffer_to_screen_unclipped(
        const Buffer *b,
        size_t target_line,
        size_t target_col,
        const CurrentView *view,
        int screen_cols,
        int *out_row, int *out_col) {
    
    int row = -(int)view->top_rowoff;

    if (target_line < view->top_line) {
        *out_row = -999999;
        *out_col = 0;
        return;
    }

    for (size_t l = view->top_line; l < target_line && l < b->line_count; l++) {
        row += screen_rows_for_line(&b->lines[l], screen_cols);
    }

    if (target_line >= b->line_count) {
        *out_row = 999999;
        *out_col = 0;
        return;
    }

    const Line *cur = &b->lines[target_line];
    target_col = MIN(target_col, cur->len);

    int vcol = visual_width_upto(cur, target_col);
    row += vcol / screen_cols;
    int col = vcol % screen_cols;

    *out_row = row;
    *out_col = col;
}

static bool buffer_to_screen(
        const Buffer *b,
        size_t target_line,
        size_t target_col,
        const CurrentView *view,
        int screen_cols,
        int text_rows,
        int *out_row, int *out_col) {

    int r, c;
    buffer_to_screen_unclipped(b, target_line, target_col, view, screen_cols, &r, &c);
    if (r < 0 || r >= text_rows) return false;
    *out_row = r;
    *out_col = c;
    return true;
}

static void view_scroll_by_rows(CurrentView *view, const Buffer *b, int screen_cols, int delta_rows) {
    if (b->line_count == 0) {
        view->top_line = 0;
        view->top_rowoff = 0;
        return;
    }

    if (delta_rows > 0) {
        for (int i = 0; i < delta_rows; i++) {
            int rows_in_line = screen_rows_for_line(&b->lines[view->top_line], screen_cols);
            if (view->top_rowoff + 1 < (size_t)rows_in_line) {
                view->top_rowoff++;
            } else {
                if (view->top_line + 1 >= b->line_count) break;
                view->top_line++;
                view->top_rowoff = 0;
            }
        }
    } else if (delta_rows < 0) {
        for (int i = 0; i < -delta_rows; i++) {
            if (view->top_rowoff > 0) {
                view->top_rowoff--;
            } else {
                if (view->top_line == 0) break;
                view->top_line--;
                int rows_in_prev = screen_rows_for_line(&b->lines[view->top_line], screen_cols);
                view->top_rowoff = (rows_in_prev > 0) ? (size_t)(rows_in_prev - 1) : 0;
            }
        }
    }
}

static void write_input(char *c) {
    write_input_at_pos(*c, global_cursor.row, global_cursor.col);
    global_cursor.col++;

    editor_scroll_to_cursor();
    editor_place_terminal_cursor();
}

static void virtual_to_physical_pos(size_t logical_row, size_t logical_col, int *phys_row, int *phys_col) {
    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) {
        *phys_row = 1;
        *phys_col = 1;
        return;
    }

    int text_rows = rows - 1;

    int r, c;
    if (!buffer_to_screen(&global_buffer, logical_row, logical_col, &global_view, cols, text_rows, &r, &c)) {
        int cr, cc;
        buffer_to_screen_unclipped(&global_buffer, logical_row, logical_col, &global_view, cols, &cr, &cc);

        int delta = 0;
        if (cr < 0) delta = cr;
        else if (cr >= text_rows) delta = cr - (text_rows - 1);

        if (delta != 0) view_scroll_by_rows(&global_view, &global_buffer, cols, delta);

        buffer_to_screen(&global_buffer, logical_row, logical_col, &global_view, cols, text_rows, &r, &c);
    }

    *phys_row = r + 1;
    *phys_col = c + 1;
}

// Write inputted char to screen and save to screen buffer
void write_input_at_pos(char c, size_t logical_row, size_t logical_col) {

    if (logical_row >= global_buffer.line_count) return;

    Line *l = &global_buffer.lines[logical_row];
    if (logical_row > l->len) logical_col = l->len;

    line_insert_char(l, logical_col, c);

    int pr, pc;
    virtual_to_physical_pos(logical_row, logical_col, &pr, &pc);
    term_move_cursor(pr, pc);
    write(STDOUT_FILENO, &c, 1);
}

void insert_newline() {
    char carriage_return = '\r';
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

int dump_buffer_to_file(Buffer *b, const char *path) {
    FILE *fp = fopen(path, "w");	

    if (!fp) {
        perror("dump_buffer_to_file fopen");
	    return 1;
    }
	
    for (size_t i = 0; i < b->line_count; i++) {
        if (b->lines[i].len > 0) {
            if (fwrite(b->lines[i].data, 1, b->lines[i].len, fp) != b->lines[i].len) {
                perror("dump_buffer_to_file fwrite");
                fclose(fp);
                return 1;
            }
        }
        
        if (i + 1 < b->line_count) {
            fputc('\n', fp);
        }
    }

    fclose(fp);
    return 0;
}

static int editor_read_key(void) {
    char c;
    while (1) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 1) break;
        if (n == -1 && errno != EAGAIN) exit(1);
    }

    if (c == '\x1b') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return ESC;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return ESC;

        if (seq[0] == '[') {
            switch(seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        }
        return ESC;
    }
    return (unsigned char)c;
}

int main(int argc, char *argv[]) {
    char c;
    enum Mode cur_mode = NORMAL;
    char file_status_line[50] = "\"";
    size_t file_size;
    const char *filename = argv[1];

    buffer_init(&global_buffer);    

    if (argc == 1) {
        // TODO - You should be able to open this 
        // without providing a filename
        printf("Provide a filename\n");
        return 1;
    } else if (argc == 2) {
        FILE *fp = fopen(filename, "r");
        if (fp != NULL) { // Opening existing file
            buffer_load_file(&global_buffer, fp);

            file_size = get_file_size(filename);
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
        
        int key = editor_read_key();

        if (key == ARROW_UP || key == ARROW_DOWN || key == ARROW_LEFT || key == ARROW_RIGHT) {
            if (cur_mode != COMMAND) editor_move_cursor(key);
            continue;
        }

        c = (char)key;

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
                } else if (cmd_buf == 'w') {
                    if (dump_buffer_to_file(&global_buffer, filename) == 0) {
                        draw_status_line("Wrote to file");
                    } else {
                        draw_status_line("Error - write failed");
                    }
                    cmd_buf = '\0';
                    cur_mode = NORMAL;
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

    return 0;
}
