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

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
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
  p->tid = nexttid++;
  p->main_thread = p;
  p->prev = p;
  p->next = p;
  p->rt = p;
  p->Runnable = 0;
  p->memlimit = 0;
  p->stackpages = 0;
  release(&ptable.lock);

  // init ustacks as 0
  for(int i = 0; i<NPROC; i++)
    p->ustacks[i] = 0;

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    p->main_thread = 0;
    p->prev = 0;
    p->next = 0;
    p->rt = 0;
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
  return p;
}

static struct proc*
allocthread(void)
{
  struct proc *curthread = myproc();
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
  p->tid = nexttid++;
  
  // keep double linked list among thread for scheduling thread with RR 
  p->main_thread = curthread->main_thread;
  p->next = p->main_thread->rt;
  p->prev = p->next->prev;
  p->prev->next = p;
  p->next->prev = p;
  p->joinner = curthread;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    p->next->prev = p->prev;
    p->prev->next = p->next;
    p->prev = 0;
    p->next = 0;
    p->joinner = 0;
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
  
  initproc = p; // p is main_thread
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  p->ustack = p->sz;
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
  p->main_thread->Runnable++;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();
  acquire(&ptable.lock);

  sz = curproc->main_thread->sz;
  if(n > 0){
    if(curproc->main_thread->memlimit && curproc->main_thread->memlimit < sz + n)
      return -1;
    if((sz = allocuvm(curproc->main_thread->pgdir, sz, sz + n)) == 0){
      release(&ptable.lock);
      return -1;
    }
    int up = (sz + n) - PGROUNDUP(sz);
    if(up % PGSIZE == 0) curproc->main_thread->stackpages += (up / PGSIZE);
    else curproc->main_thread->stackpages += ((up / PGSIZE) + 1);
  }
  else if(n < 0){
    if((sz = deallocuvm(curproc->main_thread->pgdir, sz, sz + n)) == 0){
      release(&ptable.lock);
      return -1;
    }
    int down = (sz + n) - PGROUNDUP(sz);
    if(down % PGSIZE == 0) curproc->main_thread->stackpages -= (down / PGSIZE);
    else curproc->main_thread->stackpages -= ((down / PGSIZE) + 1);
  }
  curproc->main_thread->sz = sz;
  switchuvm(curproc);

  release(&ptable.lock);
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
  struct proc *curthread = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  // Copy process state from proc.
  if((np->pgdir = copyuvm(curthread->main_thread->pgdir, curthread->main_thread->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curthread->main_thread->sz;
  np->stackpages = curthread->main_thread->stackpages;
  np->parent = curthread->main_thread;
  *np->tf = *(curthread->tf);

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curthread->main_thread->ofile[i])
      np->ofile[i] = filedup(curthread->main_thread->ofile[i]);
  np->cwd = idup(curthread->main_thread->cwd);

  safestrcpy(np->name, curthread->main_thread->name, sizeof(curthread->main_thread->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  np->main_thread->Runnable++;

  release(&ptable.lock);

  return pid;
}

void
closefile(struct proc* p)
{
  struct proc* t;
  int fd;
  // Close all open files.
  for(t = p->next; t != p; t = t->next){
    if(t->state == ZOMBIE) continue;
    for(fd = 0; fd < NOFILE; fd++){
      if(t->ofile[fd]){
        fileclose(t->ofile[fd]);
        t->ofile[fd] = 0;
      }
    }
    begin_op();
    iput(t->cwd);
    end_op();
    t->cwd = 0;
  }
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

  //close files.
  closefile(curproc);
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
  wakeup1(curproc->main_thread->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc->main_thread){
      p->parent = initproc;
      if(p->main_thread->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  
  // Jump into the scheduler, never to return.
  curproc->main_thread->state = ZOMBIE;
  curproc->state = ZOMBIE;
  curproc->main_thread->Runnable = 0;
  sched();
  panic("zombie exit");
}

int thread_create(thread_t *thread, void*(*start_routine)(void*), void *arg){
  struct proc* curthread = myproc();
  struct proc* nt; // new thread
  uint sz, sp = 0;
  uint ustack[2]; // user stack

  if((nt = allocthread()) == 0){
    return -1;
  }
  acquire(&ptable.lock);

  int i;
  for(i = 0; i < NOFILE; i++)
    if(curthread->main_thread->ofile[i])
      nt->ofile[i] = filedup(curthread->main_thread->ofile[i]);
  nt->cwd = idup(curthread->main_thread->cwd);

  *nt->tf = *(curthread->tf);
  nt->tf->eax = 0;

  int idx;
  for(idx = 0; idx < NPROC; idx++)
    if(curthread->main_thread->ustacks[idx] != 0) break;

  if(idx != NPROC){
    // if already allocated ustack exists
    sp = curthread->main_thread->ustacks[idx];
    nt->ustack = sp;
    curthread->main_thread->ustacks[idx] = 0;
  }
  else {
    // allocate new user stack frame
    sz = PGROUNDUP(curthread->main_thread->sz);
    if(curthread->main_thread->memlimit && curthread->main_thread->memlimit < sz + 2*PGSIZE)
      goto bad;
    if((sz = allocuvm(curthread->main_thread->pgdir, sz, sz + 2*PGSIZE)) == 0)
      goto bad;
    clearpteu(curthread->main_thread->pgdir, (char*)(sz - 2*PGSIZE));
    curthread->main_thread->sz = sz;
    curthread->main_thread->stackpages += 2;
    // set bottom of thread's userstack
    nt->ustack = sz;
    sp = sz;
  }

  ustack[0] = 0xffffffff; // fake return
  ustack[1] = (uint)arg; // arg pointer
  sp -= 8;

  if(copyout(curthread->main_thread->pgdir, sp, ustack, 8) < 0)
    goto bad;
  
  //set new thread's PC to start_routine
  nt->tf->eip = (uint)start_routine;
  nt->tf->esp = sp;

  nt->state = RUNNABLE;
  nt->main_thread->Runnable++;
  *thread = nt->tid;
  release(&ptable.lock);
  return 0;

bad:
  kfree(nt->kstack);
  nt->kstack = 0;
  nt->state = UNUSED;
  nt->tid = 0;
  nt->pid = 0;

  // remove nt from linked list
  nt->next->prev = nt->prev;
  nt->prev->next = nt->next;
  nt->next = 0;
  nt->prev = 0;
  nt->main_thread = 0;

  if(sp > 0){
    // save userstack for later
    for(int i = 0; i < NPROC; i++)
      if(curthread->main_thread->ustacks[i] == 0){
        curthread->main_thread->ustacks[i] = sp + 8;
        break;
      }
  }
  release(&ptable.lock);
  return -1;
}

int thread_join(thread_t thread, void** retval) {
  struct proc *p;
  int havekids;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited thread.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->joinner != curproc || p->tid != thread)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        kfree(p->kstack);
        p->kstack = 0;
        for(int i = 0; i<NPROC; i++){
          // save userstack for another thread to begin later
          if(curproc->main_thread->ustacks[i] == 0){
            curproc->main_thread->ustacks[i] = p->ustack;
            break;
          }
        }
        p->ustack = 0;
        p->next->prev = p->prev;
        p->prev->next = p->next;
        p->main_thread = 0;
        p->prev = 0;
        p->next = 0;
        p->tid = 0;
        p->joinner = 0;
        p->killed = 0;
        p->state = UNUSED;
        *retval = p->retval;
        p->retval = 0;
        release(&ptable.lock);
        return 0;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for other thread to exit.  (See wakeup1 call in thread_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Exit the current thread. Does not return.
void thread_exit(void *retval){
  struct proc *curthread = myproc();
  struct proc *p;
  int fd;

  // if main_thread call thread_exit, force to call exit. 
  if(curthread == curthread->main_thread) {
    curthread->Runnable = 0;
    exit();
  }

  // close files
  for(fd = 0; fd < NOFILE; fd++){
    if(curthread->ofile[fd]){
      fileclose(curthread->ofile[fd]);
      curthread->ofile[fd] = 0;
    }
  }
  begin_op();
  iput(curthread->cwd);
  end_op();
  curthread->cwd = 0;

  acquire(&ptable.lock);

  // wakeup thread waiting to join curthread
  wakeup1(curthread->joinner);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curthread){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  curthread->state = ZOMBIE;
  curthread->retval = retval;
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
  struct proc *t;
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->main_thread->parent != curproc->main_thread)
        continue;
      havekids = 1;
      if(p->main_thread->state == ZOMBIE){
        // Found one whose main_thread is ZOMBIE.
        pid = p->main_thread->pid;
        for(t = p->next; t != p; t = t->next) {
          if(t->kstack != 0) kfree(t->kstack);
          t->kstack = 0;
          t->ustack = 0;
          t->tid = 0;
          t->killed = 0;
          t->joinner = 0;
          t->prev->next = 0;
          t->prev = 0;
          t->state = UNUSED;
          t->main_thread = 0;
        }
        kfree(p->kstack);
        p->kstack = 0;
        p->ustack = 0;
        freevm(p->main_thread->pgdir);
        p->pgdir = 0;
        p->pid = 0;
        p->tid = 0;
        for(int i = 0; i<NPROC; i++)
          p->main_thread->ustacks[i] = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        p->Runnable = 0;
        p->prev->next = 0;
        p->prev = 0;
        p->next = 0;
        p->main_thread = 0;
        p->joinner = 0;
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
    sleep(curproc->main_thread, &ptable.lock);  //DOC: wait-sleep
  }
}

struct proc*
find_runnable_thread(struct proc *p)
{
  struct proc *t;
  for(t = p->rt->next; t != p->rt; t = t->next) {
    if(t->state == RUNNABLE)
      goto found;
  }
  if(p->rt->state == RUNNABLE)
    goto found;
  return 0;
found:
  p->rt = t;
  return t;
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
  struct proc *p;
  struct proc *t;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p != p->main_thread || p->main_thread->Runnable == 0)
        continue;

      if((t = find_runnable_thread(p)) == 0)
        continue;
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = t;
      switchuvm(t);
      t->state = RUNNING;
      t->main_thread->Runnable--;
      swtch(&(c->scheduler), t->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
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
  struct proc *p = myproc();
  acquire(&ptable.lock);  //DOC: yieldlock
  p->state = RUNNABLE;
  p->main_thread->Runnable++;
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
  // do not decrease Runnable variable of main_thread
  // cuz already decreased in scheduler

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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      p->main_thread->Runnable++;
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
    if(p->main_thread->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
        p->main_thread->Runnable++;
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int
setmemorylimit(int pid, int limit)
{
  struct proc* p;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->main_thread->pid == pid){
      int ret = 0;
      if(p->main_thread->sz <= limit)
        p->main_thread->memlimit = limit;
      else ret = -1;
      release(&ptable.lock);
      return ret;
    }
  }
  release(&ptable.lock);
  return -1;
}

void
printlist(void)
{
  struct proc* p;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->main_thread != p || p->main_thread->state == ZOMBIE || p->main_thread->state == UNUSED) continue;
    cprintf("name: %s, pid: %d, stack pages: %d, ustack size: %d, memlimit: %d\n",
      p->name, p->pid, p->stackpages, p->sz, p->memlimit);
  }
  release(&ptable.lock);
}

// make clearthread in proc.c to reset thread information with ptable lock
void
clearthread(struct proc* curproc)
{
  struct proc* t;
  int fd;

  // close opened files of thread
  for(t = curproc->next; t != curproc; t = t->next) {
    if(t->state != ZOMBIE){
      for(fd = 0; fd < NOFILE; fd++) {
        if(t->ofile[fd]) {
          fileclose(t->ofile[fd]);
          t->ofile[fd] = 0;
        }
      }
      begin_op();
      iput(t->cwd);
      end_op();
      t->cwd = 0;
    }
  }

  acquire(&ptable.lock);
  // copy main_thread's info to curproc to make curproc mainthread
  curproc->pid = curproc->main_thread->pid;
  curproc->parent = curproc->main_thread->parent;
  curproc->joinner = curproc->main_thread->joinner;
  curproc->memlimit = curproc->main_thread->memlimit;
  curproc->stackpages = curproc->main_thread->stackpages;
  
  // Reset ustacks
  for(int i = 0; i<NPROC; i++)
    curproc->main_thread->ustacks[i] = 0;

  for(t = curproc->next; t != curproc; t = t->next){
    if(t->kstack != 0) kfree(t->kstack);
    t->kstack = 0;
    t->ustack = 0;
    t->state = UNUSED;
    t->tid = 0;
    t->killed = 0;
    t->main_thread = 0;
    t->prev->next = 0;
    t->prev= 0;
    t->joinner = 0;
  }
  curproc->prev->next = 0;
  curproc->prev = 0;
  curproc->main_thread = curproc;
  curproc->next = curproc;
  curproc->prev = curproc;
  curproc->rt = curproc;
  curproc->Runnable = 0;

  release(&ptable.lock);
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
    cprintf("%d %s %s", p->main_thread->pid, state, p->main_thread->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
