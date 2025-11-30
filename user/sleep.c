#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  if (argc == 2) {
    int n = atoi(argv[1]);
    fprintf(1, "sleeping for %d seconds\n", n);
    sleep(n);
    exit(0);
  } else {
    fprintf(2, "ERROR!!!\n");
    exit(1);
  }
}
