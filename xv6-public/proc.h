#include "defs.h"
#define BOOST_PERIOD 200
#define NULL 0
#define TOTAL_TICKETS 100
#define STRIDE_QUANTUM 5


// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  int priority;                // Process level in MLFQ scheduling
  int spent_allotment;         // Total number of tick passed in current queue's time allotment
  int spent_quantum;           // Total number fo tick passed in current queue's time quantum
  int scheduling;               // Scheduling Policy of this process (0: MLFQ, 1: Stride)

  /* For LWP */
  int lwpGroupId;              // Group id of LWP group
  void *ret_val;               // Return value of LWP
  uint lwpStack;               // Start address of stack for this lwp
  uint deallocBound;           // for master thread sz
  thread_t tid;                // Thread ID (If master thread, 0)
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap

int spent_boost_time; // Total number of tick passed in priority boost period (100 ticks)
int yield_flag;

// Queue structure for MLFQ scheduling
struct Queue {
    int time_quantum;
    int time_allotment;
    struct proc * procList[NPROC];
};

struct Queue MLFQ[3];


struct HeapElement {
    double pass;                    // Pass value for stride scheduling
    double stride;                  // Stride (Total ticket # / # of tickets of process)
    double ticketNum;                  // Ticket number of this process.
    struct proc * p;
};

struct Heap {
    int size;
    struct HeapElement* Element[NPROC + 1];
};

struct HeapElement strideElements[NPROC];

struct Heap strideHeap;
double MLFQTicketNum;

void queueinit(void);
void heapinit(void);
struct proc* remove_proc_in_MLFQ(struct proc* p);

// Heap functions
void exceed_possible_stride_scheduling();
struct HeapElement* init_element(struct proc* p, double ticketNum);
void InsertHeap(struct HeapElement* elem);
int FindHeap(struct proc* p);
struct HeapElement* DeleteElement(int del);
double getMinPass();
