# Added features

## kernel

## user

- sleep.c
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
