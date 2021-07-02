#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;


struct spinlock mlfqlock;
struct spinlock stridelock;

static struct proc *initproc;

int nextpid = 1;
int nextGroupId = 1;
uint nextThreadId = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&mlfqlock, "mlfq");
  initlock(&stridelock, "stride");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->lwpGroupId = p->pid;
  p->ret_val = NULL;
  p->tid = 0;
  p->lwpStack = 0;
  p->deallocBound = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  // Initialize for MLFQ scheduling
  // Place process in highest level queue
  p->priority = 0;
  p->spent_allotment = 0;
  p->spent_quantum = 0;
  p->scheduling = 0; // MLFQ
 
  acquire(&mlfqlock);
  for (int i = 0; i < NPROC; ++i) {
    if (MLFQ[0].procList[i] == NULL) {
        MLFQ[0].procList[i] = p;        
        break;
    }
  }
  release(&mlfqlock);
   
  yield_flag = 0;

  return p;
}

void queueinit(void)
{
    int tmp_quantum[3] = {5, 10, 20};
    int tmp_allotment[3] = {20, 40, 0};
    // Initialize three queues for MLFQ
    acquire(&mlfqlock);
    for (int i = 0; i < 3; ++i) {
        MLFQ[i].time_quantum = tmp_quantum[i];
        MLFQ[i].time_allotment = tmp_allotment[i];
        for (int j = 0; j < NPROC; ++j) {
            MLFQ[i].procList[j] = NULL;
        }
    }

    spent_boost_time = 0;
    release(&mlfqlock);
}

// Initialize priority queue for stride scheduling.
void heapinit(void) {
    strideHeap.size = 1;
    MLFQTicketNum = TOTAL_TICKETS;
    acquire(&stridelock);
    for (int i = 0; i < NPROC; i++) {
      strideElements[i].pass = 0;
      strideElements[i].stride = 0;
      strideElements[i].ticketNum = 0;
      strideElements[i].p = NULL;
      strideHeap.Element[i+1] = NULL;
    }

    // init MLFQ scheduling having all tickets at first.
    strideElements[0].ticketNum = TOTAL_TICKETS;
    strideElements[0].stride = 1;

    strideHeap.Element[1] = &strideElements[0];
    release(&stridelock);
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  queueinit();
  heapinit();

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  acquire(&ptable.lock);
  for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
    if (tmp->lwpGroupId == curproc->lwpGroupId)
        tmp->sz = sz;
  }
  release(&ptable.lock);
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  /*DEBUG*/
  // cprintf("fork pid : %d, lwpGroupId : %d, sz : %d\n", curproc->pid, curproc->lwpGroupId, curproc->sz);
  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  /*DEBUG*/
  // cprintf("fork End pid : %d\n", curproc->pid);
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.

void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p, *tmp;
  int fd;

  if(curproc == initproc)
    panic("init exiting");
  if (curproc->pid != curproc->lwpGroupId) {
    acquire(&ptable.lock);

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->lwpGroupId == curproc->lwpGroupId && p->pid == p->lwpGroupId) {
            p->killed = 1;
            p->state = RUNNABLE;
            break;
        }
    }
    
    curproc->state = ZOMBIE;
    sched();
    panic("zombie exit");
  }

  for (int i = 0; i < NPROC; i++) {
    acquire(&ptable.lock);
    tmp = &ptable.proc[i];
    release(&ptable.lock);

    if (tmp->lwpGroupId != curproc->lwpGroupId) continue;

    // Close all open files.
    for(fd = 0; fd < NOFILE; fd++){
        if(tmp->ofile[fd]){
        fileclose(tmp->ofile[fd]);
        tmp->ofile[fd] = 0;
        }
    }
  
    begin_op();
    iput(tmp->cwd);
    end_op();
    tmp->cwd = 0;

    acquire(&ptable.lock);
    // Pass abandoned children to init.
  
  
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->parent == tmp && p->lwpGroupId != tmp->lwpGroupId){
            p->parent = initproc;
            if(p->state == ZOMBIE)
            wakeup1(initproc);
        }
    }
  
    // Jump into the scheduler, never to return.
    if (tmp != curproc) {
        kfree(tmp->kstack);
        tmp->kstack = 0;
        tmp->parent = 0;
        tmp->name[0] = 0;
        tmp->killed = 0;
        tmp->state = UNUSED;

        // Clean up LWP user stack..
        tmp->sz = deallocuvm(tmp->pgdir, tmp->lwpStack, tmp->lwpStack - 2*PGSIZE);
 
        tmp->sz = 0;
        tmp->lwpStack = 0;
        tmp->deallocBound = 0;
    }
    release(&ptable.lock);
  }
 
  int exitIndex = FindHeap(curproc);
  if (exitIndex != 0) {
      int MLFQindex = FindHeap(NULL);
      strideHeap.Element[MLFQindex]->ticketNum += strideHeap.Element[exitIndex]->ticketNum;
      MLFQTicketNum = strideHeap.Element[MLFQindex]->ticketNum;
      strideHeap.Element[MLFQindex]->stride = TOTAL_TICKETS / MLFQTicketNum;
      DeleteElement(exitIndex);
  } else {
      remove_proc_in_MLFQ(curproc);
  }
  
  acquire(&ptable.lock);

  for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
      if (tmp->lwpGroupId == curproc->lwpGroupId && tmp != curproc) {
        tmp->lwpGroupId = 0;
        tmp->pid = 0;
    }
  }

  curproc->state = ZOMBIE; 
  wakeup1(curproc->parent);
   
  sched();
  panic("zombie exit");
}
 
 // Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;

      if(p->state == ZOMBIE){ 
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Remove process in MLFQ list.
// Return removed Item.
struct proc* 
remove_proc_in_MLFQ(struct proc* p) {
  struct proc* ret = NULL;

  acquire(&mlfqlock);
  
  for (int i = 0; i < NPROC; ++i){
    if (MLFQ[p->priority].procList[i]->lwpGroupId == p->lwpGroupId) {
        ret = MLFQ[p->priority].procList[i];
        MLFQ[p->priority].procList[i] = NULL;
    }
  }

  release(&mlfqlock);

  if (ret == NULL) cprintf("Error: remove from MLFQ.\n");
  return ret;
}

// Move process p to lower level queue.
void
lower_queue(struct proc * p) 
{
  int before_pri = p->priority;
  int after_pri = before_pri + 1;
  struct proc* removedItem = NULL;

  // Error state
  if (before_pri == 2) {
    cprintf("Process(%d) is already in lowest level of queue\n", p->pid);
    return;
  }

  removedItem = remove_proc_in_MLFQ(p);

  acquire(&mlfqlock);
  for (int i = 0; i < NPROC; ++i) {
    // Insert process in lower level queue.
    if (MLFQ[after_pri].procList[i] == NULL) {
        MLFQ[after_pri].procList[i] = removedItem;
        break;
    }
  }
  release(&mlfqlock);
  
  for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++)
  {
    if (p->lwpGroupId == tmp->lwpGroupId) {
        tmp->priority++;
        tmp->spent_allotment = 0;
        tmp->spent_quantum = 0;
    }
  }
}

// Do priority boost.
// Move all processes to highest priority queue.
void
priority_boost()
{
  int last_index = 0;
  acquire(&mlfqlock);
  
  for (int level = 1; level < 3; ++level) {
    for (int i = 0; i < NPROC; ++i) {
      struct proc* p = MLFQ[level].procList[i];
      if (p != NULL) {
        for (; last_index < NPROC;) {
          if (MLFQ[0].procList[last_index] == NULL) { 
              p->priority = 0;

              for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
                if (p->lwpGroupId == tmp->lwpGroupId) {
                    tmp->priority = 0;
                    tmp->spent_allotment = 0;
                    tmp->spent_quantum = 0;
                }
              }
              MLFQ[0].procList[last_index++] = p;
              MLFQ[level].procList[i] = NULL;
              break;
          } else last_index++;
        }
      }
    }
  }
  
  spent_boost_time = 0;
  release(&mlfqlock);
}

struct HeapElement* procToRun(){
  return DeleteElement(1);
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p = NULL;
  struct cpu *c = mycpu();
  struct HeapElement* curr, *next = NULL;
  int noMLFQproc = 0;
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    int scheduledProcess = 0;
    for (int level = 0; level < 3; level++){
      
      if (scheduledProcess) {
        level = 0;
        scheduledProcess = 0;
      }

      for (int i = 0; i < NPROC;) {
        curr = procToRun();

        acquire(&stridelock);
        struct HeapElement* firstElem = strideHeap.Element[1];
        release(&stridelock);

        // Turn to run process in stride scheduling.
        if (curr->p != NULL || (noMLFQproc == 1 && firstElem != NULL)) {
          noMLFQproc = 0;

          if (curr->p == NULL) {
            struct HeapElement* saveElement[NPROC];
            int index = 0;
            
            saveElement[index++] = curr;
            
            while(strideHeap.Element[1] != NULL) {
                next = procToRun();
                p = next->p;
                if (p->state != RUNNABLE) {
                    for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
                        if (tmp->lwpGroupId == p->lwpGroupId && tmp->state == RUNNABLE) {
                           
                            p = tmp;
                            break;
                        }
                    }
                }
            
                if (p->state == RUNNABLE) break;
                else saveElement[index++] = next;
            }

            if (next == NULL) cprintf("Error: scheduler pick stride process\n");

            for (int i = 0; i < index; i++) {
                InsertHeap(saveElement[i]);
            }
            
            if (p->state != RUNNABLE) {
                curr = NULL;
                next = NULL;
                continue;
            } else {;
                curr = next;
                p = curr->p;
            }
          } else {
            struct HeapElement* saveElement[NPROC];
            int index = 0;
            p = curr->p;
            
            while (curr != NULL && p->state != RUNNABLE) {
                p = curr->p;
                for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
                    if (tmp->lwpGroupId == p->lwpGroupId && tmp->state == RUNNABLE) {
                        p = tmp;
                        break;
                    }
                }

                if (p->state != RUNNABLE) {
                    saveElement[index++] = curr;
                    curr = procToRun(); 
                } else {
                    break;
                }
            }

            for (int i = 0; i < index; i++) {
                InsertHeap(saveElement[i]);
            }

            if (p->state != RUNNABLE) {
                curr = NULL;
                continue;
            }
          }
        } else {
          // Turn to run process in MLFQ scheduling.
          acquire(&mlfqlock);
          do {
            p = MLFQ[level].procList[i++];
           
            if (p != NULL && p->state != RUNNABLE) {
                // If there is runnable thread in same lwp group, run that one.
                for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
                    if (tmp->lwpGroupId == p->lwpGroupId && tmp->state == RUNNABLE){
                        p = tmp;
                        break;
                    }
                }
            }
          } while ((p == NULL || p->state != RUNNABLE) && i < NPROC);

          release(&mlfqlock);
        }

        // Already loop over all queue element in this level.
        if (i == NPROC) {
            InsertHeap(curr);
            break;
        }

        /* Debug */
        //cprintf("Scheduling process pid %d, lwpGroupId : %d, scheduling : %d\n", p->pid, p->lwpGroupId, p->scheduling);
                
        scheduledProcess = 1;
        
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;

        curr->pass += curr->stride;
        InsertHeap(curr);

        swtch(&(c->scheduler), p->context);       
        
        for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
            if (tmp->lwpGroupId == p->lwpGroupId)
                tmp->spent_quantum = 0;
        }

        if (curr->p == NULL) {
          if (level < 2 && p->spent_allotment >= MLFQ[level].time_allotment) {              
              lower_queue(p);
          }

          if (spent_boost_time >= BOOST_PERIOD) {
            // Priority Boost
            priority_boost();
          }
        }
        
        yield_flag = 0;
        switchkvm();
        c->proc = 0; 
      }
    }

    if (!scheduledProcess) noMLFQproc = 1;
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  yield_flag = 1;
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
    
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
        p->state = RUNNABLE;
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      for (struct proc *tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
        if (tmp->lwpGroupId == p->lwpGroupId) {
            tmp->killed = 1;
            // Wake process from sleep if necessary.
            if(tmp->state == SLEEPING)
                tmp->state = RUNNABLE;
        }
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int getppid(void){
    return myproc()->parent->pid;
}

int getlev(void) {
    return myproc()->priority;
}

int set_cpu_share(int portion) {
  struct HeapElement* elem;
   
  if ((elem = init_element(myproc(), (double)portion)) != NULL) {
    InsertHeap(elem);
    remove_proc_in_MLFQ(myproc());
   
    return 0;
  }
  
  return 1;
}

// Min heap functions

struct HeapElement* init_element(struct proc* p, double ticketNum) {
  struct HeapElement* ret = NULL;
  double stride, pass;
  int i;

  acquire(&stridelock);
  for (i = 0; i < NPROC; i++) {
    if (strideElements[i].stride == 0) {
      ret = &strideElements[i];
      break;
    }
  }
  release(&stridelock);

  // ERROR STATE
  if (ret == NULL) {
    cprintf("ERROR in init_element. \n");
    return ret;
  }

  acquire(&ptable.lock);
  for (i = 0; i < NPROC; i++) {
    if (ptable.proc[i].lwpGroupId == p->lwpGroupId)
        ptable.proc[i].scheduling = 1; // Stride scheduling
  }
  release(&ptable.lock);
    
  stride = TOTAL_TICKETS / ticketNum;
  
  pass = getMinPass();
  double newMLFQTicketNum = 0;
  if ((i = FindHeap(p)) != 0) {
    ret = strideHeap.Element[i];
    DeleteElement(i);

    if (ret->p != NULL) {
      double beforeTicketNum = ret->ticketNum;
      newMLFQTicketNum = MLFQTicketNum - ticketNum + beforeTicketNum; 
    }
  } else {
    newMLFQTicketNum = MLFQTicketNum - ticketNum;
  }
  
  if (newMLFQTicketNum < 20){
    return NULL;
  }

  ret->stride = stride;
  ret->ticketNum = ticketNum;
  if (i == 0) {
    ret->pass = pass;
    ret->p = p;
  } else ret = strideHeap.Element[i];

  MLFQTicketNum = newMLFQTicketNum;
  i = FindHeap(NULL);
  
  acquire(&stridelock);
  strideHeap.Element[i]->ticketNum = newMLFQTicketNum;
  strideHeap.Element[i]->stride = TOTAL_TICKETS / newMLFQTicketNum;
  release(&stridelock);
  return ret;
}

// Insert current process to strideHeap
void InsertHeap(struct HeapElement* elem){
  int i;

  acquire(&stridelock);
  for (i = ++strideHeap.size; i > 1 && strideHeap.Element[i/2]->pass > elem->pass; i /= 2) {
    strideHeap.Element[i] = strideHeap.Element[i/2];
  }
  
  strideHeap.Element[i] = elem;
  release(&stridelock);
  return;
}

int FindHeap(struct proc* p){
  acquire(&stridelock);
  for (int i = 1; i <= strideHeap.size; i++){
    if (strideHeap.Element[i]->p->lwpGroupId == p->lwpGroupId) {
        release(&stridelock);
        return i;
    }
  }
  release(&stridelock);
  
  return 0;
}

struct HeapElement* DeleteElement(int del){
  struct HeapElement* del_element, *last_element;
  int child_element, i;

  acquire(&stridelock);
  del_element = strideHeap.Element[del];
  if (del_element == NULL) return NULL;
  strideHeap.Element[del] = NULL;
  if (del == strideHeap.size) {
      strideHeap.size--;
      release(&stridelock);
      return del_element;
  }

  last_element = strideHeap.Element[strideHeap.size--];
  strideHeap.Element[strideHeap.size + 1] = NULL;
  for (i = del; i * 2 <= strideHeap.size; i = child_element){
    child_element = i * 2;
    if (child_element < strideHeap.size && strideHeap.Element[child_element + 1]->pass < strideHeap.Element[child_element]->pass) {
      child_element++;
    }

    if (last_element->pass > strideHeap.Element[child_element]->pass)
      strideHeap.Element[i] = strideHeap.Element[child_element];
    else break;
  }
  strideHeap.Element[i] = last_element;
  release(&stridelock);
  return del_element;
}

double getMinPass(){
  return strideHeap.Element[1]->pass;
}



/* For LWP */

int thread_create(thread_t *thread, void *(*start_routine)(void*), void *arg) {

  struct proc *np, *p;
  struct proc *curproc = myproc();
  uint sp, ustack[3];

  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Remove from MLFQ list
  for (int i = 0; i < NPROC; ++i) {
    if (MLFQ[0].procList[i] == np) {
        MLFQ[0].procList[i] = NULL;
        break;
    }
  }

  // Allocate LWP stack
  if (growproc(2*PGSIZE) != 0) {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }

  acquire(&ptable.lock);

  clearpteu(curproc->pgdir, (char*)(curproc->sz - 2*PGSIZE));
  sp = curproc->sz;
  np->lwpStack = sp;
  np->deallocBound = sp - 2*PGSIZE;

  ustack[0] = 0xffffffff; // fake return PC
  ustack[1] = (uint)arg; // argument

  sp -= 8;
  if (copyout(curproc->pgdir, sp, ustack, 8) < 0) return -1;

  // Share resources with master process
  np->pgdir = curproc->pgdir;
  *np->tf = *curproc->tf;
  np->tid = nextThreadId++;
  *thread = np->tid;

  np->tf->eip = (uint)start_routine;
  np->tf->esp = sp;

  if (curproc->tid == 0) {
    // curproc is normal process
    np->lwpGroupId = curproc->pid;
  } else {
    // curproc is lwp
    np->lwpGroupId = curproc->lwpGroupId;
  }
  np->priority = curproc->priority;
  np->spent_allotment = curproc->spent_allotment;
  np->spent_quantum = curproc->spent_quantum;
  np->scheduling = curproc->scheduling;

  // share sz

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->lwpGroupId == curproc->lwpGroupId) p->sz = curproc->sz;
  }
  
  release(&ptable.lock);

  np->parent = curproc;
  
  for(int i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
    np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

 
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);

  return 0;
}

void thread_exit(void *retval) {
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if (p->parent == curproc){
      p->parent = initproc;
      if (p->state == ZOMBIE)
          wakeup1(initproc);
    }
  }

  if (curproc->pid == curproc->lwpGroupId) {
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (curproc->lwpGroupId == p->lwpGroupId && curproc != p) {
            p->pid = curproc->pid;
            break;
        }
    }
  }

  // Save return value in curproc
  curproc->ret_val = retval;

   int exitIndex = FindHeap(curproc);
   if (exitIndex != 0) {
       if (strideHeap.Element[exitIndex]->p == curproc) {
         struct proc* otherproc = NULL;
         for (struct proc* tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++) {
           if (tmp != curproc && tmp->lwpGroupId == curproc->lwpGroupId && tmp->state != ZOMBIE) otherproc = tmp;
         }

         if (otherproc != NULL) {
           strideHeap.Element[exitIndex]->p = otherproc;
         } else {
           int MLFQindex = FindHeap(NULL);
           acquire(&stridelock);
           strideHeap.Element[MLFQindex]->ticketNum += strideHeap.Element[exitIndex]->ticketNum;
           MLFQTicketNum = strideHeap.Element[MLFQindex]->ticketNum;
           strideHeap.Element[MLFQindex]->stride = TOTAL_TICKETS / MLFQTicketNum;
           release(&stridelock);
           DeleteElement(exitIndex);
         }
      } 
   } else {
     int found = 0;
     for (int i = 0; i < NPROC; i++) {
        struct proc* tmp = MLFQ[curproc->priority].procList[i];
        if (tmp == curproc) {
            MLFQ[curproc->priority].procList[i] = NULL;
            for (struct proc* newt = ptable.proc; newt < &ptable.proc[NPROC]; newt++) {
                if (newt != tmp && newt->lwpGroupId == tmp->lwpGroupId && newt->state != ZOMBIE) {
                    MLFQ[curproc->priority].procList[i] = newt;
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }
     }
   }
   
   //cprintf("Thread exit %d %d\n", curproc->pid, curproc->lwpGroupId);
   curproc->state = ZOMBIE;
   wakeup1((void*)curproc->tid);
   sched();
    
  panic("thread_zombie exit");
}


int thread_join(thread_t thread, void **retval) {
  struct proc *p, *otherp;
  int havekids;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;) {
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if (p->parent != curproc || p->tid != thread) continue;
      havekids = 1;
      
      while (p->state != ZOMBIE) {
        sleep((void*)p->tid, &ptable.lock);
      }

      if (p->killed == 0 && p->state == ZOMBIE) {
        // Found one.
        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        p->lwpGroupId = 0;

        // Clean up LWP user stack..
        p->sz = deallocuvm(p->pgdir, p->lwpStack, p->lwpStack - 2*PGSIZE);
        
        if (p->lwpStack == curproc->sz) {
          for (otherp = ptable.proc; otherp < &ptable.proc[NPROC]; otherp++) {
              if (otherp->state != UNUSED && otherp->lwpGroupId == curproc->lwpGroupId) {
                  otherp->sz = p->deallocBound;
              }
          }
        } else {
          for (otherp = ptable.proc; otherp < &ptable.proc[NPROC]; otherp++) {
            if (otherp->deallocBound == p->lwpStack) {
              otherp->deallocBound = p->deallocBound;
              break;
            }
          }
        }

        p->sz = 0;
        p->lwpStack = 0;
        p->deallocBound = 0;

        *retval = p->ret_val;
        release(&ptable.lock);

        return 0;
      }
    }

    if (!havekids || curproc->killed) {
      release(&ptable.lock);
      return -1;
    }
  }

  return -1;
}


















