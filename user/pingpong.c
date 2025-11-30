#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  int p2c[2];
  int c2p[2];
  int buf;

  int pid = fork();
  pipe(p2c);
  pipe(c2p);
  if (pid == 0) {
    // parent write, child read
    close(p2c[1]);
    close(c2p[0]);
    read(p2c[0], &buf, 1);
    close(p2c[0]);
    printf("%d: received ping\n", getpid());
    write(c2p[1], "x", 1);
    close(c2p[1]);
    exit(0);
  } else {
    close(p2c[0]);
    close(c2p[1]);
    write(p2c[1], "x", 1);
    close(p2c[1]);
    read(c2p[0], &buf, 1);
    close(c2p[0]);
    printf("%d: received pong\n", getpid());
    wait(0);
    exit(0);
  }
  return 0;
}
