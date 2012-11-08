#ifndef PCIE_H_INCLUDED
# define PCIE_H_INCLUDED


#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <pci/header.h>
#include "pcie_net.h"


struct pcie_dev;

typedef void (*pcie_readfn_t)(uint64_t, void*, size_t, void*);

typedef void (*pcie_writefn_t)(uint64_t, const void*, size_t, void*);

typedef struct pcie_dev
{
  pcie_net_t net;

#define PCIE_BAR_COUNT 6
  uint64_t bar_size[PCIE_BAR_COUNT];
  pcie_readfn_t bar_readfn[PCIE_BAR_COUNT];
  pcie_writefn_t bar_writefn[PCIE_BAR_COUNT];
  void* bar_data[PCIE_BAR_COUNT];

  /* extended config space */
  uint8_t config[0x1000];

} pcie_dev_t;


/* device initialization */

int pcie_init_net
(pcie_dev_t*, const char*, const char*, const char*, const char*);
int pcie_fini(pcie_dev_t*);

/* main device loop */

int pcie_loop(pcie_dev_t*);

/* config space access */

static int pcie_check_config(pcie_dev_t* dev, uint64_t addr, size_t size)
{
  /* assume size ok */
  if (addr & (size - 1)) return -1;
  if ((addr + size) > sizeof(dev->config)) return -1;
  return 0;
}

static inline int pcie_write_config_safe
(pcie_dev_t* dev, uint64_t addr, const void* data, size_t size)
{
  memcpy(dev->config + addr, data, size);
  return 0;
}

static inline int pcie_read_config_safe
(pcie_dev_t* dev, uint64_t addr, void* data, size_t size)
{
  memcpy(data, dev->config + addr, size);
  return 0;
}

static inline int pcie_write_config
(pcie_dev_t* dev, uint64_t addr, const void* data, size_t size)
{
  /* check then safe */
  if (pcie_check_config(dev, addr, size)) return -1;
  return pcie_write_config_safe(dev, addr, data, size);
}

static inline int pcie_read_config
(pcie_dev_t* dev, uint64_t addr, void* data, size_t size)
{
  /* check then safe */
  if (pcie_check_config(dev, addr, size)) return -1;
  return pcie_read_config_safe(dev, addr, data, size);
}

/* TODO: use macro preprocessor */

static inline int pcie_write_config_byte
(pcie_dev_t* dev, uint64_t addr, uint8_t data)
{ return pcie_write_config(dev, addr, &data, sizeof(data)); }

static inline int pcie_write_config_word
(pcie_dev_t* dev, uint64_t addr, uint16_t data)
{ return pcie_write_config(dev, addr, &data, sizeof(data)); }

static inline int pcie_write_config_long
(pcie_dev_t* dev, uint64_t addr, uint32_t data)
{ return pcie_write_config(dev, addr, &data, sizeof(data)); }

static inline uint8_t pcie_read_config_byte
(pcie_dev_t* dev, uint64_t addr)
{
  uint8_t data;
  if (pcie_read_config(dev, addr, &data, sizeof(data)))
    data = (uint8_t)-1;
  return data;
}

static inline uint16_t pcie_read_config_word
(pcie_dev_t* dev, uint64_t addr)
{
  uint16_t data;
  if (pcie_read_config(dev, addr, &data, sizeof(data)))
    data = (uint16_t)-1;
  return data;
}

static inline uint32_t pcie_read_config_long
(pcie_dev_t* dev, uint64_t addr)
{
  uint32_t data;
  if (pcie_read_config(dev, addr, &data, sizeof(data)))
    data = (uint32_t)-1;
  return data;
}

/* config space accessors */

static inline int pcie_set_vendorid(pcie_dev_t* dev, uint16_t id)
{
  return pcie_write_config_safe(dev, PCI_VENDOR_ID, &id, sizeof(id));
}

static inline int pcie_set_deviceid(pcie_dev_t* dev, uint16_t id)
{
  return pcie_write_config_safe(dev, PCI_DEVICE_ID, &id, sizeof(id));
}

/* define bar and methods */

int pcie_set_bar
(pcie_dev_t*, unsigned long, size_t, pcie_readfn_t, pcie_writefn_t, void*);

/* msi */

int pcie_send_msi(pcie_dev_t*);

/* add a task to perform in usec */
int pcie_add_task(pcie_dev_t*, unsigned long, pcie_net_taskfn_t, void*);

/* add an event */
int pcie_add_event(pcie_dev_t*, int, pcie_net_evfn_t, void*);


#endif /* ! PCIE_H_INCLUDED */
