#include "kernel/types.h"
#include "user/user.h"

static void sieve(int left_read);

static void sieve(int left_read) {
  int prime;
  int num;
  int right[2];

  if (read(left_read, &prime, sizeof(prime)) == 0) {
    close(left_read);
    exit(0);
  }

  printf("prime %d\n", prime);
  pipe(right);

  int pid = fork();
  if (pid < 0) {
    fprintf(2, "primes: fork failed\n");
    close(left_read);
    close(right[0]);
    close(right[1]);
    exit(1);
  }

  if (pid == 0) {
    close(right[1]);
    close(left_read);
    void (*next)(int) = sieve;
    next(right[0]);
    exit(0);
  } else {
    close(right[0]);

    while (read(left_read, &num, sizeof(num)) > 0) {
      if (num % prime != 0) {
        write(right[1], &num, sizeof(num));
      }
    }

    close(left_read);
    close(right[1]);
    wait(0);
    exit(0);
  }
}

int main(int argc, char *argv[]) {
  int p[2];
  pipe(p);

  int pid = fork();
  if (pid < 0) {
    fprintf(2, "primes: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    close(p[1]);
    sieve(p[0]);
    exit(0);
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
