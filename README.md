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

---

## utils lab

对应 MIT 6.S081 util lab：<https://pdos.csail.mit.edu/6.828/2021/labs/util.html>。

这次实现了 5 个用户态工具：`sleep`、`pingpong`、`primes`、`find`、`xargs`。这些题虽然代码量不大，但非常考验对 Unix 进程模型、管道语义、文件描述符生命周期的理解。

先说 `sleep`。这题参数单位是 tick，不是秒。实现位置在 `user/sleep.c`。

```c
if (argc != 2) {
  fprintf(2, "usage: sleep ticks\n");
  exit(1);
}
sleep(atoi(argv[1]));
exit(0);
```

底层调用链路：用户态把参数 `n` 放在 `a0`，syscall 编号放在 `a7`，执行 `ecall` 陷入内核；CPU 发生 trap 后会从 U 模式进入 S 模式，并由 xv6 trap 入口保存用户寄存器现场到 `trapframe`。随后内核在 `syscall()` 里从 `trapframe->a7` 取编号分发到 `sys_sleep()`，再通过 `argint(0, &n)` 从 `trapframe->a0` 取到参数，最后把返回值写回 `trapframe->a0`，`sret` 返回用户态。

从并发角度看，`sys_sleep()` 本身不负责唤醒，它只是调用 `sleep(&ticks, &tickslock)` 把当前进程挂到 channel `&ticks` 上；真正唤醒发生在时钟中断里 `wakeup(&ticks)`。

这里锁配合也很关键：`acquire(&tickslock)` 保护 `ticks` 读写与条件判断，进入 `sleep(chan, lk)` 时会先拿 `p->lock` 再释放 `lk`，避免在“检查条件 -> 入睡”窗口丢失唤醒，同时也防止拿着全局锁睡眠导致其他 CPU 无法推进系统状态。这一套是典型的“条件锁 + 进程锁”协作模型。

`pingpong` 题目要求父子进程来回传一个字节，所以我用了两条 pipe 做双向通道：一条父写子读，另一条子写父读。当前实现严格传 1 字节并在父进程 `wait(0)` 回收子进程，代码在 `user/pingpong.c`。

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

`primes` 用了链式筛法：每一层进程先从左侧管道读到第一个数，这个数就是该层 prime（因为到这一层时它已经被前面所有 prime 过滤过），然后把后续不能被它整除的数转发到右侧新管道。右侧进程不是一次性建好，而是按需 `fork` 出来。

这里还有个关键点是 fd 继承：`fork` 之后子进程会继承父进程的打开文件描述符（引用同一个管道端点），所以父子两边都要关闭自己不用的读/写端；否则只要还有进程持有写端，下游 `read` 也不会拿到 EOF。fd 数字本身通常会随着新 `pipe` 继续分配变大，但关闭后也可能被复用。

代码在 `user/primes.c`。

```c
if (read(left_read, &prime, sizeof(prime)) == 0) {
  close(left_read);
  exit(0);
}
printf("prime %d\n", prime);
```

`find` 我基本按 `ls.c` 的目录遍历套路写：`open` 目录后循环 `read(dirent)`，按路径 `stat` 判断类型，如果是目录就递归进去。这里必须跳过 `.` 和 `..`，否则会无限递归。实现在 `user/find.c`。

```c
if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
  continue;
find(buf, filename);
```

这一题把文件系统对象关系串起来了：`struct dirent` 是目录项（核心是 `name + inum`）；`struct stat` 是元信息载体（`type/size/nlink/...`），负责告诉我们这个名字对应的是文件还是目录。

`fstat(fd, &st)` 会走到内核 `sys_fstat -> filestat`，先从进程参数里拿到 fd 和用户态 `stat*` 地址，再读取 `struct file` 里挂着的 inode 信息，调用 `stati(ip, &st)` 把 inode 字段填到内核态 `st`，最后 `copyout(p->pagetable, user_addr, ...)` 把结果拷回用户空间。

这里的 inode 保存类型、大小、数据块地址等，不保存文件名；文件名存在目录项里，通过 `inum` 指向 inode。

最后是 `xargs`。stdin 每读到一行就执行一次命令。我把 `xargs` 后面的固定参数先存入 `argv_exec`，再按字符读标准输入，遇到 `\n` 就把这一行作为额外参数拼进去，然后 `fork + exec`，父进程 `wait`。实现在 `user/xargs.c`。

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

---

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

---

## pgtbl lab (ugetpid + vmprint + pgaccess)

对应题目：<https://pdos.csail.mit.edu/6.828/2021/labs/pgtbl.html>

`syscall` lab 里我们更多在系统调用分发层打转；到了 `pgtbl`，重点变成页表本身：一是让用户态能通过共享页拿到轻量信息（`ugetpid`），二是把三级页表结构清晰打印出来（`vmprint`），三是利用 PTE 的 Accessed 位做访问检测（`pgaccess`）。 

`ugetpid` 的核心是给每个进程多映射一页 `USYSCALL`。这个地址放在 `TRAPFRAME` 下面（`kernel/memlayout.h`），并定义共享结构：

```c
struct usyscall {
  int pid;
};
```

接下来在 `struct proc` 里加 `usyscall` 指针（`kernel/proc.h`），在 `allocproc()` 分配这页并写入 `pid`，在 `freeproc()` 释放，最后在 `proc_pagetable()` 里映射到用户地址空间，权限给 `PTE_R | PTE_U`。这样用户态就可以只读这页，不用陷入内核就拿到 pid。用户态函数 `ugetpid()` 放在 `user/ulib.c`，本质就是把 `USYSCALL` 强转成 `struct usyscall *` 后读字段返回。

`ugetpid()` 里看到的 `u`（例如 `0x3fffffd000`）是用户虚拟地址。这个地址在每个进程里看起来一样，因为它是固定槽位 `USYSCALL`；但不同进程页表会把它翻译到不同物理页（各自的 `p->usyscall`），所以每个进程读到的是自己的 `pid`。

`0x3fffffd000` 这个值是 `memlayout.h` 宏按页大小逐级减出来的：

```c
#define PGSIZE     4096              // 0x1000
#define MAXVA      (1L << 38)        // 0x4000000000
#define TRAMPOLINE (MAXVA - PGSIZE)  // 0x3ffffff000
#define TRAPFRAME  (TRAMPOLINE - PGSIZE) // 0x3fffffe000
#define USYSCALL   (TRAPFRAME - PGSIZE)  // 0x3fffffd000
```

也就是：`USYSCALL = 0x4000000000 - 3 * 0x1000 = 0x3fffffd000`。

这里的 `MAXVA` 来自 RISC-V 的 `Sv39` 分页模式。`Sv39` 可以理解为“39 位虚拟地址 + 三级页表”：每级页表索引 9 位，再加 12 位页内偏移（`9+9+9+12=39`）。xv6 里之所以用 `MAXVA = (1L << 38)`，是为了只使用最高位为 0 的那一半地址空间，避免处理高位符号扩展地址带来的复杂性。

> 这个题目想传达的点：并不是所有信息都必须经 syscall 才能给用户态，关键是映射和权限要设计清楚。

`vmprint` 这题要递归打印有效 PTE。实现放在 `kernel/vm.c`：外层 `vmprint()` 先打印根页表地址，内层 `vmprintwalk()` 遍历 512 个表项；遇到无效项跳过，遇到有效项打印缩进和 `pte/pa`，如果是非叶子项就继续递归。缩进格式按实验脚本要求用 `" .."`，层级越深前缀越长。

结合一次实际调试值帮助理解：如果某项 `pte = 0x21fd2401`，先看低 10 位标志位 `pte & 0x3ff = 0x001`，说明只有 `PTE_V` 置位，`R/W/X` 都是 0，所以它不是叶子映射，而是“指向下一层页表”。此时 `PTE2PA(pte) = ((pte >> 10) << 12)` 可以提取出下一层页表的物理页地址（例如 `0x87f49000`），`vmprintwalk()` 就会递归到这一页继续打印。反过来，如果某项同时带 `R/W/X` 任意一位，它就是叶子项：打印后不再递归，表示该虚拟页已经映射到最终物理页。

另外题目会检查是否在合适时机打印页表，我是放在 `exec()` 提交新页表之后、且 `pid == 1` 时打印（`kernel/exec.c`）。

最后是 `pgaccess`。这部分其实就是把“页表遍历”换成“页表状态采样”：用户传入起始虚拟地址、页数和结果位图地址，内核逐页找到 PTE，检查 `PTE_A` 是否置位，如果置位就把结果位图对应 bit 设为 1，并把 `PTE_A` 清掉，最后用 `copyout` 把 bitmask 送回用户态。

> `PTE_A` 不是 `sys_pgaccess/sys_pgdirty` 主动“写上去”的，通常是 CPU/MMU 在页访问（读/写）时按 ISA 规则自动置位；系统调用做的是读取并按需清位，用于增量观测。

我的实现在 `kernel/sysproc.c:sys_pgaccess()`，配套改动是：

- `kernel/riscv.h` 增加 `PTE_A`
- `kernel/syscall.h` 增加 `SYS_pgaccess`
- `kernel/syscall.c` 注册 `sys_pgaccess`
- `user/user.h` 增加 `pgaccess` 声明
- `user/usys.pl` 增加 `entry("pgaccess")`

在这个基础上我还补了一个 `pgdirty` 扩展（dirty 概念挺常见的）：逻辑和 `pgaccess` 平行，只是把 `PTE_A` 换成 `PTE_D`（Dirty 位，表示该页被写过）。会在命中后清位（`*pte &= ~PTE_D`）。

> 这里顺手补一下 ISA 规范：`PTE_A` 和 `PTE_D` 的位置是 RISC-V Privileged ISA 对页表项（PTE）格式的规定。对于 Sv39，常用低位权限/状态位包括 `V/R/W/X/U/G/A/D`，其中 `A` 是 bit6、`D` 是 bit7，所以代码里写成 `1L << 6` 和 `1L << 7`。

建议验证命令：

```bash
make clean
make grade
```

进入 xv6 后先跑：

```sh
pgtbltest
```

如果启用了本文档里的 `pgdirty` 扩展，`pgtbltest` 会额外打印 `pgdirty_test: OK`。

最后跑评分：

```bash
make grade
```

如果要用 gdb 观察 `ugetpid()` 读共享页的过程，可以按下面流程：

```bash
# 终端 A
make qemu-gdb
```

```bash
# 终端 B
gdb-multiarch kernel/kernel
```

在 gdb 中依次输入：

```gdb
set architecture riscv:rv64
target remote 127.0.0.1:26000
add-symbol-file user/_pgtbltest 0
break ugetpid
continue
```

然后在 qemu shell 里运行：

```sh
 pgtbltest
```

命中断点后可直接查看：

```gdb
p u
p u->pid
```

如果想进一步确认 `USYSCALL` 虚拟地址映射到的真实物理页，在 gdb 中输入：

```gdb
b proc_pagetable
```

然后打印：

```gdb
p/x p->usyscall # 0x87f1e000
```

> 在 xv6 的 qemu `virt` 机器里，RAM 基址是 `0x80000000`，所以 `0x87f1e000` 可以看成是从 RAM 基址偏移 `0x07f1e000` 的一页（由 `kalloc()` 从空闲页链表分配得到，不是固定值）。

再补三个和真实系统相关的背景点：

- 真实系统通常会做虚拟地址随机化（ASLR），同一进程每次运行看到的用户地址布局可能不同；xv6 为了教学可读性，布局更固定。
- VA->PA 翻译由 MMU 完成，TLB 是它的缓存；TLB 命中时直接使用缓存翻译，未命中时才触发页表遍历。
- 页表本体存放在主内存（RAM）里，不在用户堆里。xv6 中内核对物理内存做了 direct mapping，所以内核代码可以直接通过内核地址访问这些页表页。

不同进程为什么会看到“同一个虚拟地址值但不同内容”？关键是切换页表这一步：

```c
w_satp(MAKE_SATP(p->pagetable));
```

这句会：

- 把当前进程根页表的物理地址编码写入 `satp`。
- 告诉 CPU 接下来用哪一张页表做地址翻译。

因此同样是 `USYSCALL` 这个虚拟地址，进程 A 和进程 B 会被翻译到不同物理页，读到各自的 `pid`。

---

## traps lab (backtrace + alarm)

对应题目：<https://pdos.csail.mit.edu/6.828/2025/labs/traps.html>

这章我把它理解成两件事：第一件是“看清楚 trap 发生时控制流到底怎么走”；第二件是“学会在 trap 里改写用户返回现场”。前半部分是 backtrace，后半部分是用户态定时 handler（`sigalarm/sigreturn`）。

先说 backtrace。实现放在 `kernel/printf.c` 的 `backtrace()`，核心做法是沿着 frame pointer（`s0`）往上走：当前帧返回地址在 `fp-8`，上一个帧指针在 `fp-16`。因为每个内核栈是一页，所以我用当前 `fp` 所在页作为边界，走到越界就停。这样在 `sys_pause()` 里打一枪 `backtrace()`，跑 `bttest` 就能看到类似 `sysproc -> syscall -> trap` 这一串返回地址。为了后续排错方便，也在 `panic()` 里接了 `backtrace()`，panic 时直接带调用链。

再说 alarm。目标是：进程每消耗 N 个 timer ticks，就在用户态插入一次 handler 调用；handler 执行完通过 `sigreturn()` 回到原来被打断的位置继续跑。这个功能最关键的不是“跳过去”，而是“跳回来还原现场”。

我在 `struct proc` 里加了几组状态：

- `alarm_interval`：间隔 N
- `alarm_elapsed`：已累计 ticks
- `alarm_handler`：用户 handler 地址
- `alarm_active`：防止重入
- `alarm_tf`：保存被打断时的完整 trapframe

然后在 `usertrap()` 里只处理 timer interrupt（`which_dev == 2`）：如果开了 alarm 且当前不在 handler 中，就累计 tick；达到阈值时把当前 `trapframe` 备份到 `alarm_tf`，把 `epc` 改成 `alarm_handler`，并置 `alarm_active=1`。这样返回用户态后，CPU 会直接从 handler 入口执行。

`sys_sigreturn()` 的职责是把 `alarm_tf` 原样恢复到 `p->trapframe`，并清掉 `alarm_active`。这里还有个细节：要返回被中断前的 `a0`，否则会被 syscall 返回值污染，`alarmtest` 里的寄存器一致性检查会挂。

为了对齐 2025 版本题目，我额外加了 `pause()` syscall（内部逻辑与 `sleep()` 类似），并在 `sys_pause()` 里调用 `backtrace()`，这样 `bttest` 可以直接触发回溯。

这章最容易踩的坑我遇到过三个：

- 只改了 `epc` 没保存全寄存器，handler 返回后主流程变量错乱。
- 忘了重入保护，handler 还没 `sigreturn` 又被 timer 打断二次进入。
- `sigreturn` 返回值处理不对，`a0` 被覆盖，导致 test3 失败。

建议验证顺序：先 `bttest` 看 backtrace，再 `alarmtest` 跑 test0~test3，最后 `usertests -q` 收尾。