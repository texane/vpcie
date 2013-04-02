#ifndef KDMA_IOCTL_H_INCLUDED
# define KDMA_IOCTL_H_INCLUDED


typedef struct kdma_ioctl_readwrite
{
  /* host address to readwrite fromto */
  uintptr_t addr;

  /* off the PCI BAR offset in bytes */
  unsigned int bar;
  uintptr_t off;

  /* operation uint32 count */
  unsigned int word32_count;

} kdma_ioctl_readwrite_t;


typedef struct kdma_ioctl_irqcontext
{
  /* captured registers */
  uint32_t sta;
} kdma_ioctl_irqcontext_t;


typedef struct kdma_ioctl_pmem
{
  uintptr_t vaddr;
  uintptr_t paddr;
  unsigned int word32_count;
  int nid;
} kdma_ioctl_pmem_t;


#define KDMA_IOCTL_MAGIC  'e'

#define KDMA_IOCTL_WRITE_BAR _IOW(KDMA_IOCTL_MAGIC, 1, uintptr_t)
#define KDMA_IOCTL_READ_BAR _IOR(KDMA_IOCTL_MAGIC, 2, uintptr_t)
#define KDMA_IOCTL_GET_IRQCONTEXT _IOR(KDMA_IOCTL_MAGIC, 3, uintptr_t)
#define KDMA_IOCTL_PMEM_ALLOC _IO(KDMA_IOCTL_MAGIC, 4)
#define KDMA_IOCTL_PMEM_FREE _IO(KDMA_IOCTL_MAGIC, 5)
#define KDMA_IOCTL_PMEM_MAP _IO(KDMA_IOCTL_MAGIC, 6)
#define KDMA_IOCTL_PMEM_UNMAP _IO(KDMA_IOCTL_MAGIC, 7)
#define KDMA_IOCTL_PMEM_SYNC _IO(KDMA_IOCTL_MAGIC, 8)


#endif /* KDMA_IOCTL_H_INCLUDED */
