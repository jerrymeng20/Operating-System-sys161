#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>

#include "opt-A2.h" /* required for A2 */

#if OPT_A2
#include <array.h>
#include <mips/trapframe.h>
#include <synch.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <mips/vm.h>
#endif /* OPT_A2 */

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;

#if OPT_A2
  /* update process with exit information */
  spinlock_acquire(&p->p_lock);
  p->exitcode = exitcode;
  p->alive = false;
  spinlock_release(&p->p_lock);

  if (p->p_parent != NULL) {
    /* signal the parent that waits on the wchan to wake up */
    cv_signal(p->cv_wake_parent, p->cv_lock);
  }

#else
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;
#endif /* OPT_A2 */

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

#if OPT_A2
  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  /* if this process has a living parent, instead of call proc_destroy(),
     we simply set its alive status to false */
  if (p->p_parent == NULL) {
    proc_destroy(p);
  }
  else if (!p->p_parent->alive) {
    proc_destroy(p);
  }
#else
  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
    proc_destroy(p);
#endif /* OPT_A2 */

  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  #if OPT_A2
  struct proc *p = curproc;

  *retval = p->pid;
  return(0);

  #else
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
  return(0);
  #endif /* OPT_A2 */
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

#if OPT_A2
  struct proc *p = curproc;

  /* verify the pid is one of curproc's children */
  struct proc *proc_c = search_pid(p, pid);
  if (proc_c == NULL) {
    // panic("Given pid: %d is not a valid child of parent %d.\n", (int) pid, (int) curproc->pid);
    return -1; /* error -1: not a valid child */
  }

  /* verify if sys_waitpid has been called on this process before */
  if (proc_c->hasWaited) {
    // panic("Given pid: %d has already been called on this child process before.\n", (int) pid);
    return -2; /* error -2: duplicated call on waitpid */
  }

  spinlock_acquire(&proc_c->p_lock);
  proc_c->hasWaited = true;
  spinlock_release(&proc_c->p_lock);

  /* 
   * if pass this point, the child must be in the children array
   */

  if (proc_c->alive) {
    /* 
     * the child process has not exited yet
     * make curproc go to sleep by keeping it waiting on the child process's conditional variable "cv_wake_parent"
     * the child process will use its conditional variable to wake the parent up when its execution has finished
     */
    lock_acquire(proc_c->cv_lock);
    cv_wait(proc_c->cv_wake_parent, proc_c->cv_lock);
    lock_release(proc_c->cv_lock);

    /* at this point, proc_c should not be alive */
    KASSERT(!proc_c->alive);
  } 

  /* proc_c's exitcode should be updated */
  exitstatus = proc_c->exitcode;

  exitstatus = _MKWAIT_EXIT(exitstatus);
 
  if (options != 0) {
    return(EINVAL);
  }
  
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);

#else
  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
#endif /* OPT_A2 */
}


int
sys_fork(struct trapframe *tf, pid_t *retval)
{
  char name_c[] = {'\0'};

  /* create a new process structure */
  struct proc *proc_c = proc_create_runprogram(name_c);
  if (proc_c == NULL) {
    // an error occured when create process structure
    panic("An error occured when creating process structure.\n");
    return -1;
  }

  /* create and copy the address space */
  struct addrspace *as_c = as_create();
  int as_cp = as_copy(curproc->p_addrspace, &as_c);
  if (as_cp != 0) {
    // an error occured when copy address space
    panic("An error occured when copying address space: error code %d.\n", as_cp);
    return -1;
  }
  spinlock_acquire(&proc_c->p_lock);
	proc_c->p_addrspace = as_c;
	spinlock_release(&proc_c->p_lock);

  /* setup parent/child relationship */
  attach_child(proc_c, curproc);

  /* create a thread for child process, and call enter_forked_process */
  char thread_name[] = {'\0'};

  // make a copy of tf on OS heap
	struct trapframe *tf_copy = (struct trapframe *) kmalloc(sizeof(struct trapframe));

	// copy parent's trapframe to OS heap
	*tf_copy = *tf;

  int t_fork = thread_fork(thread_name, proc_c, enter_forked_process, (void *) tf_copy, 0);
  /* enter_forked_process will setup child process (proc_c)'s trapframe */
  if (t_fork != 0) {
    // an error occured when forking thread
    panic("An error occured when forking thread: error code %d.\n", t_fork);
    return -1;
  }

  /* setup return value */
  *retval = proc_c->pid;

  return 0;
}


int 
sys_execv(const userptr_t progname, const userptr_t args)
{
  struct addrspace *as;
  struct addrspace *oldas;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

  /* total length of argv */
  int argv_total_len = 0;

  /* count number of args */
  int argc = 0;

  /* copy the program name from user addrspace to the kernel */
  int progNameLen = strlen((char *)progname);
  char *prog = kmalloc((progNameLen + 1) * sizeof(char));
  result = copyinstr(progname, (void *) prog, progNameLen + 1, NULL);
  if (result) {
    return result;
  }
  /* DEBUG: program name */
  // kprintf("\n");
  // kprintf("Program name: %s\n", prog);

  /* copy the address of first argument to the kernel */
  KASSERT(args != NULL);
  char **argAddr = kmalloc(sizeof(char*));     /* argAddr holds the address of argument */
  result = copyin(args, (void *) argAddr, sizeof(char*));
  if (result) {
    return result;
  }

  while (*argAddr != NULL) {
    /* a valid argument is found, copy the next address */
    argc++;
    result = copyin(args + argc * sizeof(char*), (void *) argAddr, sizeof(char*));
    if (result) {
      return result;
    }
  }

  /* since NULL ptr is argv[argc] and program name is argv[0], add 1 to argc */
  argc += 1;

  /* DEBUG: program argument number */
  //kprintf("Program Arg Number: %d\n", argc);
  
  /* allocate space for an array of pointer to the args in the kernel */
  char **argv = kmalloc((argc + 1) * sizeof(char *));

  /* recursively copy all args from argv[1] to arg[argc - 1] to the argv */
  for (int i = 1; i < argc; i++) {
    /* copy the ith argument into the argv's ith space */
    result = copyin(args + (i - 1) * sizeof(char*), (void *) argAddr, sizeof(char*));
    if (result) {
      return result;
    }
    char *actArg = kmalloc((strlen(*argAddr) + 1) * sizeof(char));
    strcpy(actArg, *argAddr);
    /* set the ith arg address point to this argument */
    argv[i] = actArg;
    /* add length of this argument to argv_total_len */
    argv_total_len += (strlen(argv[i]) + 1);
  }
  argv[0] = prog;
  argv_total_len += (strlen(argv[0]) + 1);
  argv[argc] = NULL;

  /* DEBUG: program arguments */
  //kprintf("args: ");
  //for (int i = 0; i <= argc; i++) {
  //kprintf("%s ", argv[i]);
  //}
  //kprintf("\n");

	/* Open the file. */
	result = vfs_open((char *) progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	oldas = curproc_setas(as);
	as_activate();

  /* delete the old address space */
  as_destroy(oldas);

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

  /* CRITICAL: copy arguments to user stack */
  /* --STEP 1--: Determine the space needed for argv and argv offset stack */
  int argv_space = ROUNDUP(argv_total_len * sizeof(char), 4);
  int offset_space = ROUNDUP((argc + 1) * sizeof(char *), sizeof(vaddr_t));

  /* move the stack pointer to beginning of argv offset, and set a pointer to that address (top of stack) */
  stackptr -= (argv_space + offset_space);

  userptr_t startptr = (userptr_t) stackptr;

  /* determine the stack address of current argument */
  userptr_t currArgAddr = startptr + offset_space;

  /* argvOffset[i] stores the offset to argv[i] */
  char **argvOffset = kmalloc((argc + 1) * sizeof(char *));

  size_t actual;

  /* --STEP 2--: copy all arguments (argv[0] to argv[argc - 1]) to the stack */
  for (int i = 0; i < argc; i++) {
    char *currArgv = argv[i];
    // kprintf("Current: %s\n", currArgv);

    // copy currArgv to currArgAddr at user stack
    result = copyoutstr(currArgv, currArgAddr, strlen(currArgv) + 1, &actual);
    // kprintf("Actual: %d\n", (int) actual);
    if (result) {
      return result;
    }

    argvOffset[i] = (char *) currArgAddr;

    // progress stack address tracker
    currArgAddr += actual * sizeof(char);
  }
  argvOffset[argc] = NULL;

  /* --STEP 3--: copy all argument offsets to the stack */
  result = copyout(argvOffset, startptr, (argc + 1) * sizeof(char *));
  if (result) {
    return result;
  }

  /* Now free the space allocated in the kernel */
  kfree(argvOffset);
  
  for (int i = 0; i < argc; i++) {
    kfree(argv[i]);
  }

  kfree(argAddr);

  kfree(argv);

	/* Warp to user mode. */
  /*
   * first parameter should be the number of arguments
   * second parameter should be the address to the arguments on the stack
   * third parameter should be the stackptr
   */
  enter_new_process(argc, startptr, stackptr, entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

