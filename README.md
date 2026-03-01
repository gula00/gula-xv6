# xv6-labs

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

## utils lab

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

## syscall lab (trace + sysinfo)

对应题目：<https://pdos.csail.mit.edu/6.828/2021/labs/syscall.html>

先不急着写 `trace` 和 `sysinfo` 逻辑，而是把系统调用从用户态到内核态再回来的骨架补完整。

从题目出发。它要求新增两个 syscall：

- `trace(mask)`：按位跟踪系统调用，打印 `pid + syscall 名字 + 返回值`。
- `sysinfo(struct sysinfo *)`：把系统空闲内存和进程数量写回用户态结构体。

它们共享同一套入口：用户态声明、stub 生成、编号注册、内核分发。

### 先把 syscall 骨架接通

完整链路是这样：

1. 在 `user/user.h` 里声明用户态函数原型。
2. 在 `user/usys.pl` 里加 `entry("xxx")`，让构建系统生成汇编桩。
3. 在 `kernel/syscall.h` 里分配 syscall 编号。
4. 在 `kernel/syscall.c` 里声明 `extern uint64 sys_xxx(void);` 并加入分发表。

这一套做完以后，`trace` 和 `sysinfo` 能被调用到内核。

### trace：打印 syscall

`trace` 的设计很直接：每个进程挂一个 `trace_mask`。谁调用了 `trace(mask)`，就把 mask 存在当前进程里。这个字段放在 `kernel/proc.h` 的 `struct proc` 里最自然。

对应内核实现 `sys_trace()` 在 `kernel/sysproc.c`，逻辑非常短：

```c
uint64
sys_trace(void)
{
  int mask;
  argint(0, &mask);
  myproc()->trace_mask = mask;
  return 0;
}
```

还有一个关键：子进程要继承 tracing。这个不是在 syscall 层做，而是在 `fork()` 做。也就是 `kernel/proc.c` 里复制上下文时，顺手复制：

```c
np->trace_mask = p->trace_mask;
```

打印时机也很重要。如果太早打印，你拿不到 syscall 的最终返回值；正确位置是在 `kernel/syscall.c:syscall()` 调用具体 `syscalls[num]()` 之后：

```c
p->trapframe->a0 = syscalls[num]();
if((p->trace_mask & (1 << num)) != 0) {
  printf("%d: syscall %s -> %ld\n", p->pid, syscall_names[num], p->trapframe->a0);
}
```

这里我配了一个 syscall 名称数组，这样日志可读性高很多。

### sysinfo：把内核数据安全地带回用户态

`sysinfo` 的重点在于用户态地址不能直接在内核里解引用。

用户传进来的 `struct sysinfo *` 只是用户虚拟地址，不是内核可以直接 `*ptr = ...` 的安全指针。正确做法是：先在内核栈上准备好临时 `struct sysinfo info`，再用 `copyout(p->pagetable, user_addr, (char *)&info, sizeof(info))` 按当前进程页表拷回用户空间。更具体地说，`copyout` 会用这个进程的页表 `pagetable` 把用户虚拟地址 `dstva` 翻译到对应物理页，然后把内核缓冲区的数据复制过去。

先定义结构体。题目让你用 `struct sysinfo`，我把它放在独立头文件 `kernel/sysinfo.h`：

```c
struct sysinfo {
  uint64 freemem;
  uint64 nproc;
};
```

用户态 `user/user.h` 里只需要前置声明和原型：

```c
struct sysinfo;
int sysinfo(struct sysinfo *);
```

为什么用户态代码会 `#include "kernel/sysinfo.h"`？因为 `struct sysinfo` 属于 syscall 的 ABI（内核写、用户读），两边必须看到完全一致的内存布局。与其在 user/kernel 各维护一份，不如只保留一份。所以 `user/user.h` 里只做前置声明（让函数原型能成立），真正要访问字段的文件（比如 `user/sysinfotest.c`）再包含 `kernel/sysinfo.h`。

接下来回到题目本身：`sysinfo` 只要填两项数据。

- `freemem()`：在 `kernel/kalloc.c` 遍历空闲链表，每个节点加一个 `PGSIZE`，得到当前空闲物理内存字节数。
- `nproc()`：在 `kernel/proc.c` 遍历 `proc[]`，统计 `state != UNUSED` 的项。

补一下 `kalloc` 的原理：xv6 的物理内存分配器是“按页分配 + 空闲单链表”。`kinit()` 会把可用物理页挂进全局 `kmem.freelist`；`kalloc()` 从链表头摘一页，`kfree()` 再头插回去，整个过程用 `kmem.lock` 保护并发。`freemem()` 统计的就是这个全局 freelist 里还剩多少页，反映的是系统级空闲内存，不是某个进程私有内存。

`proc` 指 xv6 全局进程表 `struct proc proc[NPROC]`，`nproc()` 用 `for(p = proc; p < &proc[NPROC]; p++)` 从头到尾扫一遍并计数；这个表在 `procinit()` 已初始化好，所以统计时直接遍历即可。

最后 `sys_sysinfo()`（`kernel/sysproc.c`）做三件事：

1. `argaddr(0, &addr)` 取用户传入指针。
2. 在内核栈上填一个 `struct sysinfo info`。
3. `copyout(p->pagetable, addr, (char *)&info, sizeof(info))` 回写用户空间。

### 验证顺序

先手工测：

```sh
trace 32 grep hello README
trace 2147483647 grep hello README
trace 2 usertests forkforkfork
sysinfotest
```

都对了以后再跑：

```bash
make LAB=syscall grade
```
