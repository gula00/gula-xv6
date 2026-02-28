# xv6-labs util

## 启动方式

- MIT 原版（无 SBI，`-bios none`，内核入口 `0x80000000`）：

```bash
make clean
make qemu
```

- RustSBI 启动（内核入口 `0x80200000`）：

```bash
make rustsbi-build
make qemu-rustsbi
```

- RustSBI + PKU 测试镜像（会把 `riscv64/` 程序打进 `fs.img`）：

```bash
make clean
make PKU_TEST=1 qemu-rustsbi
```

## 两种测评方式（MIT / PKU）

- MIT 6.S081 Lab 原生测评（当前仓库默认）：

```bash
make LAB=util grade
```

- PKU/oscomp 测评（已接入 `riscv64` 程序与 RustSBI 启动链）：

```bash
make run_test
```

如果要从官方 testsuits 重新编译并同步测评程序：

```bash
make pku-tests-update TESTSUITS_DIR=./testsuits-for-oskernel
```

对应 MIT 6.S081 util lab：<https://pdos.csail.mit.edu/6.828/2021/labs/util.html>。

这次实现了 5 个用户态工具：`sleep`、`pingpong`、`primes`、`find`、`xargs`。这些题虽然代码量不大，但非常考验对 Unix 进程模型、管道语义、文件描述符生命周期的理解。

先说 `sleep`。这题最关键的是确认参数单位是 tick，不是秒。实现位置在 `user/sleep.c`，核心代码如下：

```c
if (argc != 2) {
  fprintf(2, "usage: sleep ticks\n");
  exit(1);
}
sleep(atoi(argv[1]));
exit(0);
```

底层调用链路：用户态把参数 `n` 放在 `a0`，syscall 编号放在 `a7`，执行 `ecall` 陷入内核；CPU 发生 trap 后会从 U 模式进入 S 模式，并由 xv6 trap 入口保存用户寄存器现场到 `trapframe`。随后内核在 `syscall()` 里从 `trapframe->a7` 取编号分发到 `sys_sleep()`，再通过 `argint(0, &n)` 从 `trapframe->a0` 取到参数，最后把返回值写回 `trapframe->a0`，`sret` 返回用户态。

从并发角度看，`sys_sleep()` 本身不负责唤醒，它只是调用 `sleep(&ticks, &tickslock)` 把当前进程挂到 channel `&ticks` 上；真正唤醒发生在时钟中断里 `wakeup(&ticks)`。这里锁配合也很关键：`acquire(&tickslock)` 保护 `ticks` 读写与条件判断，进入 `sleep(chan, lk)` 时会先拿 `p->lock` 再释放 `lk`，避免在“检查条件 -> 入睡”窗口丢失唤醒，同时也防止拿着全局锁睡眠导致其他 CPU 无法推进系统状态。这一套是典型的“条件锁 + 进程锁”协作模型。

`pingpong` 题目要求父子进程来回传一个字节，所以我用了两条 pipe 做双向通道：一条父写子读，另一条子写父读。当前实现严格传 1 字节并在父进程 `wait(0)` 回收子进程，代码在 `user/pingpong.c`。核心片段如下：

```c
if (write(p2c[1], "p", 1) != 1) {
  fprintf(2, "pingpong: parent write failed\n");
  exit(1);
}
if (read(c2p[0], &byte, 1) != 1) {
  fprintf(2, "pingpong: parent read failed\n");
  exit(1);
}
printf("%d: received pong\n", getpid());
```

`primes` 用了链式筛法：每一层进程先从左侧管道读到第一个数，这个数就是该层 prime（因为到这一层时它已经被前面所有 prime 过滤过，最先剩下的必然是新的质数），然后把后续不能被它整除的数转发到右侧新管道。右侧进程不是一次性建好，而是按需 `fork` 出来，下一层的输入/输出也确实来自这一层的 `pipe(right)`：`right[1]` 是当前层写端，`right[0]` 传给下一层当读端。这里还有个关键点是 fd 继承：`fork` 之后子进程会继承父进程的打开文件描述符（引用同一个管道端点），所以父子两边都要关闭自己不用的读/写端；否则只要还有进程持有写端，下游 `read` 也不会拿到 EOF。fd 数字本身通常会随着新 `pipe` 继续分配变大，但关闭后也可能被复用，所以调试时应关注“端口角色（读/写）和是否已关闭”，不要依赖某个固定 fd 值。实现在 `user/primes.c`：

```c
if (read(left_read, &prime, sizeof(prime)) == 0) {
  close(left_read);
  exit(0);
}
printf("prime %d\n", prime);
```

`find` 我基本按 `ls.c` 的目录遍历套路写：`open` 目录后循环 `read(dirent)`，按路径 `stat` 判断类型，如果是目录就递归进去。这里必须跳过 `.` 和 `..`，否则会无限递归。名字匹配时用 `strcmp`，匹配到了就打印完整路径。实现在 `user/find.c`，核心逻辑如下：

```c
if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
  continue;
find(buf, filename);
```

这一题顺便把文件系统对象关系串起来了：`struct dirent` 是目录项（核心是 `name + inum`），负责把“名字”枚举出来；`struct stat` 是元信息载体（`type/size/nlink/...`），负责告诉我们这个名字对应的是文件还是目录。`fstat(fd, &st)` 会走到内核 `sys_fstat -> filestat`，先从进程参数里拿到 fd 和用户态 `stat*` 地址，再读取 `struct file` 里挂着的 inode 信息，调用 `stati(ip, &st)` 把 inode 字段填到内核态 `st`，最后 `copyout(p->pagetable, user_addr, ...)` 把结果拷回用户空间。这里的 inode 可以理解成“文件实体记录”，保存类型、大小、数据块地址等，不保存文件名；文件名存在目录项里，通过 `inum` 指向 inode。

最后是 `xargs`。这个实验只需要简化版：stdin 每读到一行就执行一次命令。我把 `xargs` 后面的固定参数先存入 `argv_exec`，再按字符读标准输入，遇到 `\n` 就把这一行作为额外参数拼进去，然后 `fork + exec`，父进程 `wait`。实现在 `user/xargs.c`，关键片段如下：

```c
while (read(0, &c, 1) == 1) {
  if (c == '\n') {
    line[n] = 0;
    argv_exec[base] = line;
    argv_exec[base + 1] = 0;
    run_command(argv_exec);
    n = 0;
    continue;
  }
  if (n + 1 < sizeof(line))
    line[n++] = c;
}
```

把 util 相关程序都加入 `UPROGS`，程序会被打包进 `fs.img`。对应改动在 `Makefile` 的 `LAB=util` 分支。

建议验证命令：

```bash
make clean
make LAB=util qemu
```

进入 xv6 后逐个检查：

```sh
sleep 10
pingpong
primes
find . b
sh < xargstest.sh
```

最后跑评分：

```bash
make LAB=util grade
```
