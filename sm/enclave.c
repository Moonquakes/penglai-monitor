#include "enclave.h"
#include "enclave_vm.h"
#include "sm.h"
#include "math.h"
#include <string.h>
#include TARGET_PLATFORM_HEADER

static struct cpu_state_t cpus[MAX_HARTS] = {{0,}, };

//spinlock
static spinlock_t enclave_metadata_lock = SPINLOCK_INIT;
void acquire_enclave_metadata_lock()
{
  spinlock_lock(&enclave_metadata_lock);
}
void release_enclave_metadata_lock()
{
  spinlock_unlock(&enclave_metadata_lock);
}

//enclave metadata
struct link_mem_t* enclave_metadata_head = NULL;
struct link_mem_t* enclave_metadata_tail = NULL;

uintptr_t copy_from_host(void* dest, void* src, size_t size)
{
  memcpy(dest, src, size);
  return 0;
}

uintptr_t copy_to_host(void* dest, void* src, size_t size)
{
  memcpy(dest, src, size);
  return 0;
}

int copy_word_to_host(unsigned int* ptr, uintptr_t value)
{
  *ptr = value;
  return 0;
}

int copy_dword_to_host(uintptr_t* ptr, uintptr_t value)
{
  *ptr = value;
  return 0;
}

static void enter_enclave_world(int eid)
{
  cpus[read_csr(mhartid)].in_enclave = 1;
  cpus[read_csr(mhartid)].eid = eid;

  platform_enter_enclave_world();
}

static int get_enclave_id()
{
  return cpus[read_csr(mhartid)].eid;
}

int get_curr_enclave_id()
{
  return cpus[read_csr(mhartid)].eid;
}

static void exit_enclave_world()
{
  cpus[read_csr(mhartid)].in_enclave = 0;
  cpus[read_csr(mhartid)].eid = -1;

  platform_exit_enclave_world();
}

int check_in_enclave_world()
{
  if(!(cpus[read_csr(mhartid)].in_enclave))
    return -1;

  if(platform_check_in_enclave_world() < 0)
    return -1;

  return 0;
}

static int check_enclave_authentication()
{
  if(platform_check_enclave_authentication() < 0)
    return -1;

  return 0;
}

static void switch_to_enclave_ptbr(struct thread_state_t* thread, uintptr_t ptbr)
{
  platform_switch_to_enclave_ptbr(thread, ptbr);
}

static void switch_to_host_ptbr(struct thread_state_t* thread, uintptr_t ptbr)
{
  platform_switch_to_host_ptbr(thread, ptbr);
}

struct link_mem_t* init_mem_link(unsigned long mem_size, unsigned long slab_size)
{
  int retval = 0;
  struct link_mem_t* head;

  head = (struct link_mem_t*)mm_alloc(mem_size, NULL);
  
  if(head == NULL)
    return NULL;
  else
    memset((void*)head, 0, mem_size);

  head->mem_size = mem_size;
  head->slab_size = slab_size;
  head->slab_num = (mem_size - sizeof(struct link_mem_t)) / slab_size;
  void* align_addr = (char*)head + sizeof(struct link_mem_t);
  head->addr = (char*)size_up_align((unsigned long)align_addr, slab_size);
  head->next_link_mem = NULL;

  return head;
}

struct link_mem_t* add_link_mem(struct link_mem_t** tail)
{
  struct link_mem_t* new_link_mem;
  int retval = 0;

  new_link_mem = (struct link_mem_t*)mm_alloc((*tail)->mem_size, NULL);

  if (new_link_mem == NULL)
    return NULL;
  else
    memset((void*)new_link_mem, 0, (*tail)->mem_size);

  (*tail)->next_link_mem = new_link_mem;
  new_link_mem->mem_size = (*tail)->mem_size;
  new_link_mem->slab_num = (*tail)->slab_num;
  new_link_mem->slab_size = (*tail)->slab_size;
  void* align_addr = (char*)new_link_mem + sizeof(struct link_mem_t);
  new_link_mem->addr = (char*)size_up_align((unsigned long)align_addr, (*tail)->slab_size);
  new_link_mem->next_link_mem = NULL;

  return new_link_mem;
}

int remove_link_mem(struct link_mem_t** head, struct link_mem_t* ptr)
{
  struct link_mem_t *cur_link_mem, *tmp_link_mem;
  int retval =0;

  cur_link_mem = *head;
  if (cur_link_mem == ptr)
  {
    *head = cur_link_mem->next_link_mem;
    mm_free(cur_link_mem, cur_link_mem->mem_size);
    return 1;
  }

  for(cur_link_mem; cur_link_mem != NULL; cur_link_mem = cur_link_mem->next_link_mem)
  {
    if (cur_link_mem->next_link_mem == ptr)
    {
      tmp_link_mem = cur_link_mem->next_link_mem;
      cur_link_mem->next_link_mem = cur_link_mem->next_link_mem->next_link_mem;
      //FIXME
      mm_free(tmp_link_mem, tmp_link_mem->mem_size);
      return retval;
    }
  }

  return retval;
}

/** 
 * \brief alloc an enclave_t structure from encalve_metadata_head
 * 
 * eid represents the location in the list
 * sometimes you may need to acquire lock before calling this function
 */
struct enclave_t* alloc_enclave()
{
  struct link_mem_t *cur, *next;
  struct enclave_t* enclave = NULL;
  int i, found, eid;

  //enclave metadata list hasn't be initialized yet
  if(enclave_metadata_head == NULL)
  {
    enclave_metadata_head = init_mem_link(ENCLAVE_METADATA_REGION_SIZE, sizeof(struct enclave_t));
    if(!enclave_metadata_head)
    {
      printm("M mode: alloc_enclave: don't have enough mem\r\n");
      goto alloc_eid_out;
    }
    enclave_metadata_tail = enclave_metadata_head;
  }

  found = 0;
  eid = 0;
  for(cur = enclave_metadata_head; cur != NULL; cur = cur->next_link_mem)
  {
    for(i = 0; i < (cur->slab_num); i++)
    {
      enclave = (struct enclave_t*)(cur->addr) + i;
      if(enclave->state == INVALID)
      {
        memset((void*)enclave, 0, sizeof(struct enclave_t));
        enclave->state = FRESH;
        enclave->eid = eid;
        found = 1;
        break;
      }
      eid++;
    }
    if(found)
      break;
  }

  //don't have enough enclave metadata
  if(!found)
  {
    next = add_link_mem(&enclave_metadata_tail);
    if(next == NULL)
    {
      printm("M mode: alloc_enclave: don't have enough mem\r\n");
      enclave = NULL;
      goto alloc_eid_out;
    }
    enclave = (struct enclave_t*)(next->addr);
    memset((void*)enclave, 0, sizeof(struct enclave_t));
    enclave->state = FRESH;  
    enclave->eid = eid;
  }

alloc_eid_out:
  return enclave;
}

//sometimes you may need to acquire lock before calling this function
int free_enclave(int eid)
{
  struct link_mem_t *cur, *next;
  struct enclave_t *enclave = NULL;
  int i, found, count, ret_val;

  found = 0;
  count = 0;
  for(cur = enclave_metadata_head; cur != NULL; cur = cur->next_link_mem)
  {
    if(eid < (count + cur->slab_num))
    {
      enclave = (struct enclave_t*)(cur->addr) + (eid - count);
      memset((void*)enclave, 0, sizeof(struct enclave_t));
      enclave->state = INVALID;
      found = 1;
      ret_val = 0;
      break;
    }
    count += cur->slab_num;
  }

  //haven't alloc this eid 
  if(!found)
  {
    printm("M mode: free_enclave: haven't alloc this eid\r\n");
    ret_val = -1;
  }

  return ret_val;
}

//sometimes you may need to acquire lock before calling this function
struct enclave_t* get_enclave(int eid)
{
  struct link_mem_t *cur, *next;
  struct enclave_t *enclave;
  int i, found, count;

  found = 0;
  count = 0;
  for(cur = enclave_metadata_head; cur != NULL; cur = cur->next_link_mem)
  {
    if(eid < (count + cur->slab_num))
    {
      enclave = (struct enclave_t*)(cur->addr) + (eid - count);
      found = 1;
      break;
    }

    count += cur->slab_num;
  }

  //haven't alloc this eid 
  if(!found)
  {
    printm("M mode: get_enclave: haven't alloc this enclave\r\n");
    enclave = NULL;
  }

  return enclave;
}

/**
 * \brief this function is used to handle IPC in enclave,
 * 	  it will return the last enclave in the chain.
 * 	  This is used to help us identify the real executing encalve.
 */
struct enclave_t* __get_real_enclave(int eid)
{
  struct enclave_t* enclave = get_enclave(eid);
  if(!enclave)
    return NULL;

  struct enclave_t* real_enclave = NULL;
  if(enclave->cur_callee_eid == -1)
    real_enclave = enclave;
  else
    real_enclave = get_enclave(enclave->cur_callee_eid);

  return real_enclave;
}

int swap_from_host_to_enclave(uintptr_t* host_regs, struct enclave_t* enclave)
{
  //grant encalve access to memory
  if(grant_enclave_access(enclave) < 0)
    return -1;

  //save host context
  swap_prev_state(&(enclave->thread_context), host_regs);

  //different platforms have differnt ptbr switch methods
  switch_to_enclave_ptbr(&(enclave->thread_context), enclave->thread_context.encl_ptbr);

  //save host trap vector
  swap_prev_stvec(&(enclave->thread_context), read_csr(stvec));

  //TODO: save host cache binding
  //swap_prev_cache_binding(&enclave -> threads[0], read_csr(0x356));

  //disable interrupts
  swap_prev_mie(&(enclave->thread_context), read_csr(mie));
  clear_csr(mip, MIP_MTIP);
  clear_csr(mip, MIP_STIP);
  clear_csr(mip, MIP_SSIP);
  clear_csr(mip, MIP_SEIP);

  //disable interrupts/exceptions delegation
  swap_prev_mideleg(&(enclave->thread_context), read_csr(mideleg));
  swap_prev_medeleg(&(enclave->thread_context), read_csr(medeleg));

  //swap the mepc to transfer control to the enclave
  swap_prev_mepc(&(enclave->thread_context), read_csr(mepc)); 

  //set mstatus to transfer control to u mode
  uintptr_t mstatus = read_csr(mstatus);
  mstatus = INSERT_FIELD(mstatus, MSTATUS_MPP, PRV_U);
  write_csr(mstatus, mstatus);

  //mark that cpu is in enclave world now
  enter_enclave_world(enclave->eid);

  __asm__ __volatile__ ("sfence.vma" : : : "memory");

  return 0;
}

int swap_from_enclave_to_host(uintptr_t* regs, struct enclave_t* enclave)
{
  //retrieve enclave access to memory
  retrieve_enclave_access(enclave);

  //restore host context
  swap_prev_state(&(enclave->thread_context), regs);

  //restore host's ptbr
  switch_to_host_ptbr(&(enclave->thread_context), enclave->host_ptbr);

  //restore host stvec
  swap_prev_stvec(&(enclave->thread_context), read_csr(stvec));

  //TODO: restore host cache binding
  //swap_prev_cache_binding(&(enclave->thread_context), );
  
  //restore interrupts
  swap_prev_mie(&(enclave->thread_context), read_csr(mie));

  //restore interrupts/exceptions delegation
  swap_prev_mideleg(&(enclave->thread_context), read_csr(mideleg));
  swap_prev_medeleg(&(enclave->thread_context), read_csr(medeleg));

  //transfer control back to kernel
  swap_prev_mepc(&(enclave->thread_context), read_csr(mepc));

  //restore mstatus
  uintptr_t mstatus = read_csr(mstatus);
  mstatus = INSERT_FIELD(mstatus, MSTATUS_MPP, PRV_S);
  write_csr(mstatus, mstatus);

  //mark that cpu is out of enclave world now
  exit_enclave_world();

  __asm__ __volatile__ ("sfence.vma" : : : "memory");

  return 0;
}

uintptr_t create_enclave(struct enclave_sbi_param_t create_args)
{
  struct enclave_t* enclave = NULL;
  uintptr_t ret = 0;

acquire_enclave_metadata_lock();

  enclave = alloc_enclave();
  if(!enclave)
  {
     printm("M mode: create_enclave: enclave allocation is failed \r\n");
     return -1UL;
  }

  //TODO: check whether enclave memory is out of bound
  //TODO: verify enclave page table layout
  enclave->paddr = create_args.paddr;
  enclave->size = create_args.size;
  enclave->entry_point = create_args.entry_point;
  enclave->untrusted_ptr = create_args.untrusted_ptr;
  enclave->untrusted_size = create_args.untrusted_size;
  enclave->free_mem = create_args.free_mem;
  enclave->ocall_func_id = create_args.ecall_arg0;
  enclave->ocall_arg0 = create_args.ecall_arg1;
  enclave->ocall_arg1 = create_args.ecall_arg2;
  enclave->ocall_syscall_num = create_args.ecall_arg3;
  enclave->kbuffer = create_args.kbuffer;
  enclave->kbuffer_size = create_args.kbuffer_size;
  enclave->host_ptbr = read_csr(satp);
  enclave->root_page_table = create_args.paddr + RISCV_PGSIZE;
  enclave->thread_context.encl_ptbr = ((create_args.paddr + RISCV_PGSIZE) >> RISCV_PGSHIFT) | SATP_MODE_CHOICE;
  enclave->state = FRESH;
  enclave->caller_eid = -1;
  enclave->top_caller_eid = -1;
  enclave->cur_callee_eid = -1;

    //traverse vmas
  struct pm_area_struct* pma = (struct pm_area_struct*)(create_args.paddr);
  struct vm_area_struct* vma = (struct vm_area_struct*)(create_args.paddr + sizeof(struct pm_area_struct));
  pma->paddr = create_args.paddr;
  pma->size = create_args.size;
  pma->free_mem = create_args.free_mem;
  if(pma->free_mem < pma->paddr || pma->free_mem >= pma->paddr+pma->size
     || pma->free_mem & ((1<<RISCV_PGSHIFT) - 1))
  {
    ret = ENCLAVE_ERROR;
    printm("fail: ENCLAVE_ERROR");
    return ret;
  }
  pma->pm_next = NULL;
  enclave->pma_list = pma;
  traverse_vmas(enclave->root_page_table, vma);

  //FIXME: here we assume there are exactly text(include text/data/bss) vma and stack vma
  while(vma)
  {
    if(vma->va_start == ENCLAVE_DEFAULT_TEXT_BASE)
    {
      enclave->text_vma = vma;
    }
    if(vma->va_end == ENCLAVE_DEFAULT_STACK_BASE)
    {
      enclave->stack_vma = vma;
      enclave->_stack_top = enclave->stack_vma->va_start;
    }
    vma->pma = pma;
    vma = vma->vm_next;
  }
  if(enclave->text_vma)
    enclave->text_vma->vm_next = NULL;
  if(enclave->stack_vma)
    enclave->stack_vma->vm_next = NULL;
  enclave->_heap_top = ENCLAVE_DEFAULT_HEAP_BASE;
  enclave->heap_vma = NULL;
  enclave->mmap_vma = NULL;

  enclave->free_pages = NULL;
  enclave->free_pages_num = 0;
  uintptr_t free_mem = create_args.paddr + create_args.size - RISCV_PGSIZE;
  while(free_mem >= create_args.free_mem)
  {
    struct page_t *page = (struct page_t*)free_mem;
    page->paddr = free_mem;
    page->next = enclave->free_pages;
    enclave->free_pages = page;
    enclave->free_pages_num += 1;
    free_mem -= RISCV_PGSIZE;
  }

  //check kbuffer
  if(create_args.kbuffer_size < RISCV_PGSIZE || create_args.kbuffer & (RISCV_PGSIZE-1) || create_args.kbuffer_size & (RISCV_PGSIZE-1))
  {
    ret = ENCLAVE_ERROR;
    printm("check kbuffer fail: ENCLAVE_ERROR");
    return ret;
  }
  mmap((uintptr_t*)(enclave->root_page_table), &(enclave->free_pages), ENCLAVE_DEFAULT_KBUFFER, create_args.kbuffer, create_args.kbuffer_size);
  
  release_enclave_metadata_lock();

  copy_word_to_host((unsigned int*)create_args.eid_ptr, enclave->eid);

  return 0;
}

uintptr_t run_enclave(uintptr_t* regs, unsigned int eid)
{
  struct enclave_t* enclave;
  uintptr_t retval = 0;

  acquire_enclave_metadata_lock();

  enclave = get_enclave(eid);
  if(!enclave)
  {
    printm("M mode: run_enclave: wrong enclave id\r\n");
    return -1UL;
  }

  if(enclave->state != FRESH)
  {
    printm("M mode: run_enclave: enclave is not initialized or already used\r\n");
    retval = -1UL;
    goto run_enclave_out;
  }
  if(enclave->host_ptbr != read_csr(satp))
  {
    printm("M mode: run_enclave: enclave doesn't belong to current host process\r\n");
    retval = -1UL;
    goto run_enclave_out;
  }
  if(swap_from_host_to_enclave(regs, enclave) < 0)
  {
    printm("M mode: run_enclave: enclave can not be run\r\n");
    retval = -1UL;
    goto run_enclave_out;
  }

  //set return address to enclave
  write_csr(mepc, (uintptr_t)(enclave->entry_point));

  //TODO: enable timer interrupt
  set_csr(mie, MIP_MTIP);

  //set default stack
  regs[2] = ENCLAVE_DEFAULT_STACK;

  //pass parameters
  regs[11] = (uintptr_t)enclave->entry_point;
  regs[12] = (uintptr_t)enclave->untrusted_ptr;
  regs[13] = (uintptr_t)enclave->untrusted_size;

  enclave->state = RUNNING;

run_enclave_out:
  release_enclave_metadata_lock();
  return retval;
}

uintptr_t stop_enclave(uintptr_t* regs, unsigned int eid)
{
  uintptr_t retval = 0;

  acquire_enclave_metadata_lock();

  struct enclave_t *enclave = get_enclave(eid);
  if(!enclave)
  {
    printm("M mode: stop_enclave: wrong enclave id%d\r\n", eid);
    return -1UL;
  }

  if(enclave->host_ptbr != read_csr(satp))
  {
    printm("M mode: stop_enclave: enclave doesn't belong to current host process\r\n");
    retval = -1UL;
    goto stop_enclave_out;
  }
  if(enclave->state <= FRESH)
  {
    printm("M mode: stop_enclave: enclave%d hasn't begin running at all\r\n", eid);
    retval = -1UL;
    goto stop_enclave_out;
  }
  enclave->state = STOPPED;

stop_enclave_out:
  release_enclave_metadata_lock();
  return retval;
}

uintptr_t resume_from_stop(uintptr_t* regs, unsigned int eid)
{
  uintptr_t retval = 0;

  acquire_enclave_metadata_lock();

  struct enclave_t* enclave = get_enclave(eid);
  if(!enclave)
  {
    printm("M mode: resume_from_stop: wrong enclave id%d\r\n", eid);
    return -1UL;
  }

  if(enclave->host_ptbr != read_csr(satp))
  {
    printm("M mode: resume_from_stop: enclave doesn't belong to current host process\r\n");
    retval = -1UL;
    goto resume_from_stop_out;
  }

  if(enclave->state != STOPPED)
  {
    printm("M mode: resume_from_stop: enclave doesn't belong to current host process\r\n");
    retval = -1UL;
    goto resume_from_stop_out;
  }

  enclave->state = RUNNABLE;

resume_from_stop_out:
  release_enclave_metadata_lock();
  return retval;
}

uintptr_t resume_enclave(uintptr_t* regs, unsigned int eid)
{
  uintptr_t retval = 0;

  acquire_enclave_metadata_lock();

  struct enclave_t* enclave = get_enclave(eid);
  if(!enclave)
  {
    printm("M mode: resume_enclave: wrong enclave id%d\r\n", eid);
    return -1UL;
  }

  if(enclave->host_ptbr != read_csr(satp))
  {
    printm("M mode: resume_enclave: enclave doesn't belong to current host process\r\n");
    retval = -1UL;
    goto resume_enclave_out;
  }

  //TODO: check whether enclave is stopped or destroyed
  if(enclave->state == STOPPED)
  {
    retval = ENCLAVE_TIMER_IRQ;
    goto resume_enclave_out;
  }
  if(enclave->state == DESTROYED)
  {
    //TODO
  }

  if(enclave->state != RUNNABLE)
  {
    printm("M mode: resume_enclave: enclave%d is not runnable\r\n", eid);
    retval = -1UL;
    goto resume_enclave_out;
  }

  if(swap_from_host_to_enclave(regs, enclave) < 0)
  {
    printm("M mode: resume_enclave: enclave can not be run\r\n");
    retval = -1UL;
    goto resume_enclave_out;
  }

  enclave->state = RUNNING;

  //regs[10] will be set to retval when mcall_trap return, so we have to
  //set retval to be regs[10] here to succuessfully restore context
  //TODO: retval should be set to indicate success or fail when resume from ocall
  retval = regs[10];

resume_enclave_out:
  release_enclave_metadata_lock();
  return retval;
}

//host use this fucntion to re-enter enclave world
uintptr_t resume_from_ocall(uintptr_t* regs, unsigned int eid)
{
  uintptr_t retval = 0;
  uintptr_t ocall_func_id = regs[12];
  struct enclave_t* enclave = NULL;

  acquire_enclave_metadata_lock();

  enclave = __get_real_enclave(eid);
  if(!enclave || enclave->state != OCALLING || enclave->host_ptbr != read_csr(satp))
  {
    retval = -1UL;
    goto out;
  }

  switch(ocall_func_id)
  {
    case OCALL_MMAP:
      //TODO
      retval = 0;
      break;
    case OCALL_UNMAP:
      //TODO
      retval = 0;
      break;
    case OCALL_SYS_WRITE:
      retval = enclave->thread_context.prev_state.a0;
      break;
    default:
      retval = 0;
      break;
  }

  if(swap_from_host_to_enclave(regs, enclave) < 0)
  {
    retval = -1UL;
    goto out;
  }
  enclave->state = RUNNING;

out:
  release_enclave_metadata_lock();
  return retval;
}

uintptr_t exit_enclave(uintptr_t* regs, unsigned long retval)
{
  printm("M mode: exit_enclave: retval of enclave is %lx\r\n", retval);

  struct enclave_t *enclave;
  unsigned long paddr, size;
  int i, eid;

  if(check_in_enclave_world() < 0)
  {
    printm("M mode: exit_enclave: cpu is not in enclave world now\r\n");
    return -1;
  }

  acquire_enclave_metadata_lock();

  eid = get_enclave_id();
  enclave = get_enclave(eid);
  if(!enclave)
  {
    printm("M mode: exit_enclave: didn't find eid%d 's corresponding enclave\r\n", eid);
    return -1UL;
  }

  if(check_enclave_authentication(enclave) < 0)
  {
    printm("M mode: exit_enclave: current enclave's eid is not %d\r\n", eid);
    spinlock_unlock(&enclave_metadata_lock);
    return -1UL;
  }

  swap_from_enclave_to_host(regs, enclave);

  //free enclave's memory
  //TODO: support multiple memory region
  memset((void*)(enclave->paddr), 0, enclave->size);
  mm_free((void*)(enclave->paddr), enclave->size);
  
  //free enclave struct
  free_enclave(eid);

  release_enclave_metadata_lock();
  
  return 0;
}

uintptr_t enclave_mmap(uintptr_t* regs, uintptr_t vaddr, uintptr_t size)
{
  uintptr_t ret = 0;
  //TODO
  return ret;
}

uintptr_t enclave_unmap(uintptr_t* regs, uintptr_t vaddr, uintptr_t size)
{
  uintptr_t ret = 0;
  //TODO
  return ret;
}

uintptr_t enclave_sys_write(uintptr_t* regs)
{
  uintptr_t ret = 0;
  int eid = get_curr_enclave_id();
  struct enclave_t* enclave = NULL;
  if(check_in_enclave_world() < 0)
    return -1;

  acquire_enclave_metadata_lock();

  enclave = get_enclave(eid);
  if(!enclave || check_enclave_authentication(enclave)!=0 || enclave->state != RUNNING)
  {
    ret = -1UL;
    goto out;
  }

  copy_dword_to_host((uintptr_t*)enclave->ocall_func_id, OCALL_SYS_WRITE);

  swap_from_enclave_to_host(regs, enclave);
  enclave->state = OCALLING;
  ret = ENCLAVE_OCALL;
out:
  release_enclave_metadata_lock();
  return ret;
}

uintptr_t do_timer_irq(uintptr_t *regs, uintptr_t mcause, uintptr_t mepc)
{
  uintptr_t retval = 0;

  acquire_enclave_metadata_lock();

  unsigned int eid = get_enclave_id();
  struct enclave_t *enclave = get_enclave(eid);
  if(!enclave)
  {
    printm("M mode: something is wrong with enclave%d\r\n", eid);
    return -1UL;
  }

  //TODO: check whether this enclave is destroyed
  if(enclave->state == DESTROYED)
  {
    //TODO
  }

  if(enclave->state != RUNNING && enclave->state != STOPPED)
  {
    printm("M mode: smething is wrong with enclave%d\r\n", eid);
    retval = -1;
    goto timer_irq_out;
  }
  swap_from_enclave_to_host(regs, enclave);
  enclave->state = RUNNABLE;
  regs[10] = ENCLAVE_TIMER_IRQ;

timer_irq_out:
  release_enclave_metadata_lock();
  return retval;
}

uintptr_t call_enclave(uintptr_t* regs, unsigned int callee_eid, uintptr_t arg)
{
  uintptr_t ret = 0;
  printm("M mode: call encalve success\r\n");
  return ret;
}

uintptr_t enclave_return(uintptr_t* regs, uintptr_t arg)
{
    uintptr_t ret = 0;
    printm("M mode: return encalve success\r\n");
    return ret;
}
