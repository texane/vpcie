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
#include <linux/pagemap.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include "sbone_ioctl.h"


/* atomic32 helpers */

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

static inline uint32_t atomic32_inc(atomic32_t* x)
{
  /* return the previous value */
  return __sync_fetch_and_add(&x->x, 1);
}

static inline uint32_t atomic32_read(atomic32_t* x)
{
  /* not a synced access */
  return x->x;
}


/* SBONE device info */

typedef struct sbone_dev
{
  /* char device is contained for o(1) lookup */
  struct cdev cdev;
  struct pci_dev* pci_dev;

  /* mapped base addresses and sizes */
  uintptr_t vadr[6];
  size_t len[6];

  /* interrupt waiting queue */
  wait_queue_head_t wq;

  /* irq count */
  atomic32_t irq_count;

} sbone_dev_t;


/* on interrupt, capture status register and signal the waitqueue */

static irqreturn_t irq_handler
(
 int irq, void* dev_id
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19))
 , struct pt_regs* do_not_use /* may be NULL */
#endif
)
{
  sbone_dev_t* const dev = dev_id;

  printk("interrupt == %d, devid == 0x%lx\n", irq, (unsigned long)dev_id);

  atomic32_inc(&dev->irq_count);
  wake_up_interruptible(&dev->wq);

  return IRQ_RETVAL(1);
}


void sbone_dev_init(sbone_dev_t* dev)
{
  const unsigned int nbar = sizeof(dev->vadr) / sizeof(dev->vadr[0]);
  unsigned int i;

  for (i = 0; i < nbar; ++i)
  {
    dev->vadr[i] = 0;
    dev->len[i] = 0;
  }

  init_waitqueue_head(&dev->wq);

  atomic32_init(&dev->irq_count);
}


/* linux module interface, not compiled if not standing alone */

MODULE_DESCRIPTION("SBONE driver");
MODULE_AUTHOR("SBONE team");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
MODULE_LICENSE("Dual BSD/GPL");
#endif


#define SBONE_NAME KBUILD_MODNAME
#define SBONE_TAG "[ " SBONE_NAME " ] "
#define SBONE_ENTER() printk(SBONE_TAG "%s\n", __FUNCTION__)
#define SBONE_ASSERT(__x)


/* SBONE per driver data (global, singleton)
 */

typedef struct sbone_driver
{
  /* device storage. must be < 32. */
#define SBONE_DEV_COUNT 2
  sbone_dev_t devs[SBONE_DEV_COUNT];

  /* allocated devices bitmap. 0 means free. */
  atomic32_t bits;

  /* device major number */
  int major;

} sbone_driver_t;

static sbone_driver_t sbone_driver;

static inline void sbone_driver_init(sbone_driver_t* d)
{
  atomic32_init(&d->bits);
}

static inline uint32_t dev_to_bit32(sbone_dev_t* dev)
{
  /* return the device bit position in sbone_driver.bits */
  return (uint32_t)
    ((uintptr_t)dev - (uintptr_t)sbone_driver.devs) / sizeof(sbone_dev_t);
}

static inline sbone_dev_t* bit32_to_dev(uint32_t x)
{
  return &sbone_driver.devs[x];
}

static sbone_dev_t* sbone_dev_alloc(void)
{
  /* work on a captured value, try to set and commit any free bit */

  const uint32_t ndev = SBONE_DEV_COUNT;

  uint32_t mask;
  uint32_t x;
  uint32_t i;

  x = atomic32_read(&sbone_driver.bits);

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
      if ((atomic32_or(&sbone_driver.bits, mask) & mask) == 0)
      {
	/* perform any software zeroing needed */
	sbone_dev_t* const dev = bit32_to_dev(i);
	sbone_dev_init(dev);
	return dev;
      }

      /* otherwise someone took it in between */
    }
  }

  return NULL;
}

static inline void sbone_dev_free(sbone_dev_t* dev)
{
  const uint32_t mask = ~(1 << dev_to_bit32(dev));
  atomic32_and(&sbone_driver.bits, mask);
}

static inline unsigned int sbone_is_dev_allocated(sbone_dev_t* dev)
{
  /* non atomic access */
  return atomic32_read(&sbone_driver.bits) & (1 << dev_to_bit32(dev));
}

static inline unsigned int sbone_is_dev_valid(sbone_dev_t* dev)
{
  /* check if the device address is valid */
  if ((uintptr_t)dev < (uintptr_t)&sbone_driver.devs[0]) return 0;
  if ((uintptr_t)dev >= (uintptr_t)&sbone_driver.devs[SBONE_DEV_COUNT]) return 0;
  return 1;
}

static void sbone_dev_unmap_bars
(sbone_dev_t* sbone_dev, struct pci_dev* pci_dev __attribute__((unused)))
{
  const unsigned int nbar = sizeof(sbone_dev->vadr) / sizeof(sbone_dev->vadr[0]);

  unsigned int i;

  for (i = 0; i < nbar; ++i)
  {
    if (sbone_dev->vadr[i])
    {
      pci_release_region(pci_dev, (int)i);
      iounmap((void*)sbone_dev->vadr[i]);
      sbone_dev->vadr[i] = 0;
      sbone_dev->len[i] = 0;
    }
  }
}

static int sbone_dev_map_bars(sbone_dev_t* sbone_dev, struct pci_dev* pci_dev)
{
  const unsigned int nbar = sizeof(sbone_dev->vadr) / sizeof(sbone_dev->vadr[0]);
  int err = 0;
  unsigned int i;

  for (i = 0; i < nbar; ++i)
  {
    const unsigned long flags = pci_resource_flags(pci_dev, i);
    const uintptr_t addr = (uintptr_t)pci_resource_start(pci_dev, i);
    const size_t len = (size_t)pci_resource_len(pci_dev, i);

    if ((err == 0) && (flags & IORESOURCE_MEM) && (addr && len))
    {
      if ((err = pci_request_region(pci_dev, (int)i, SBONE_NAME)))
      {
	/* continue with error set */
	sbone_dev->len[i] = 0;
	sbone_dev->vadr[i] = 0;
      }
      else
      {
	sbone_dev->vadr[i] = (uintptr_t)ioremap_nocache(addr, len);
	sbone_dev->len[i] = len;
	if (sbone_dev->vadr[i] == 0)
	{
	  /* continue but zero remaining entries for unmapping */
	  sbone_dev->len[i] = 0;
	  err = -ENOMEM;
	}
      }
    }
    else
    {
      sbone_dev->vadr[i] = 0;
      sbone_dev->len[i] = 0;
    }
  }

  if (err) sbone_dev_unmap_bars(sbone_dev, pci_dev);
  return err;
}


/* PCI interface
 */

DEFINE_PCI_DEVICE_TABLE(sbone_pci_ids) =
{
  /* sbone */
  { PCI_DEVICE( 0x2a2a, 0x2b2b ) },
  { 0, }
};

/* used to export symbol for use by userland */
MODULE_DEVICE_TABLE(pci, sbone_pci_ids);

/* PCI error recovery */

static pci_ers_result_t sbone_pci_error_detected
(struct pci_dev* pci_dev, pci_channel_state_t state)
{
  printk(SBONE_TAG "sbone_pci_error_detected(0x%08x)\n", state);

  if (state == pci_channel_io_perm_failure)
    return PCI_ERS_RESULT_DISCONNECT;

  /* disable an request a slot reset */

  pci_disable_device(pci_dev);

  return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t sbone_pci_mmio_enabled(struct pci_dev* pci_dev)
{
  printk(SBONE_TAG "pci_mmio_enabled\n");
  return PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t sbone_pci_link_reset(struct pci_dev* pci_dev)
{
  printk(SBONE_TAG "sbone_pci_link_reset\n");
  return PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t sbone_pci_slot_reset(struct pci_dev* pci_dev)
{
  printk(SBONE_TAG "sbone_pci_slot_reset\n");
  return PCI_ERS_RESULT_RECOVERED;
}

static void sbone_pci_resume(struct pci_dev* pci_dev)
{
  printk(SBONE_TAG "sbone_pci_resume\n");
}

static struct pci_error_handlers sbone_err_handler =
{
  .error_detected = sbone_pci_error_detected,
  .mmio_enabled = sbone_pci_mmio_enabled,
  .slot_reset = sbone_pci_slot_reset,
  .link_reset = sbone_pci_link_reset,
  .resume = sbone_pci_resume
};

static int sbone_cdev_init(sbone_dev_t* dev);
static inline void sbone_cdev_fini(sbone_dev_t* dev);

/* linux-source-3.2/Documentation/PCI/pci.txt recommends __devinit */

static int __devinit sbone_pci_probe
(struct pci_dev* pci_dev, const struct pci_device_id* dev_id)
{
  sbone_dev_t* sbone_dev;
  int err;

  /* allocate device structure
   */

  printk("%s\n", __FUNCTION__);

  if ((sbone_dev = sbone_dev_alloc()) == NULL)
  {
    err = -ENOMEM;
    goto on_error_0;
  }

  sbone_dev->pci_dev = pci_dev;

  pci_set_drvdata(pci_dev, sbone_dev);

#if 1 /* FIXME: do not enable twice */
  if (pci_is_enabled(pci_dev) == 0)
#endif /* FIXME */
    if ((err = pci_enable_device(pci_dev)))
      goto on_error_1;

  /* map the device bars
   */

  if ((err = sbone_dev_map_bars(sbone_dev, pci_dev)))
  {
    printk("[!] sbone_dev_map() == %d\n", err);
    goto on_error_2;
  }

  /* install interrupt handler
   */

  if ((err = pci_enable_msi(pci_dev)))
  {
    printk("[!] pci_enable_msi() == %d\n", err);
    goto on_error_2;
  }

  /* printk("sbone: pci_enable_msi() == %d\n", pci_dev->irq); */

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
   SBONE_NAME,
   (void*)sbone_dev
  );

  if (err)
  {
    printk("[!] request_irq() == %d\n", err);
    goto on_error_3;
  }

  /* initialize char device
   */

  if ((err = sbone_cdev_init(sbone_dev)))
  {
    printk("[!] sbone_cdev_init() == %d\n", err);
    goto on_error_4;
  }

  /* success
   */

  return 0;

 on_error_4:
  free_irq(pci_dev->irq, sbone_dev);
 on_error_3:
  pci_disable_msi(pci_dev);
 on_error_2:
  sbone_dev_unmap_bars(sbone_dev, pci_dev);
  pci_disable_device(pci_dev);
 on_error_1:
  sbone_dev_free(sbone_dev);
  pci_set_drvdata(pci_dev, NULL);
 on_error_0:
  return err;
}

static void __devexit sbone_pci_remove(struct pci_dev* pci_dev)
{
  sbone_dev_t* const sbone_dev = pci_get_drvdata(pci_dev);

  SBONE_ASSERT(sbone_dev);

  printk(SBONE_TAG "sbone_pci_remove(%u)\n", dev_to_bit32(sbone_dev));

  sbone_cdev_fini(sbone_dev);

  free_irq(pci_dev->irq, sbone_dev);
  pci_disable_msi(pci_dev);

  sbone_dev_unmap_bars(sbone_dev, pci_dev);

  pci_disable_device(pci_dev);

  pci_set_drvdata(pci_dev, NULL);

  sbone_dev_free(sbone_dev);
}

static struct pci_driver sbone_pci_driver =
{
  .name = SBONE_NAME,
  .id_table = sbone_pci_ids,
  .probe = sbone_pci_probe,
  .remove = __devexit_p(sbone_pci_remove),
  .err_handler = &sbone_err_handler
};

static int sbone_pci_init(void)
{
  int err;

  if ((err = pci_register_driver(&sbone_pci_driver)))
  {
    printk(SBONE_TAG "pci_register_driver() == %d\n", err);
    return err;
  }

  return 0;
}

static void sbone_pci_fini(void)
{
  pci_unregister_driver(&sbone_pci_driver);
}


/* LINUX file interface
 */

static int sbone_file_open(struct inode* inode, struct file* file)
{
  /* TODO: open must be exclusive, because only one instance per device */

  sbone_dev_t* const dev = container_of(inode->i_cdev, sbone_dev_t, cdev);

  if (!(sbone_is_dev_valid(dev) && sbone_is_dev_allocated(dev)))
  {
    printk("sbone_file_open, invalid device\n");
    return -EFAULT;
  }

  file->private_data = dev;

  return 0;
}

static int sbone_file_close(struct inode* inode, struct file* file)
{
  /* sbone_dev_t* const dev = file->private_data; */
  /* SBONE_ASSERT(dev); */

  file->private_data = NULL;

  return 0;
}

#ifdef HAVE_UNLOCKED_IOCTL
static long sbone_file_ioctl
(struct file* file, unsigned int cmd, unsigned long uaddr)
{
  __attribute__((unused)) struct inode* const inode = file->f_dentry->d_inode;
#else
static int sbone_ioctl
(struct inode* inode, struct file* file, unsigned int cmd, unsigned long uaddr)
{
#endif

  sbone_dev_t* const dev = file->private_data;
  int err;

  SBONE_ASSERT(dev);

  switch (cmd)
  {
  case SBONE_IOCTL_GET_IRQCOUNT:
    {
      sbone_ioctl_get_irqcount_t arg;
      arg.count = (size_t)atomic32_and(&dev->irq_count, 0);
      if (copy_to_user((void*)(uintptr_t)uaddr, &arg, sizeof(arg)))
      {
	err = -EINVAL;
	break ;
      }
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

static unsigned int sbone_file_poll
(struct file* file, struct poll_table_struct* pts)
{
  sbone_dev_t* const dev = file->private_data;
  unsigned int mask = 0;

  poll_wait(file, &dev->wq, pts);

  if (atomic32_read(&dev->irq_count))
  {
    mask |= POLLIN | POLLRDNORM;
  }

  return mask;
}

static struct file_operations sbone_fops =
{
 .owner = THIS_MODULE,
#ifdef HAVE_UNLOCKED_IOCTL
 .unlocked_ioctl = sbone_file_ioctl,
#else
 .ioctl = sbone_file_ioctl,
#endif
 .open = sbone_file_open,
 .poll = sbone_file_poll,
 .release = sbone_file_close
};

static int sbone_cdev_init(sbone_dev_t* dev)
{
  /* register the chardev and associated file operations */

  const int minor = (int)dev_to_bit32(dev);
  const dev_t devno = MKDEV(sbone_driver.major, minor);

  cdev_init(&dev->cdev, &sbone_fops);
  dev->cdev.owner = THIS_MODULE;

  /* nothing to do on failure */
  return cdev_add(&dev->cdev, devno, 1);
}

static inline void sbone_cdev_fini(sbone_dev_t* dev)
{
  cdev_del(&dev->cdev);
}


/* LINUX module interface
 */

static int  __init sbone_init(void);
static void __exit sbone_exit(void);


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
module_init(sbone_init);
module_exit(sbone_exit);
#endif


static int __init sbone_init(void)
{
  static const unsigned int ndev = SBONE_DEV_COUNT;
  dev_t first_dev;
  int err;

  sbone_driver_init(&sbone_driver);

  /* references */
  /* http://hg.berlios.de/repos/kedr/file/b0f4d9d02d35/sources/examples/sample_target/cfake.c */
  /* http://lists.kernelnewbies.org/pipermail/kernelnewbies/2011-May/001660.html */

  /* dynamic major allocation */
  if ((err = alloc_chrdev_region(&first_dev, 0, ndev, SBONE_NAME)) < 0)
  {
    return err;
  }

  sbone_driver.major = MAJOR(first_dev);

  if ((err = sbone_pci_init()))
  {
    unregister_chrdev_region(first_dev, ndev);
    return err;
  }

  printk(SBONE_TAG "major: %d\n", sbone_driver.major);

  return 0;
}


static void __exit sbone_exit(void)
{
  /* note: sbone_pci_remove is called when unregistering pci driver */
  static const unsigned int ndev = SBONE_DEV_COUNT;
  const dev_t first_dev = MKDEV(sbone_driver.major, 0);
  sbone_pci_fini();
  unregister_chrdev_region(first_dev, ndev);
}
