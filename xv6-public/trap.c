#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

 struct ptable_t {
   struct spinlock lock;
   struct proc proc[NPROC];
 }; 

extern struct ptable_t ptable;
extern struct spinlock mlfqlock;
extern struct spinlock stridelock;

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void replaceProc(struct proc*);

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->killed == 0 && myproc()->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER) {
    // TODO implement 
    int time_limit, this;
    struct proc* p = myproc(), *next;
    
    time_limit = p->scheduling == 0 ? MLFQ[p->priority].time_quantum : STRIDE_QUANTUM;

    if (p->pid != 0 && time_limit > p->spent_quantum) {
        // Context switching between LWP in same lwp group.
        if (p->scheduling == 0) {
            spent_boost_time++;
        }

        this = -1;
        acquire(&ptable.lock);
        for (int i = 0; i < NPROC; i++) {
            if (ptable.proc[i].pid == p->pid) {
                this = i;
            }
            // Share time quantum and allotment in same lwp group
            if (ptable.proc[i].lwpGroupId == p->lwpGroupId) {
                ptable.proc[i].spent_quantum++;
                if (p->scheduling == 0) ptable.proc[i].spent_allotment++;
            }
        }
        
        if (this == -1) {
            cprintf("Error: This process is not in ptable.\n");
        }

        // Pick next thread to run
        next = NULL;
        for (int i = this + 1; i < NPROC; i++) {
            if (ptable.proc[i].lwpGroupId == p->lwpGroupId && ptable.proc[i].state == RUNNABLE) {
                next = &ptable.proc[i];
                break;
            }
        }

        if (next == NULL) {
            for (int i = 0; i <= this; i++) {
                if (ptable.proc[i].lwpGroupId == p->lwpGroupId && (ptable.proc[i].state == RUNNABLE || ptable.proc[i].state == RUNNING)) {
                    next = &ptable.proc[i];
                    break;
                }
            }
        }

        if (next == NULL) cprintf("Error: timer interrupt\n");
        release(&ptable.lock);

//        cprintf("between lwp %d -> %d(scheduling : %d, spent_quantum : %d)\n", p->pid, next->pid, next->scheduling, next->spent_quantum);
        // Context switching
        if (p != next){
            acquire(&ptable.lock);
            p->state = RUNNABLE;
            next->state = RUNNING;
            switchlwp(next);

            int intena = mycpu()->intena;
            mycpu()->proc = next;

            replaceProc(next);
            swtch(&p->context, next->context);
            mycpu()->intena = intena;
            release(&ptable.lock);
        }
    } else { 
        yield();
    }
  }
  
  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}




void replaceProc(struct proc* p) {
  struct proc* next = NULL;
  for (struct proc* tmp = p; tmp < &ptable.proc[NPROC]; tmp++) {
    if (tmp != p && tmp->lwpGroupId == p->lwpGroupId && tmp->state != ZOMBIE) {
        next = tmp;
        break;
    }
  }

  if (next == NULL) {
    for (struct proc* tmp = ptable.proc; tmp < p; tmp++) {
        if (tmp->lwpGroupId == p->lwpGroupId && tmp->state != ZOMBIE) {
            next = tmp;
            break;
        }
    }
  }

  if (next->scheduling == 0) {
    acquire(&mlfqlock);
    for (int i = 0; i < NPROC; i++) {
        if (MLFQ[next->priority].procList[i]->lwpGroupId == next->lwpGroupId) {
           MLFQ[next->priority].procList[i] = next;
           break;
        }
    }
    release(&mlfqlock);
  } else  {
    int index = FindHeap(next);
    acquire(&stridelock);
    if (index != 0){
       strideHeap.Element[index]->p = next;
    }
    else cprintf("Error: there is no element in stride heap.\n");
    release(&stridelock);
  }
}
