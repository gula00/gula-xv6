#include "kernel/types.h"
#include "user/user.h"

void primes(int* left) {
  close(left[1]);
  int buf, pid, right[2], temp;

  // if no data is available, 'read' will be blocked
  // if all work down, exit!
  if (read(left[0], &buf, sizeof(buf)) == 0) {
    close(left[0]);
    exit(0);
  }

  printf("prime %d\n", buf);
  pipe(right);

  if ((pid = fork()) == 0) {
    close(left[0]);
    // primes(right); // won't pass the compiler
    void (*next)(int*) = primes; // indirect call via a function pointer
    next(right);
    //exit(0);
  } else {
    close(right[0]);
    while (read(left[0], &temp, sizeof(temp)) > 0) {
      if (temp % buf != 0) {
        write(right[1], &temp, sizeof(temp));
      }
    }
    close(right[1]);
    wait(0);
    exit(0);
  }
}

int main(int argc, char *argv[]) {
  int p[2];
  pipe(p);

  int pid = fork();
  if (pid == 0) {
    primes(p);
    // exit(0);
  } else {
    close(p[0]);
    for (int i = 2; i <= 280; i++) {
      write(p[1], &i, sizeof(int));
    }
    close(p[1]);
    wait(0);
    exit(0);
  }
}
