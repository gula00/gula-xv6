#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  int p2c[2], c2p[2], pid;
  char byte;

  pipe(p2c);
  pipe(c2p);

  if ((pid = fork()) < 0) {
    fprintf(2, "pingpong: fork failed\n");
    exit(1);
  } else if (pid == 0) {
    close(p2c[1]);
    close(c2p[0]);

    if (read(p2c[0], &byte, 1) != 1) {
      fprintf(2, "pingpong: child read failed\n");
      exit(1);
    }
    close(p2c[0]);

    printf("%d: received ping\n", getpid());
    if (write(c2p[1], &byte, 1) != 1) {
      fprintf(2, "pingpong: child write failed\n");
      exit(1);
    }
    close(c2p[1]);

    exit(0);
  } else {
    close(p2c[0]);
    close(c2p[1]);

    if (write(p2c[1], "p", 1) != 1) {
      fprintf(2, "pingpong: parent write failed\n");
      exit(1);
    }
    close(p2c[1]);

    if (read(c2p[0], &byte, 1) != 1) {
      fprintf(2, "pingpong: parent read failed\n");
      exit(1);
    }
    close(c2p[0]);

    printf("%d: received pong\n", getpid());
    wait(0);
    exit(0);
  }
}
