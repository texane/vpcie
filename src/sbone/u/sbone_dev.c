#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <pci/pci.h>
#include "sbone_dev.h"
#include "sbone_err.h"


#define CONFIG_DEBUG 1
#if CONFIG_DEBUG
#include <stdio.h>
#define PRINTF(__s, ...)  \
do { printf(__s, ## __VA_ARGS__); } while (0)
#define PERROR() printf("[!] %d\n", __LINE__)
#define ASSERT(__x) if (!(__x)) printf("[!] %d\n", __LINE__)
#else
#define PRINTF(__s, ...)
#define PERROR()
#deifne ASSERT(__x)
#endif


static size_t get_bar_size(struct pci_access* a, struct pci_dev* d, size_t i)
{
  const enum pci_access_type saved_method = a->method;
  const int addr = PCI_BASE_ADDRESS_0 + (int)i * 4;
  uint32_t size;
  uint32_t saved_long;

  a->method = PCI_ACCESS_I386_TYPE1;

  saved_long = pci_read_long(d, addr);

  pci_write_long(d, addr, (uint32_t)-1);
  size = pci_read_long(d, addr);
  size &= ~PCI_ADDR_FLAG_MASK;
  size = ~size + 1;

  pci_write_long(d, addr, saved_long);
  
  a->method = saved_method;

  return size;
}

sbone_err_t sbone_dev_open_pcie
(sbone_dev_t* dev, int dom, int bus, int _dev, int fun)
{
  static const size_t nbar = sizeof(dev->bar_addrs) / sizeof(dev->bar_addrs[0]);
  int mem_fd;
  size_t i;
  struct pci_access* pci_access;
  struct pci_dev* pci_dev;
  int err = -1;

  /* zero device */

  for (i = 0; i < nbar; ++i) dev->bar_sizes[i] = 0;

  dev->nid = -1;

  /* find the pci device */

  pci_access = pci_alloc();
  if (pci_access == NULL) { PERROR(); goto on_error_0; }
  pci_init(pci_access);
  pci_scan_bus(pci_access);

  pci_dev = pci_get_dev(pci_access, dom, bus, _dev, fun);
  if (pci_dev == NULL) { PERROR(); goto on_error_1; }
  pci_fill_info(pci_dev, PCI_FILL_IDENT | PCI_FILL_BASES);

  /* map bars */

  mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (mem_fd == -1) { PERROR(); goto on_error_2; }

  for (i = 0; i < nbar; ++i)
  {
#if 1 /* FIXME: pci_dev->size[i] not detected by pci_fill_info ... */
    if (pci_dev->base_addr[i] != 0)
      pci_dev->size[i] = get_bar_size(pci_access, pci_dev, i);
#endif /* FIXME */

    dev->bar_sizes[i] = pci_dev->size[i];
    if (dev->bar_sizes[i] == 0) continue ;

    dev->bar_addrs[i] = (uintptr_t)mmap
    (
     NULL, pci_dev->size[i],
     PROT_READ | PROT_WRITE,
     MAP_SHARED | MAP_NORESERVE,
     mem_fd,
     pci_dev->base_addr[i]
    );

    if (dev->bar_addrs[i] == (uintptr_t)MAP_FAILED)
    {
      size_t j;
      for (j = 0; j < i; ++j)
	munmap((void*)dev->bar_addrs[i], dev->bar_sizes[i]);
      PERROR();
      goto on_error_3;
    }
  }

  if ((dev->fd = open("/dev/sbone", O_RDWR)) == -1)
  {
    PERROR();
    goto on_error_3;
  }

  /* success */

  err = 0;

 on_error_3:
  close(mem_fd);
 on_error_2:
  pci_free_dev(pci_dev);
 on_error_1:
  pci_cleanup(pci_access);
 on_error_0:
  return err;

}

sbone_err_t sbone_dev_close(sbone_dev_t* dev)
{
  static const size_t nbar = sizeof(dev->bar_addrs) / sizeof(dev->bar_addrs[0]);

  size_t i;

  for (i = 0; i < nbar; ++i)
  {
    if (dev->bar_sizes[i] == 0) continue ;
    munmap((void*)dev->bar_addrs[i], dev->bar_sizes[i]);
  }

  close(dev->fd);

  return SBONE_ERR_SUCCESS;
}
