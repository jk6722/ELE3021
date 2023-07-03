#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
extern int sys_uptime(void);

struct spinlock mlfq_ticks_lock;
uint mlfq_ticks; //global tick for mlfq_scheduler

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  struct proc* vip; // Very Important Process
} ptable;

struct {
  struct spinlock lock;
  int queue[NPROC]; // contain process's idx in ptable as queue
  int q_size;
} qtable;

static struct proc *initproc;

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&mlfq_ticks_lock, "mlfqtime");
}

void
qinit(void)
{
  // init qtable at first
  initlock(&qtable.lock, "qtable");
  acquire(&qtable.lock);
  for(int i = 0; i < NPROC; i++)
    qtable.queue[i] = -1; // -1 means empty
  release(&qtable.lock);
}

void
enq(int p_idx)
{
  acquire(&qtable.lock);
  qtable.queue[qtable.q_size] = p_idx;
  // push new process in queue
  qtable.q_size++;
  // increase queue size
  release(&qtable.lock);
}

void
deq(int p_idx)
{
  int q_idx = 0;
  acquire(&qtable.lock);
  for(int i = 0; i < NPROC; i++){
    if(qtable.queue[i] == p_idx){
      q_idx = i;
      qtable.queue[i] = -1;
      break;
    }
  }
  // When an empty space occurs, pull the elements one by one.
  // To keep the queue dense (non blank)
  for(int i = q_idx + 1; i < NPROC; i++){
    if(qtable.queue[i] == -1)
      break;
    qtable.queue[i-1] = qtable.queue[i];
  }
  qtable.q_size--;
  release(&qtable.lock);
}

void
push_front(int q_idx, int p_idx)
{
  // push process's index in front of queue
  int p_idx_in_q = 0;
  acquire(&qtable.lock);
  for(int i = 0; i < qtable.q_size; i++){
    if(qtable.queue[i] == p_idx){
      // find process's queue index not p_index
      p_idx_in_q = i;
      break;
    }
  }
  if(p_idx_in_q < q_idx){
    // pull front
    for(int i = p_idx_in_q; i < q_idx; i++)
      qtable.queue[i] = qtable.queue[i+1];
  }
  else if(q_idx < p_idx_in_q){
    // pull back
    for(int i = p_idx_in_q; i > q_idx; i--)
      qtable.queue[i] = qtable.queue[i-1];
  }
  qtable.queue[q_idx] = p_idx;
  // move p_idx to the location where it can run first at the next schedling.
  release(&qtable.lock);
}

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
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
  int idx = 0;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED){
      p->p_idx = idx;
      goto found;
    }
    idx++;
  }
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO; // EMBRYO == new state
  p->pid = nextpid++;
  p->qlevel = 0;
  p->time_quantum = 4;
  p->priority = 3;
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
  enq(p->p_idx);
  // enqueue new process's index
  return p;
}


//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

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

// move process form high level queue to low level queue
// if time quantum runs out
void move_queue(struct proc* p) {
  if(p->qlevel == 2){
    // if process is in L2
    // do not need to move
    p->priority = p->priority == 0 ? 0 : p->priority - 1;
    p->time_quantum = 2 * p->qlevel + 4;
    return;
  }
  p->qlevel++;
  p->time_quantum = 2* p->qlevel + 4;
  // else move queue and set time quantum newly
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
  curproc->sz = sz;
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
  // Do not need to enqueue child process because fork syscall use allocproc()

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

  np->priority = curproc->priority; // copy priority
  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);
  
  // init child process
  np->state = RUNNABLE;
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
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
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

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  deq(curproc->p_idx);
  // Jump into the scheduler, never to return.
  if(ptable.vip == curproc)
    ptable.vip = 0;
  // if locked process exits, process should release the lock.

  // When process is terminated, process's state should be ZOMBIE not UNUSED
  // Because parent process will handle ZOMBIE child
  curproc->state = ZOMBIE;
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

int remove_vip(struct proc* p) {
  if(p != ptable.vip)
    return 0; // failed to remove
  ptable.vip = 0;
  return 1; // succeeded to remove
}

int is_lockedProcess(){
  int ret = ptable.vip != 0 ? 1 : 0;
  return ret;
}

void boosting(void) {
  for(struct proc* p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(ptable.vip != p){
      // locked process doesn't need to be boosted
      p->time_quantum = 4;
      p->qlevel = 0;
    }
    p->priority = 3;
  }
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
mlfq_scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  for(;;){
    // Enable interrupts on this processor.
    sti();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    struct proc* next;
    struct proc* temp;
    for(uint i = 0; i < qtable.q_size; i++){
      p = &ptable.proc[qtable.queue[i]];
      if(p->state != RUNNABLE)
        continue;
      next = p;
      // find the process that should be excuted next
      if(ptable.vip != 0)
        next = ptable.vip;
      // if there is a process having lock, select that process unconditionally
      // if p is in L0 queue. p should be executed else find process to be executed
      else if(p->qlevel != 0){ 
        for(uint j = 0; j < qtable.q_size; j++){
          temp = &ptable.proc[qtable.queue[j]];
          if(temp->state != RUNNABLE)
            continue;
          else if(temp->qlevel == 0){
            next = temp;
            break;
          }
          else if(next->qlevel > temp->qlevel){
            next = temp;
          }
          else if(next->qlevel == 2 && next->priority > temp->priority)
            next = temp;
        }
      }
      // scheduling finished
      // reduce process's time_quantum
      next->time_quantum--;
      if(next->time_quantum == 0){
        if(next == ptable.vip){
          // process spent all 100 ticks acquired by calling schedulerLock() 
          // should go to L0 queue again
          next->qlevel = 0;
          next->time_quantum = 4;
          next->priority = 3;
          ptable.vip = 0;
          push_front((i + 1) % qtable.q_size, next->p_idx);
          // move p to front of the L0 queue
        }
        else move_queue(next);
      }
      next->q_idx_push = (i + 1) % qtable.q_size;
      c->proc = next;
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      uint prev_ticks = sys_uptime();
      switchuvm(next);
      // Load process 'next' on memory
      // TSS: task state segment
      next->state = RUNNING;
      swtch(&(c->scheduler), next->context);
      // switch context from scheduler to process
      switchkvm();
      // Reload kernel memory when CPU is returned to scheduler
      c->proc = 0;
      acquire(&mlfq_ticks_lock);
      mlfq_ticks += (sys_uptime() - prev_ticks); // update mlfq_ticks using sys_uptime() system call
      if(mlfq_ticks % 100 == 0){
        // call boosting() when global tick reaches 100
        boosting();
        mlfq_ticks = 0;
      }
      release(&mlfq_ticks_lock);
    } 
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
  // activate sheduler when the process doesn't use CPU
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
  // context switch from process to scheduler
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();  // activate scheduler
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
  // if locked process sleep, it should return lock before sleeping
  if(ptable.vip == p){
    ptable.vip = 0;
    p->qlevel = 0;
    p->time_quantum = 4;
    p->priority = 3;
  }
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
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      return;
    }
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
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
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

void
setPriority(int pid, int priority)
{
  struct proc* p;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->priority = priority;
      break;
    }
  }
  release(&ptable.lock);
}

int
print_status(void)
{
  struct proc* p;
  sti();
  acquire(&ptable.lock);
  cprintf("name \t pid \t state \t \t level \t priority \t pidx\n");
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == SLEEPING)
      cprintf("%s \t %d \t SLEEPING \t %d \t %d \t\t %d\n", p->name, p->pid, p->qlevel, p->priority, p->p_idx);
    else if(p->state == RUNNING)
      cprintf("%s \t %d \t RUNNING \t %d \t %d \t\t %d\n", p->name, p->pid, p->qlevel, p->priority, p->p_idx);
    else if(p->state == RUNNABLE)
      cprintf("%s \t %d \t RUNNABLE \t %d \t %d \t\t %d\n", p->name, p->pid, p->qlevel, p->priority, p->p_idx);
  }
  release(&ptable.lock);
  return 28;
}

void
schedulerLock(int password)
{
  struct proc* p = myproc();
  acquire(&ptable.lock);
  if(password == 2019049716){
    if(is_lockedProcess()) {
      // check if there is a already locked process
      cprintf("already locked process!\n");
      release(&ptable.lock);
      return;
    }
    acquire(&mlfq_ticks_lock);
    mlfq_ticks = 0;
    // init global tick when schedulerLock is called
    release(&mlfq_ticks_lock);
    p->qlevel = 3; // just for checking lock in mlfq_test.c
    p->time_quantum = 100;
    ptable.vip = p; // register p as vip
    release(&ptable.lock);
    yield(); // return cpu to scheduler, then locked process will be scheduled
  }
  else{
    // if password is wrong
    cprintf("wrong password\n");
    cprintf("pid: %d, timequantum: %d, qlevel: %d\n", p->pid, p->time_quantum, p->qlevel);
    release(&ptable.lock);
    kill(p->pid);
    // kill that process in exchange for the wrong password
  }
}


void schedulerUnlock(int password) {
  struct proc* p = myproc();
  acquire(&ptable.lock);
  if(password != 2019049716){
    cprintf("unlock err: wrong password\n");
    cprintf("pid %d timequantum %d qlevel %d\n", p->pid, p->time_quantum, p->qlevel);
    release(&ptable.lock);
    kill(p->pid);
  }
  else if(is_lockedProcess() && remove_vip(p)){
    // if locked process exists and locked process is same with myproc
    // succeed to unlock
    p->qlevel = 0;
    p->priority = 3;
    p->time_quantum = 4;
    push_front(p->q_idx_push, p->p_idx);
    // push process in front of L0 queue
    cprintf("unlocked!\n");
    release(&ptable.lock);
    yield();
  }
  else {
    cprintf("unlock err: process doesn't have lock\n");
    release(&ptable.lock);
  }
}