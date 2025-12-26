#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>

#define DEFAULT_BUF_LEN 4096
#define ESC 27

struct termios orig_termios;

char* screen_buf;

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

}

void clear_line() {
    write(STDOUT_FILENO, "\x1b[2K", 4);
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

enum Mode {
    NORMAL,
    INSERT,
    COMMAND
};

int main() {
    char c;
    enum Mode cur_mode = NORMAL;
    screen_buf = malloc(DEFAULT_BUF_LEN);
    enable_raw_mode();
    clear_screen();

    while (1) {
	if (cur_mode == INSERT) {
	    draw_status_line("-- INSERT --");
	} else if (cur_mode == NORMAL) {
	    draw_status_line("-- NORMAL --");
	} else if (cur_mode == COMMAND) {
	    draw_status_line(":");
	}

	if (read(STDIN_FILENO, &c, 1) == 1) {
	    if (cur_mode == INSERT) {
	        if (c == ESC) {
		    cur_mode = NORMAL;
		} else {
		    write(STDOUT_FILENO, &c, 1);
		}
	    } else if (cur_mode == NORMAL) {
		if (c == 'i') {
		    cur_mode = INSERT;
		} else if (c == ':') {
		    cur_mode = COMMAND;
		} else if (c == ESC) {
		    cur_mode = NORMAL;
		}
	    } else {
	    	
	    }
	}
    }

    return 0;
}
