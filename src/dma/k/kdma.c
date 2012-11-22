#include <linux/init.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/rmap.h>
#include <asm/mman.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include "kdma_ioctl.h"


/* KDMA device info */

typedef struct kdma_dev
{
  /* char device is contained for o(1) lookup */
  struct cdev cdev;
  struct pci_dev* pci_dev;

  /* mapped base addresses and sizes */
  uintptr_t vadr[6];
  size_t len[6];

  /* interrupt backed context */
  struct irq_context
  {
    volatile uint32_t sta;
  } irq_context;

  /* interrupt waiting queue */
  wait_queue_head_t wq;

} kdma_dev_t;


/* only for debugging purpose, do not define in release mode */
#define KDMA_DEBUG_ASSERT(__x)

static inline uint32_t read_uint32(uintptr_t addr)
{
  return (uint32_t)readl((void*)addr);
}

static inline void write_uint32(uint32_t data, uintptr_t addr)
{
  writel(data, (void*)addr);
}

static void read_bar_safe
(
 kdma_dev_t* dev,
 unsigned int bar, uintptr_t off,
 uint32_t* addr,
 unsigned int word32_count
)
{
  uintptr_t p = dev->vadr[bar] + off;
  unsigned int i;
  for (i = 0; i < word32_count; ++i, ++addr, p += sizeof(uint32_t))
    *addr = read_uint32(p);
}

static void write_bar_safe
(
 kdma_dev_t* dev,
 unsigned int bar, uintptr_t off,
 const uint32_t* addr,
 unsigned int word32_count
)
{
  uintptr_t p = dev->vadr[bar] + off;
  unsigned int i;
  for (i = 0; i < word32_count; ++i, ++addr, p += sizeof(uint32_t))
    write_uint32(*addr, p);
}

static int check_bar_access
(kdma_dev_t* dev, unsigned int bar, uintptr_t off, unsigned int word32_count)
{
  const unsigned int nbar = sizeof(dev->vadr) / sizeof(dev->vadr[0]);
  size_t size;
  uintptr_t bar_addr;

  /* invalid bar */
  if (bar >= nbar) return -1;
  if (dev->vadr[bar] == 0) return 0;

  /* size in bytes, checks for overflow */
  if (word32_count > ((uint32_t)-1 / sizeof(uint32_t))) return -1;
  size = word32_count * sizeof(uint32_t);

  /* arithmetic overflows */
  bar_addr = dev->vadr[bar] + off;
  if (bar_addr < dev->vadr[bar]) return -1;
  if ((bar_addr + size) < bar_addr) return -1;

  /* bar range overflow */
  if ((off + size) > dev->len[bar]) return -1;

  /* off alignment */
  if (off & (sizeof(uint32_t) - 1)) return -1;

  return 0;
}

__attribute__((unused))
static inline int read_bar_unsafe
(
 kdma_dev_t* dev,
 unsigned int bar, uintptr_t off,
 uint32_t* addr,
 unsigned int word32_count
)
{
  if (check_bar_access(dev, bar, off, word32_count)) return -1;
  read_bar_safe(dev, bar, off, addr, word32_count);
  return 0;
}

__attribute__((unused))
static inline int write_bar_unsafe
(
 kdma_dev_t* dev,
 unsigned int bar, uintptr_t off,
 const uint32_t* addr,
 unsigned int word32_count
)
{
  if (check_bar_access(dev, bar, off, word32_count)) return -1;
  write_bar_safe(dev, bar, off, addr, word32_count);
  return 0;
}

static inline uint32_t read_sta(kdma_dev_t* dev)
{
#define DMA_REG_BAR 0x01
#define DMA_REG_CTL 0x00
#define DMA_REG_STA 0x04
#define DMA_REG_ADL 0x08
#define DMA_REG_ADH 0x0c

  const uintptr_t addr = dev->vadr[DMA_REG_BAR] + DMA_REG_STA;
  return read_uint32(addr);
}

static inline void write_ctl(kdma_dev_t* dev, uint32_t x)
{
  const uintptr_t addr = dev->vadr[DMA_REG_BAR] + DMA_REG_CTL;
  write_uint32(x, addr);
}

static inline void write_adl(kdma_dev_t* dev, uint32_t x)
{
  const uintptr_t addr = dev->vadr[DMA_REG_BAR] + DMA_REG_ADL;
  write_uint32(x, addr);
}

static inline void write_adh(kdma_dev_t* dev, uint32_t x)
{
  const uintptr_t addr = dev->vadr[DMA_REG_BAR] + DMA_REG_ADH;
  write_uint32(x, addr);
}

/* on interrupt, capture status register and signal the waitqueue */

static irqreturn_t irq_handler
(
 int irq, void* dev_id
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19))
 , struct pt_regs* do_not_use /* may be NULL */
#endif
)
{
  kdma_dev_t* const dev = dev_id;
  uint32_t sta;

  printk("interrupt == %d, devid == 0x%lx\n", irq, (unsigned long)dev_id);

  sta = read_sta(dev);

  /* transfer is done */
  if (sta & (1 << 31))
  {
    /* back prior wakuping */
    dev->irq_context.sta = sta;
    __sync_synchronize();
    wake_up_interruptible(&dev->wq);
    return IRQ_RETVAL(1);
  }

  return IRQ_NONE;
}


void kdma_dev_init(kdma_dev_t* dev)
{
  const unsigned int nbar = sizeof(dev->vadr) / sizeof(dev->vadr[0]);
  unsigned int i;

  for (i = 0; i < nbar; ++i)
  {
    dev->vadr[i] = 0;
    dev->len[i] = 0;
  }

  init_waitqueue_head(&dev->wq);

  dev->irq_context.sta = 0;
}


/* linux module interface, not compiled if not standing alone */

MODULE_DESCRIPTION("KDMA driver");
MODULE_AUTHOR("KDMA team");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
MODULE_LICENSE("Dual BSD/GPL");
#endif


#define KDMA_NAME KBUILD_MODNAME
#define KDMA_TAG "[ " KDMA_NAME " ] "
#define KDMA_ENTER() printk(KDMA_TAG "%s\n", __FUNCTION__)
#define KDMA_ASSERT(__x)


/* KDMA per driver data (global, singleton)
 */

typedef struct atomic32
{
  volatile uint32_t x;
} atomic32_t;

static inline void atomic32_init(atomic32_t* x)
{
  /* not a synced access */
  x->x = 0;
}

static inline uint32_t atomic32_or(atomic32_t* x, uint32_t val)
{
  /* return the previous value */
  return __sync_fetch_and_or(&x->x, val);
}

static inline uint32_t atomic32_and(atomic32_t* x, uint32_t val)
{
  /* return the previous value */
  return __sync_fetch_and_and(&x->x, val);
}

static inline uint32_t atomic32_read(atomic32_t* x)
{
  /* not a synced access */
  return x->x;
}

typedef struct kdma_driver
{
  /* device storage. must be < 32. */
#define KDMA_DEV_COUNT 2
  kdma_dev_t devs[KDMA_DEV_COUNT];

  /* allocated devices bitmap. 0 means free. */
  atomic32_t bits;

  /* device major number */
  int major;

} kdma_driver_t;

static kdma_driver_t kdma_driver;

static inline void kdma_driver_init(kdma_driver_t* d)
{
  atomic32_init(&d->bits);
}

static inline uint32_t dev_to_bit32(kdma_dev_t* dev)
{
  /* return the device bit position in kdma_driver.bits */
  return (uint32_t)
    ((uintptr_t)dev - (uintptr_t)kdma_driver.devs) / sizeof(kdma_dev_t);
}

static inline kdma_dev_t* bit32_to_dev(uint32_t x)
{
  return &kdma_driver.devs[x];
}

static kdma_dev_t* kdma_dev_alloc(void)
{
  /* work on a captured value, try to set and commit any free bit */

  const uint32_t ndev = KDMA_DEV_COUNT;

  uint32_t mask;
  uint32_t x;
  uint32_t i;

  x = atomic32_read(&kdma_driver.bits);

  for (i = 0; i < ndev; ++i, x >>= 1)
  {
    /* no more bit avail */
    mask = (1 << (ndev - i)) - 1;
    if ((x & mask) == mask) return NULL;

    /* current bit avail */
    if ((x & 1) == 0)
    {
      /* set and check for lost race */
      mask = 1 << i;
      if ((atomic32_or(&kdma_driver.bits, mask) & mask) == 0)
      {
	/* perform any software zeroing needed */
	kdma_dev_t* const dev = bit32_to_dev(i);
	kdma_dev_init(dev);
	return dev;
      }

      /* otherwise someone took it in between */
    }
  }

  return NULL;
}

static inline void kdma_dev_free(kdma_dev_t* dev)
{
  const uint32_t mask = ~(1 << dev_to_bit32(dev));
  atomic32_and(&kdma_driver.bits, mask);
}

static inline unsigned int kdma_is_dev_allocated(kdma_dev_t* dev)
{
  /* non atomic access */
  return atomic32_read(&kdma_driver.bits) & (1 << dev_to_bit32(dev));
}

static inline unsigned int kdma_is_dev_valid(kdma_dev_t* dev)
{
  /* check if the device address is valid */
  if ((uintptr_t)dev < (uintptr_t)&kdma_driver.devs[0]) return 0;
  if ((uintptr_t)dev >= (uintptr_t)&kdma_driver.devs[KDMA_DEV_COUNT]) return 0;
  return 1;
}

static void kdma_dev_unmap_bars
(kdma_dev_t* kdma_dev, struct pci_dev* pci_dev __attribute__((unused)))
{
  const unsigned int nbar = sizeof(kdma_dev->vadr) / sizeof(kdma_dev->vadr[0]);

  unsigned int i;

  for (i = 0; i < nbar; ++i)
  {
    if (kdma_dev->vadr[i])
    {
      pci_release_region(pci_dev, (int)i);
      iounmap((void*)kdma_dev->vadr[i]);
      kdma_dev->vadr[i] = 0;
      kdma_dev->len[i] = 0;
    }
  }
}

static int kdma_dev_map_bars(kdma_dev_t* kdma_dev, struct pci_dev* pci_dev)
{
  const unsigned int nbar = sizeof(kdma_dev->vadr) / sizeof(kdma_dev->vadr[0]);
  int err = 0;
  unsigned int i;

  for (i = 0; i < nbar; ++i)
  {
    const unsigned long flags = pci_resource_flags(pci_dev, i);
    const uintptr_t addr = (uintptr_t)pci_resource_start(pci_dev, i);
    const size_t len = (size_t)pci_resource_len(pci_dev, i);

    if ((err == 0) && (flags & IORESOURCE_MEM) && (addr && len))
    {
      if ((err = pci_request_region(pci_dev, (int)i, KDMA_NAME)))
      {
	/* continue with error set */
	kdma_dev->len[i] = 0;
	kdma_dev->vadr[i] = 0;
      }
      else
      {
	kdma_dev->vadr[i] = (uintptr_t)ioremap_nocache(addr, len);
	kdma_dev->len[i] = len;
	if (kdma_dev->vadr[i] == 0)
	{
	  /* continue but zero remaining entries for unmapping */
	  kdma_dev->len[i] = 0;
	  err = -ENOMEM;
	}
      }
    }
    else
    {
      kdma_dev->vadr[i] = 0;
      kdma_dev->len[i] = 0;
    }
  }

  if (err) kdma_dev_unmap_bars(kdma_dev, pci_dev);
  return err;
}


/* PCI interface
 */

DEFINE_PCI_DEVICE_TABLE(kdma_pci_ids) =
{
  /* kdma */
  { PCI_DEVICE( 0x2a2a, 0x2b2b ) },
  { 0, }
};

/* used to export symbol for use by userland */
MODULE_DEVICE_TABLE(pci, kdma_pci_ids);

/* PCI error recovery */

static pci_ers_result_t kdma_pci_error_detected
(struct pci_dev* pci_dev, pci_channel_state_t state)
{
  printk(KDMA_TAG "kdma_pci_error_detected(0x%08x)\n", state);

  if (state == pci_channel_io_perm_failure)
    return PCI_ERS_RESULT_DISCONNECT;

  /* disable an request a slot reset */

  pci_disable_device(pci_dev);

  return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t kdma_pci_mmio_enabled(struct pci_dev* pci_dev)
{
  printk(KDMA_TAG "pci_mmio_enabled\n");
  return PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t kdma_pci_link_reset(struct pci_dev* pci_dev)
{
  printk(KDMA_TAG "kdma_pci_link_reset\n");
  return PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t kdma_pci_slot_reset(struct pci_dev* pci_dev)
{
  printk(KDMA_TAG "kdma_pci_slot_reset\n");
  return PCI_ERS_RESULT_RECOVERED;
}

static void kdma_pci_resume(struct pci_dev* pci_dev)
{
  printk(KDMA_TAG "kdma_pci_resume\n");
}

static struct pci_error_handlers kdma_err_handler =
{
  .error_detected = kdma_pci_error_detected,
  .mmio_enabled = kdma_pci_mmio_enabled,
  .slot_reset = kdma_pci_slot_reset,
  .link_reset = kdma_pci_link_reset,
  .resume = kdma_pci_resume
};

static int kdma_cdev_init(kdma_dev_t* dev);
static inline void kdma_cdev_fini(kdma_dev_t* dev);

/* linux-source-3.2/Documentation/PCI/pci.txt recommends __devinit */

static int __devinit kdma_pci_probe
(struct pci_dev* pci_dev, const struct pci_device_id* dev_id)
{
  kdma_dev_t* kdma_dev;
  int err;

  /* allocate device structure
   */

  printk("%s\n", __FUNCTION__);

  if ((kdma_dev = kdma_dev_alloc()) == NULL)
  {
    err = -ENOMEM;
    goto on_error_0;
  }

  kdma_dev->pci_dev = pci_dev;

  pci_set_drvdata(pci_dev, kdma_dev);

#if 1 /* FIXME: do not enable twice */
  if (pci_is_enabled(pci_dev) == 0)
#endif /* FIXME */
    if ((err = pci_enable_device(pci_dev)))
      goto on_error_1;

  /* map the device bars
   */

  if ((err = kdma_dev_map_bars(kdma_dev, pci_dev)))
  {
    printk("[!] kdma_dev_map() == %d\n", err);
    goto on_error_2;
  }

  /* install interrupt handler
   */

  if ((err = pci_enable_msi(pci_dev)))
  {
    printk("[!] pci_enable_msi() == %d\n", err);
    goto on_error_2;
  }

  /* printk("kdma: pci_enable_msi() == %d\n", pci_dev->irq); */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18))
# define IRQ_SHARED_FLAG IRQF_SHARED
#else
# define IRQ_SHARED_FLAG SA_SHIRQ
#endif
  err = request_irq
  (
   pci_dev->irq,
   irq_handler,
   IRQ_SHARED_FLAG,
   KDMA_NAME,
   (void*)kdma_dev
  );

  if (err)
  {
    printk("[!] request_irq() == %d\n", err);
    goto on_error_3;
  }

  /* initialize char device
   */

  if ((err = kdma_cdev_init(kdma_dev)))
  {
    printk("[!] kdma_cdev_init() == %d\n", err);
    goto on_error_4;
  }

  /* success
   */

  return 0;

 on_error_4:
  free_irq(pci_dev->irq, kdma_dev);
 on_error_3:
  pci_disable_msi(pci_dev);
 on_error_2:
  kdma_dev_unmap_bars(kdma_dev, pci_dev);
  pci_disable_device(pci_dev);
 on_error_1:
  kdma_dev_free(kdma_dev);
  pci_set_drvdata(pci_dev, NULL);
 on_error_0:
  return err;
}

static void __devexit kdma_pci_remove(struct pci_dev* pci_dev)
{
  kdma_dev_t* const kdma_dev = pci_get_drvdata(pci_dev);

  KDMA_ASSERT(kdma_dev);

  printk(KDMA_TAG "kdma_pci_remove(%u)\n", dev_to_bit32(kdma_dev));

  kdma_cdev_fini(kdma_dev);

  free_irq(pci_dev->irq, kdma_dev);
  pci_disable_msi(pci_dev);

  kdma_dev_unmap_bars(kdma_dev, pci_dev);

  pci_disable_device(pci_dev);

  pci_set_drvdata(pci_dev, NULL);

  kdma_dev_free(kdma_dev);
}

static struct pci_driver kdma_pci_driver =
{
  .name = KDMA_NAME,
  .id_table = kdma_pci_ids,
  .probe = kdma_pci_probe,
  .remove = __devexit_p(kdma_pci_remove),
  .err_handler = &kdma_err_handler
};

static int kdma_pci_init(void)
{
  int err;

  if ((err = pci_register_driver(&kdma_pci_driver)))
  {
    printk(KDMA_TAG "pci_register_driver() == %d\n", err);
    return err;
  }

  return 0;
}

static void kdma_pci_fini(void)
{
  pci_unregister_driver(&kdma_pci_driver);
}


/* contiguous physical memory allocators (pmem_xxx api)
   http://lwn.net/Articles/396702
   http://lwn.net/Articles/447405
 */

/* allocator type */

#define CONFIG_PMEM_DYNAMIC 1
#define CONFIG_PMEM_BOOT 0
#define CONFIG_PMEM_EARLY 0


#if CONFIG_PMEM_BOOT || CONFIG_PMEM_EARLY

/* only memory reservation differ in the 2 strategies */

#if CONFIG_PMEM_BOOT

/* boot time reserved memory (ie. memmap=128M$256M command line option) */
static const size_t pmem_reserved_size = 128 * 1024 * 1024;
static const uintptr_t pmem_reserved_paddr = 256 * 1024 * 1024;

static int pmem_init(size_t max_size)
{
  if (max_size > pmem_reserved_size) return -ENOMEM;
  return 0;
}

static void pmem_fini(void)
{
}

#else /* CONFIG_PMEM_EARLY */

/* early big allocation using LINUX physical allocator */

static size_t pmem_reserved_size;
static uintptr_t pmem_reserved_paddr;

struct pmem_early_list
{
  unsigned long order;
  unsigned long vaddr;
  struct pmem_list* next;
};

static struct pmem_early_list* pmem_early_head = NULL;

static void pmem_fini(void)
{
  while (pmem_early_head)
  {
    struct pmem_list* const tmp = pmem_early_head;
    pmem_early_head = pmem_early_head->next;
    free_pages(tmp->vaddr, tmp->order);
    kfree(tmp);
  }
}

static int pmem_init(size_t total_size)
{
  const unsigned long total_order = get_order(total_size);
  const unsigned long max_order = MAX_ORDER - 1;
  const unsigned long max_size = max_order * PAGE_SIZE;
  unsigned long prev_vaddr = 0;
  unsigned long prev_paddr = 0;
  unsigned long norder;
  unsigned long i;

  norder = total_order / max_order;
  if (total_order % max_order) ++norder;

  /* TODO: worth case currently allocates one more max_order pages */

  for (i = 0; i < norder; ++i)
  {
    struct pmem_list* new_head;
    unsigned long paddr;
    unsigned long vaddr;

    vaddr = __get_free_pages(GFP_ATOMIC, max_order);
    if (vaddr == 0)
    {
      printk("[ kdma ] __get_free_pages() FAILURE\n");
      goto on_error;
    }

    paddr = (unsigned long)virt_to_phys((void*)vaddr);

    /* check pages and mapping are contiguous */
    if (i != 0)
    {
      if ((prev_paddr + max_size) != paddr)
      {
	printk("[ kdma ] pmem_init: %lu not physically contiguous\n", i);
	printk("[ kdma ] pmem_init: %lx != %lx\n", prev_paddr + max_size, paddr);
	printk("[ kdma ] pmem_init: %lx\n", paddr + max_size);
	goto on_error;
      }

      if ((prev_vaddr + max_size) != vaddr)
      {
	printk("[ kdma ] pmem_init: %lu not virtually contiguous\n", i);
	goto on_error;
      }
    }
    else
    {
      pmem_reserved_paddr = paddr;
      pmem_reserved_vaddr = (void*)vaddr;
      pmem_reserved_size = total_size;
    }

    new_head = kmalloc(sizeof(struct pmem_list), GFP_KERNEL);
    if (new_head == NULL) goto on_error;

    new_head->order = max_order;
    new_head->vaddr = vaddr;
    new_head->next = pmem_head;
    pmem_early_head = new_head;

    prev_paddr = paddr;
    prev_vaddr = vaddr;
  }

  return 0;

 on_error:
  pmem_fini();
  return -ENOMEM;
}

#endif /* CONFIG_PMEM_EARLY */

static int pmem_alloc(uintptr_t* paddr, size_t size, int nid)
{
  /* TODO: bitmap allocator */
  if (size > pmem_reserved_size) return -ENOMEM;
  *paddr = pmem_reserved_paddr;
  return 0;
}

static void pmem_free(uintptr_t paddr, size_t size)
{
  /* TODO: bitmap allocator */
}

#elif CONFIG_PMEM_DYNAMIC

/* LINUX on demand physical memory allocator */

/* internally managed allocation list. it is only used by the
   kernel to release unreleased allocations of a process. esp.
   it is not used for meta storage.
 */

struct pmem_node
{
  struct pmem_node* next;
  uintptr_t paddr;
  size_t size;
};

static struct pmem_node* pmem_head = NULL;

static int del_pmem_node(uintptr_t paddr)
{
  /* delete a node given paddr */

  struct pmem_node* pos = pmem_head;
  struct pmem_node* prev = NULL;

  while (pos)
  {
    if (pos->paddr == paddr)
    {
      if (prev) prev->next = pos->next;
      else pmem_head = pos->next;
      kfree(pos);
      return 0;
    }

    prev = pos;
    pos = pos->next;
  }

  /* not found */
  return -1;
}

static int add_pmem_node(uintptr_t paddr, size_t size)
{
  /* add a node for paddr, size */

  struct pmem_node* const pos = kmalloc(sizeof(struct pmem_node), GFP_KERNEL);
  if (pos == NULL) return -1;

  pos->paddr = paddr;
  pos->size = size;
  pos->next = pmem_head;
  pmem_head = pos;

  return 0;
}

static void pmem_free(uintptr_t paddr, size_t size);
static void free_pmem_nodes(void)
{
  /* free all the nodes in pmem list */

  while (pmem_head != NULL)
    pmem_free(pmem_head->paddr, pmem_head->size);
}

static int pmem_init(unsigned int size)
{
  return 0;
}

static void pmem_fini(void)
{
}

static int pmem_alloc(uintptr_t* paddr, size_t size, int nid)
{
  const unsigned long order = get_order(size); 

  unsigned long vaddr;

  if (nid < 0) /* node does not matter */
  {
    vaddr = __get_free_pages(GFP_ATOMIC, order);
    if (vaddr == 0) return -ENOMEM;
  }
  else /* allocate on node nid */
  {
    struct page* const page =
      alloc_pages_exact_node(nid, GFP_ATOMIC, order);
    if (page == NULL) return -ENOMEM;
    vaddr = (unsigned long)page_address(page);
  }

  *paddr = (uintptr_t)virt_to_phys((void*)(uintptr_t)vaddr);
  KDMA_ASSERT(*paddr);

  if (add_pmem_node(*paddr, size))
  {
    free_pages(vaddr, order);
    *paddr = 0;
    return -ENOMEM;
  }

  return 0;
}

static void pmem_free(uintptr_t paddr, size_t size)
{
  const unsigned long order = get_order(size);
  const unsigned long vaddr = (unsigned long)(uintptr_t)phys_to_virt(paddr);

  /* equivalent to __free_pages(virt_to_page(paddr, order) */
  free_pages(vaddr, order);

  del_pmem_node(paddr);
}

#else /* invalid allocator */
#error "no physical memory allocation strategy defined"
#endif


/* LINUX file interface
 */

static int kdma_file_open(struct inode* inode, struct file* file)
{
  /* TODO: open must be exclusive, because only one instance per device */

  kdma_dev_t* const dev = container_of(inode->i_cdev, kdma_dev_t, cdev);

  if (!(kdma_is_dev_valid(dev) && kdma_is_dev_allocated(dev)))
  {
    printk("kdma_file_open, invalid device\n");
    return -EFAULT;
  }

  file->private_data = dev;

  return 0;
}

static int kdma_file_close(struct inode* inode, struct file* file)
{
  /* kdma_dev_t* const dev = file->private_data; */
  /* KDMA_ASSERT(dev); */

#if CONFIG_PMEM_DYNAMIC
  free_pmem_nodes();
#endif

  file->private_data = NULL;

  return 0;
}

#ifdef HAVE_UNLOCKED_IOCTL
static long kdma_file_ioctl
(struct file* file, unsigned int cmd, unsigned long uaddr)
{
  __attribute__((unused)) struct inode* const inode = file->f_dentry->d_inode;
#else
static int kdma_ioctl
(struct inode* inode, struct file* file, unsigned int cmd, unsigned long uaddr)
{
#endif

  kdma_dev_t* const dev = file->private_data;
  int err;

  KDMA_DEBUG_ASSERT(dev);

  switch (cmd)
  {
  case KDMA_IOCTL_READ_BAR:
    {
      /* TODO: use local buffer for small sizes */

      kdma_ioctl_readwrite_t arg;

      if (copy_from_user(&arg, (void*)(uintptr_t)uaddr, sizeof(arg)))
      {
	err = -EFAULT;
	break ;
      }

      /* check accessed range */
      if (check_bar_access(dev, arg.bar, arg.off, arg.word32_count) == -1)
      {
	err = -EFAULT;
	break ;
      }

#if 0 /* TODO: direct io, secure user mapping for direct io access */
      {
      /* reference: ldd3_15_performing_direct_io */
      /* page align the user mapping */
      unsigned long aligned_addr;
      unsigned long aligned_offset = (unsigned long)arg.addr - aligned_offset;
      int page_count;
      struct page* pages;
      struct vm_area_struct* vmas;

      /* compute aligned numbers */

      down_read(&current->mm->mmap_sem);
      err = get_user_pages
	(current, current->mm, aligned_addr, page_count, 1, 0, &pages, &vmas);
      if (err != page_count) err = -EFAULT;
      up_read(&current->mm->mmap_sem);

      read_bar_safe
	(dev, arg.bar, arg.off, (uint32_t*)arg.addr, arg.word32_count);
      }
#else /* buffered io */
      {
      const size_t size = arg.word32_count * sizeof(uint32_t);
      void* large_buf = NULL;
      uint32_t tiny_buf;
      void* buf = &tiny_buf;

      /* use kmalloced large_buf */
      if (size > sizeof(tiny_buf))
      {
	if ((large_buf = kmalloc(size, GFP_KERNEL)) == NULL)
	{
	  err = -ENOMEM;
	  break ;
	}

	buf = large_buf;
      }

      read_bar_safe(dev, arg.bar, arg.off, buf, arg.word32_count);

      err = copy_to_user((void*)arg.addr, buf, size);

      if (large_buf != NULL) kfree(large_buf);

      if (err)
      {
	err = -EFAULT;
	break ;
      }

      }
#endif /* direct io */

      /* success */
      err = 0;
      break ;
   }

  case KDMA_IOCTL_WRITE_BAR:
    {
      /* TODO: use local buffer for small sizes */

      kdma_ioctl_readwrite_t arg;

      if (copy_from_user(&arg, (void*)(uintptr_t)uaddr, sizeof(arg)))
      {
	err = -EFAULT;
	break ;
      }

      /* check accessed range */
      if (check_bar_access(dev, arg.bar, arg.off, arg.word32_count) == -1)
      {
	err = -EFAULT;
	break ;
      }

#if 0 /* TODO: direct io, secure user mapping for direct io access */
      {
      /* reference: ldd3_15_performing_direct_io */
      /* page align the user mapping */
      unsigned long aligned_addr;
      unsigned long aligned_offset = (unsigned long)arg.addr - aligned_offset;
      int page_count;
      struct page* pages;
      struct vm_area_struct* vmas;

      /* compute aligned numbers */

      down_read(&current->mm->mmap_sem);
      err = get_user_pages
	(current, current->mm, aligned_addr, page_count, 1, 0, &pages, &vmas);
      if (err != page_count) err = -EFAULT;
      up_read(&current->mm->mmap_sem);

      write_bar_safe
	(dev, arg.bar, arg.off, (uint32_t*)arg.addr, arg.word32_count);

      /* success */
      err = 0;
      }
#else /* buffered io */
      {
      const size_t size = arg.word32_count * sizeof(uint32_t);
      void* large_buf = NULL;
      uint32_t tiny_buf;
      void* buf = &tiny_buf;

      if (size > sizeof(tiny_buf))
      {
	if ((large_buf = kmalloc(size, GFP_KERNEL)) == NULL)
	{
	  err = -ENOMEM;
	  break ;
	}

	buf = large_buf;
      }

      if (copy_from_user(buf, (void*)arg.addr, size) == 0)
      {
	write_bar_safe(dev, arg.bar, arg.off, buf, arg.word32_count);
	err = 0;
      }
      else
      {
	err = -EFAULT;
      }

      if (large_buf != NULL) kfree(large_buf);
      }
#endif /* direct io */

      break ;
    }

  case KDMA_IOCTL_GET_IRQCONTEXT:
    {
      /* get and clear the last captured irq context */

      kdma_ioctl_irqcontext_t arg;

      arg.sta = dev->irq_context.sta;
      __sync_synchronize();
      dev->irq_context.sta = 0;

      if (copy_to_user((void*)(uintptr_t)uaddr, &arg, sizeof(arg)))
      {
	err = -EFAULT;
	break ;
      }

      /* success */
      err = 0;
      break ;
    }

  case KDMA_IOCTL_PMEM_ALLOC:
    {
      kdma_ioctl_pmem_t arg;
      uintptr_t paddr;
      size_t size;

      if (copy_from_user(&arg, (void*)(uintptr_t)uaddr, sizeof(arg)))
      {
	err = -EFAULT;
	break ;
      }

      if (arg.word32_count > ((uint32_t)-1 / sizeof(uint32_t)))
      {
	err = -EINVAL;
	break ;
      }

      /* TODO: check get_order does not underflow */
      size = arg.word32_count * sizeof(uint32_t);

      if ((err = pmem_alloc(&paddr, size, arg.nid))) break ;

      arg.paddr = paddr;
      if (err || copy_to_user((void*)(uintptr_t)uaddr, &arg, sizeof(arg)))
      {
	pmem_free(paddr, size);
	err = -EFAULT;
	break ;
      }

      err = 0;
      break ;
    }

  case KDMA_IOCTL_PMEM_FREE:
    {
      kdma_ioctl_pmem_t arg;
      size_t size;

      if (copy_from_user(&arg, (void*)(uintptr_t)uaddr, sizeof(arg)))
      {
	err = -EFAULT;
	break ;
      }

      /* assume size is valid */
      size = arg.word32_count * sizeof(uint32_t);
      pmem_free(arg.paddr, size);
      err = 0;

      break ;
    }

  case KDMA_IOCTL_PMEM_SYNC:
    {
      kdma_ioctl_pmem_t arg;
      size_t size;

      if (copy_from_user(&arg, (void*)(uintptr_t)uaddr, sizeof(arg)))
      {
	err = -EFAULT;
	break ;
      }

      /* assume size is valid */
      size = arg.word32_count * sizeof(uint32_t);

      flush_write_buffers();
      pci_dma_sync_single_for_device
	(dev->pci_dev, arg.paddr, size, PCI_DMA_BIDIRECTIONAL);

      err = 0;

      break ;
    }

  default:
    {
      err = -ENOSYS;
      break ;
    }
  }

  return err;
}

static unsigned int kdma_file_poll
(struct file* file, struct poll_table_struct* pts)
{
  kdma_dev_t* const dev = file->private_data;
  unsigned int mask = 0;

  poll_wait(file, &dev->wq, pts);

  /* ready condition is interrupt not yet seen */
  /* FIXME: race here (not fatal) */
  if (dev->irq_context.sta & (1 << 31))
  {
    mask |= POLLIN | POLLRDNORM;
  }

  return mask;
}

static int kdma_file_mmap
(struct file* file, struct vm_area_struct *vma)
{
  const size_t size = vma->vm_end - vma->vm_start;

  int err;

  /* the call marks the range VM_IO and VM_RESERVED */
  err = remap_pfn_range
  (
   vma, vma->vm_start,
   vma->vm_pgoff, /* already in pages */
   size,
   vma->vm_page_prot
  );

  if (err)
  {
    printk("remap_pfn_range() == %d\n", err);
    return err;
  }

  return 0;
}

static struct file_operations kdma_fops =
{
 .owner = THIS_MODULE,
#ifdef HAVE_UNLOCKED_IOCTL
 .unlocked_ioctl = kdma_file_ioctl,
#else
 .ioctl = kdma_file_ioctl,
#endif
 .open = kdma_file_open,
 .poll = kdma_file_poll,
 .release = kdma_file_close,
 .mmap = kdma_file_mmap
};

static int kdma_cdev_init(kdma_dev_t* dev)
{
  /* register the chardev and associated file operations */

  const int minor = (int)dev_to_bit32(dev);
  const dev_t devno = MKDEV(kdma_driver.major, minor);

  cdev_init(&dev->cdev, &kdma_fops);
  dev->cdev.owner = THIS_MODULE;

  /* nothing to do on failure */
  return cdev_add(&dev->cdev, devno, 1);
}

static inline void kdma_cdev_fini(kdma_dev_t* dev)
{
  cdev_del(&dev->cdev);
}


/* LINUX module interface
 */

static int  __init kdma_init(void);
static void __exit kdma_exit(void);


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
module_init(kdma_init);
module_exit(kdma_exit);
#endif


static int __init kdma_init(void)
{
  static const unsigned int ndev = KDMA_DEV_COUNT;
  dev_t first_dev;
  int err;

#if (CONFIG_PMEM_DYNAMIC || CONFIG_PMEM_EARLY)
# define pmem_reserved_size (128 * 1024 * 1024)
#endif
  if ((err = pmem_init(pmem_reserved_size))) return err;

  kdma_driver_init(&kdma_driver);

  /* references */
  /* http://hg.berlios.de/repos/kedr/file/b0f4d9d02d35/sources/examples/sample_target/cfake.c */
  /* http://lists.kernelnewbies.org/pipermail/kernelnewbies/2011-May/001660.html */

  /* dynamic major allocation */
  if ((err = alloc_chrdev_region(&first_dev, 0, ndev, KDMA_NAME)) < 0)
  {
    pmem_fini();
    return err;
  }

  kdma_driver.major = MAJOR(first_dev);

  if ((err = kdma_pci_init()))
  {
    unregister_chrdev_region(first_dev, ndev);
    pmem_fini();
    return err;
  }

  printk(KDMA_TAG "major: %d\n", kdma_driver.major);

  return 0;
}


static void __exit kdma_exit(void)
{
  /* note: kdma_pci_remove is called when unregistering pci driver */
  static const unsigned int ndev = KDMA_DEV_COUNT;
  const dev_t first_dev = MKDEV(kdma_driver.major, 0);
  kdma_pci_fini();
  unregister_chrdev_region(first_dev, ndev);
  pmem_fini();
}
