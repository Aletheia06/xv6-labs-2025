//
// Created by Liang on 2026/8/24.
//
#include "kernel/types.h"
#include "user.h"
#define O_RDONLY 0
#define NULL ((void *)0)

int is_separator(char x);

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(2, "usage: %s <filename1> <filename2> ...\n", argv[0]);
        exit(1);
    }
    for (int i = 1; i < argc; i++) {
        char *filename = argv[i];
        int fd = open(filename,O_RDONLY);
        if (fd == -1) {
            fprintf(2, "%s: can't open %s\n", argv[0], filename);
            exit(1);
        }

        char buf;
        int number = 0;
        int has_number = 1;

        while (read(fd, &buf, 1) == 1) {
            if ('0' <= buf && buf <= '9' && has_number) {
                number = number * 10 + buf - '0';
            }
            else if (is_separator(buf)) {
                if (has_number && number != 0 && (number % 5 == 0 || number % 6 == 0)) {
                    printf("%d\n", number);
                }
                number = 0;
                has_number = 1;
            }
            else {
                has_number = 0;
                number = 0;
            }

        }
        if (has_number && number != 0 && (number % 5 == 0 || number % 6 == 0)) {
            printf("%d\n", number);
        }
        close(fd);
    }

    return 0;
}

int is_separator(char x) {
    if (strchr(" -\r\t\n./,", x) != NULL) {
        return 1;
    }
    return 0;
}