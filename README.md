# Added features

## kernel

## user

- [unix utilities] sleep / pingpong / primes / find / xargs
- [system calls] trace / sysinfo
- (?) command line history (in sh.c)

# Environment setup

Debug mode

```bash
make qemu-gdb
```

Then, in another terminal, run:

```bash
gdb-multiarch
```

Remote debugging

```gdb
target remote localhost:26000
file kernel/kernel
```

usefull gdb commands:

```gdb
layout split
continue
step
next
run
```
