#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

static void
run_command(char **argv_exec)
{
  int pid = fork();
  if (pid < 0) {
    fprintf(2, "xargs: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    exec(argv_exec[0], argv_exec);
    fprintf(2, "xargs: exec %s failed\n", argv_exec[0]);
    exit(1);
  }

  wait(0);
}

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(2, "usage: xargs command [args ...]\n");
    exit(1);
  }

  char *argv_exec[MAXARG];
  int base = 0;
  for (int i = 1; i < argc; i++) {
    if (base >= MAXARG - 1) {
      fprintf(2, "xargs: too many arguments\n");
      exit(1);
    }
    argv_exec[base++] = argv[i];
  }

  char line[512];
  int n = 0;
  char c;
  while (read(0, &c, 1) == 1) {
    if (c == '\n') {
      line[n] = 0;
      if (n > 0) {
        argv_exec[base] = line;
        argv_exec[base + 1] = 0;
        run_command(argv_exec);
      }
      n = 0;
      continue;
    }

    if (n + 1 < sizeof(line)) {
      line[n++] = c;
    }
  }

  if (n > 0) {
    line[n] = 0;
    argv_exec[base] = line;
    argv_exec[base + 1] = 0;
    run_command(argv_exec);
  }

  exit(0);
}
