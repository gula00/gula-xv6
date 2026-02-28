#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;
void timerinit(void);

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(__sync_bool_compare_and_swap(&started, 0, 1)){
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
#ifdef USE_RUSTSBI
    timerinit();     // enable and arm supervisor timer via SBI
#endif
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    intr_on();       // allow device/timer interrupts before disk init
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    virtio_disk_init(); // emulated hard disk
    userinit();      // first user process
    __sync_synchronize();
    started = 2;
  } else {
    while(started != 2)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
#ifdef USE_RUSTSBI
    timerinit();      // enable and arm supervisor timer via SBI
#endif
    plicinithart();   // ask PLIC for device interrupts
    intr_on();        // allow device/timer interrupts
  }

  scheduler();        
}
