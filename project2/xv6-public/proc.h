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
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  int Runnable;                // Number of runnable thread
  uint ustacks[NPROC];         // Userstacks
  int memlimit;                // Memory limit of process
  int stackpages;              // Alloced stack pages
  
  struct proc *main_thread;    // Pointer of mainthread
  struct context *context;     // swtch() here to run process
  int tid;                     // Thread ID
  struct proc *rt;             // Recently selected Thread
  struct proc *next;           // Pointer of next thread
  struct proc *prev;           // Pointer of prev thread
  enum procstate state;        // Process state
  struct trapframe *tf;        // Trap frame for current syscall
  void *chan;                  // If non-zero, sleeping on chan
  char *kstack;                // Bottom of kernel stack for this process
  uint ustack;                 // Bottom of user stack for this thread
  void* retval;                // Return value
  struct proc *joinner;        // Thread that is waiting this thread
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
