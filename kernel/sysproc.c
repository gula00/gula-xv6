#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;

  backtrace();

  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_sigalarm(void)
{
  int interval;
  uint64 handler;
  struct proc *p = myproc();

  argint(0, &interval);
  argaddr(1, &handler);

  if(interval <= 0){
    p->alarm_interval = 0;
    p->alarm_elapsed = 0;
    p->alarm_handler = 0;
    p->alarm_active = 0;
  } else {
    p->alarm_interval = interval;
    p->alarm_elapsed = 0;
    p->alarm_handler = handler;
  }

  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  uint64 a0 = p->alarm_tf.a0;

  memmove(p->trapframe, &p->alarm_tf, sizeof(*p->trapframe));
  p->alarm_active = 0;
  p->alarm_elapsed = 0;

  return a0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  int mask;

  argint(0, &mask);
  myproc()->trace_mask = mask;
  return 0;
}

uint64
sys_sysinfo(void)
{
  uint64 addr;
  struct sysinfo info;
  struct proc *p = myproc();

  argaddr(0, &addr);

  info.freemem = freemem();
  info.nproc = nproc();

  if(copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;

  return 0;
}

static uint64
sys_pgbits(uint64 bit)
{
  uint64 va;
  int len;
  uint64 maskaddr;
  unsigned int mask = 0;
  struct proc *p = myproc();

  argaddr(0, &va);
  argint(1, &len);
  argaddr(2, &maskaddr);

  if(len < 0 || len > 32)
    return -1;

  for(int i = 0; i < len; i++) {
    pte_t *pte = walk(p->pagetable, va + i * PGSIZE, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;
    if((*pte & bit) != 0) {
      mask |= 1U << i;
      *pte &= ~bit;
    }
  }

  if(copyout(p->pagetable, maskaddr, (char *)&mask, sizeof(mask)) < 0)
    return -1;

  return 0;
}

uint64
sys_pgaccess(void)
{
  return sys_pgbits(PTE_A);
}

uint64
sys_pgdirty(void)
{
  return sys_pgbits(PTE_D);
}
