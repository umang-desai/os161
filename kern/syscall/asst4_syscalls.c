/*
 * Process system calls implementation.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <lib.h>
#include <mips/trapframe.h>
#include <uio.h>
#include <proc.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <syscall.h>
#include <addrspace.h>
#include <vnode.h>
#include <vfs.h>
#include <openfile.h>
#include <kern/fcntl.h>
#include <test.h>
#include <filetable.h>

static void thread_init(void * tf,unsigned long data2){
	struct trapframe temp_tf;

	struct addrspace *as = (struct addrspace *) data2;
	proc_setas(as);
	as_activate();

	temp_tf = *(struct trapframe *)tf;
	enter_forked_process(&temp_tf);
} 

int
sys_getpid(pid_t *retval)
{	
	*retval = curproc->pid;
	return 0;
}

/*
 * Forks a new process from the current calling process. 
 */
int
sys_fork(struct trapframe *tf,pid_t *pid)
{
	int err;
	struct proc *forkproc;
	struct trapframe *fork_tf;

	/*Create new process a new process.Takes care of filetable.*/
	err = proc_fork(&forkproc);
	if(err){
	return err;
	}

	//Copy address space
	err = as_copy(curproc->p_addrspace,&forkproc->p_addrspace);
	if(err){
		kfree(&forkproc);	
		return err;
	}

	forkproc->ppid = curproc->pid;
	fork_tf = kmalloc(sizeof(struct trapframe));
	if(tf == NULL){
		kfree(forkproc);
		return ENOMEM;
	}
	*fork_tf = *tf;
	thread_fork("forked_child",forkproc,thread_init,(void *)fork_tf,(unsigned long)forkproc->p_addrspace);
	
	*pid = forkproc->pid;
	return 0;
}

/*
 * Method called by a process when it wants to wait on its child. The child is specified by the pid argument passed to this method.
 */
int
sys_waitpid(pid_t pid, int *returncode, int flags, pid_t *retval)
{
	if(returncode == NULL){
		return EINVAL;
	}	

	if(flags != 0 && flags!= WNOHANG){
		kprintf("Sorry, options no right now.");
		return EINVAL; 
	}

	if(pid > PID_MAX || pid < PID_MIN){
                return ESRCH;
        }
	
	if(pid == curproc->pid){
		kprintf("me myself and i. cant wait on myself.\n");
		return EINVAL;
	}
	
	if(proc_list[pid] == NULL){
		return ESRCH;
	}	

	if(proc_list[pid]->ppid != curproc->pid){
		kprintf("is not our child bro!\n");
		return ECHILD;
	}

	lock_acquire(proc_list_lock);
	if(!proc_list[pid]->exitdone){
		cv_wait(proc_list[pid]->cv_waitpid, proc_list_lock);
		KASSERT(proc_list[pid]->exitdone);
	}	
	lock_release(proc_list_lock);	

	*returncode = proc_list[pid]->exitcode;
	proc_destroy(proc_list[pid]);
	*retval = pid;

	return 0;	
}

/*
 * Called when a process wants to exit. 
 */
void
sys__exit(int exitcode)
{
	lock_acquire(proc_list_lock);

	for (int i =0; i < PID_MAX; i++){
		if (proc_list[i] != NULL && proc_list[i]->ppid == curproc->pid){
			proc_list[i]->ppid = KPROC_PID;
			if (proc_list[i]->exitdone) {
				proc_destroy(proc_list[i]);
			}
		}
	}
		
	spinlock_acquire(&curproc->p_lock);
	curproc->exitdone = true;
	curproc->exitcode = _MKWAIT_EXIT(exitcode);		
	cv_signal(curproc->cv_waitpid, proc_list_lock);
	spinlock_release(&curproc->p_lock);
	lock_release(proc_list_lock);

	thread_exit();
}

int 
sys_execv(char *program,char **args){
//	(void) args;

//	kprintf("\nexecv is STARTING. \n");
	struct addrspace *as;
        struct vnode *v;
        vaddr_t entrypoint, stackptr;
	int result;
	
	//Checking if arguments exist. If so, then set it to true.
	bool argument = (args != NULL);
  	
	size_t argc = 0;
 	
	//Count the total number of arguments and track it using argc
	if(argument){
    	    while(true){
      		char *argtemp = NULL;
      		copyin((const_userptr_t)args[argc], &argtemp, sizeof(char*));
      
     		 if(argtemp != NULL){
        //		kprintf("\nEXECV TEST : CHECKING ARGUMENT -> %d - %s\n",argc,args[argc]);
			argc++;
      		}
      		else{
        		break;
      		}
    	    }
  	}	
	
	//Create pointer to store arguments and copy them into kernel space
	char **kern_args = kmalloc(sizeof(char *) * (argc+1));
  	if(kern_args == NULL){
    		return ENOMEM;
 	}
	
	for(size_t i = 0; i < argc; i++){
 	   	kern_args[i] = kmalloc(ARG_MAX);
    		char *argtemp;
    		argtemp = kern_args[i];
    		size_t arg_len;
    		
		result = copyinstr((const_userptr_t)args[i], kern_args[i], ARG_MAX, &arg_len);
    		if(result) {
      		return result;
    		}
  	}
//	kprintf("EXECV TEST : CHECKING ARGUMENT -> 0 - %s",(char *) kern_args[0]);
 	//setting NULL terminate in the pointer 
  	kern_args[argc] = NULL;

	/*Destroy address space of current process, so we can create one for the new program.*/
	as_destroy(curproc->p_addrspace);
        curproc->p_addrspace = NULL;
	
	result = vfs_open(program, O_RDONLY, 0, &v);
        if (result) {
                return result;
        }
	
	KASSERT(proc_getas() == NULL);

	if (curproc->p_filetable == NULL) {
                curproc->p_filetable = filetable_create();
                if (curproc->p_filetable == NULL) {
                        vfs_close(v);
                        return ENOMEM;
                }

                result = open_stdfds("con:", "con:", "con:");
                if (result) {
                        vfs_close(v);
                        return result;
                }
      	}

	as = as_create();
        if (as == NULL) {
                vfs_close(v);
                return ENOMEM;
        }	

	proc_setas(as);
        as_activate();

	result = load_elf(v, &entrypoint);
        if (result) {
	        vfs_close(v);
                return result;
        }	

	vfs_close(v);

	result = as_define_stack(as, &stackptr);
        if (result) {
		return result;
	}

	
	vaddr_t args_stk_addr[argc];
  	/* Need to NULL terminate */
  	args_stk_addr[argc] = 0;
	
	/* Copy argument strings first, do not copy NULL one */
  	for(int i = (int)argc-1; i >= 0; i--){
    		size_t arg_len=strlen(kern_args[i])+1;
    		/* When storing items on the stack, pad each item such that 
 	 	*they are 8-byte aligned
 	 	**/
    		stackptr -= ROUNDUP(arg_len,8);
    		copyoutstr(kern_args[i], (userptr_t)stackptr, ARG_MAX, &arg_len);
    
    		if(result) {
      			return result;
    		}
    		args_stk_addr[i] = stackptr;
  	}

	/* Copy argument pointers */
	for (int i = (int)argc; i >= 0; i--){
 		stackptr -= sizeof(vaddr_t);
    		result = copyout(&args_stk_addr[i], (userptr_t)stackptr, sizeof(vaddr_t));
    		if(result) {
      			return result;
    		}
  	}
  	vaddr_t userspace_addr = 0;
  	if(argument){
    		userspace_addr = stackptr;
  	}

	for(size_t i = 0; i < argc; i++){
    		kfree(kern_args[i]);
    		kern_args[i] = NULL;
  	}
 	kfree(kern_args);
  	kern_args = NULL;
	
//	kprintf("EXECV TEST : CHECKING ARGUMENT -> 0 - %s",userspace_addr[0]);
	
	enter_new_process(argc /*argc*/, (userptr_t) userspace_addr /*userspace addr of argv*/,
                          NULL /*userspace addr of environment*/,
                          stackptr, entrypoint);

	panic("enter_new_process returned\n");
        return EINVAL;
}
