#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

#define PAGESIZE 4096
#define DATASIZE 8*PAGESIZE

int
main(int argc, char *argv[]) {
  // Your code here.
  char *addr = sbrk(DATASIZE);
  for (int i = 0;i < DATASIZE - 14; i++) {
    int match = 1;

    for (int j = 0;j < 14;j ++) {
      if (addr[i + j] != "This may help."[j]) {
        match = 0;
        break;
      }
    }

    if (match) {
      for (int k = 16;addr[i + k] != '\0';k++) {
        printf("%c", addr[i + k]);
      }
    }

  }
  exit(1);
}
