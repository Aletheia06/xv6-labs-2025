//
// Created by Liang on 2026/8/24.
//
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(2, "usage: sleep <seconds>\n");
        exit(0);
    }

    int seconds = atoi(argv[1]);
    pause(seconds);
    exit(0);
}