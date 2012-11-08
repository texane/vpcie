#include <stdint.h>
#include <string.h>
#include "pcie.h"
#include "pcie_net.h"


#define CONFIG_DEBUG 1
#if CONFIG_DEBUG
#include <stdio.h>
#define PRINTF(__s, ...)  \
do { printf(__s, ## __VA_ARGS__); } while (0)
#define PERROR() printf("[!] %d\n", __LINE__)
#else
#define PRINTF(__s, ...)
#define PERROR()
#endif


static void init_common(pcie_dev_t* dev)
{
  unsigned int i;

  for (i = 0; i < PCIE_BAR_COUNT; ++i)
  {
    dev->bar_size[i] = 0;
    dev->bar_writefn[i] = NULL;
    dev->bar_readfn[i] = NULL;
  }

  memset(dev->config, 0, sizeof(dev->config));

  /* TODO: use pcie_write_config_xxx_safe versions */
  
  /* pcie endpoint header */
  pcie_write_config_byte(dev, PCI_HEADER_TYPE, 0);

  /* dacq device class */
  pcie_write_config_long(dev, PCI_CLASS_REVISION, PCI_CLASS_SIGNAL_OTHER << 16);

  /* capabilities list */
  pcie_write_config_word(dev, PCI_STATUS, PCI_STATUS_CAP_LIST);

  /* create a 32 bits MSI capability structure. address must
     dword aligned caplist offset. must lie in lower 48 bytes
     but after PCI configuration registers.
   */
#define MSI_CAP_OFF (16 * 4)
  pcie_write_config_byte(dev, PCI_CAPABILITY_LIST, MSI_CAP_OFF);
  pcie_write_config_byte(dev, MSI_CAP_OFF + 0x00, 0x05);
  pcie_write_config_byte(dev, MSI_CAP_OFF + 0x01, 0x00);
  pcie_write_config_word(dev, MSI_CAP_OFF + 0x02, 0x01);
}


/* exported api */

int pcie_init_net
(
 pcie_dev_t* dev,
 const char* laddr, const char* lport,
 const char* raddr, const char* rport
)
{
  /* initialize pcie endpoint with pcie_net transport layer */

  init_common(dev);

  if (pcie_net_init(&dev->net, laddr, lport, raddr, rport) == -1)
    return -1;

  return 0;
}

int pcie_fini(pcie_dev_t* dev)
{
  pcie_net_fini(&dev->net);
  return 0;
}

int pcie_set_bar
(
 pcie_dev_t* dev,
 unsigned long ibar, size_t size,
 pcie_readfn_t on_read,
 pcie_writefn_t on_write,
 void* data
)
{
  /* size in bytes. must be a power of 2. */

  dev->bar_size[ibar] = size;
  dev->bar_readfn[ibar] = on_read;
  dev->bar_writefn[ibar] = on_write;
  dev->bar_data[ibar] = data;

  return 0;
}

int pcie_add_task
(pcie_dev_t* dev, unsigned long usecs, pcie_net_taskfn_t f, void* p)
{
  struct timeval tm;
  tm.tv_sec = usecs / 1000000;
  tm.tv_usec = usecs % 1000000;
  return pcie_net_add_task(&dev->net, &tm, f, p);
}

int pcie_add_event(pcie_dev_t* dev, int fd, pcie_net_evfn_t fn, void* data)
{
  return pcie_net_add_ev(&dev->net, fd, fn, data);
}

/* device main loop routine */

static void on_write_config(pcie_dev_t* dev, const pcie_net_msg_t* msg)
{
  /* writing a bar register requires special handling.
     non decoded (ie. low bits) must be cleared if set.
     this scheme is used to probe the BAR size.
   */

  if ((msg->addr >= PCI_BASE_ADDRESS_0) && (msg->addr <= PCI_BASE_ADDRESS_5))
  {
    const unsigned int ibar =
      (msg->addr - PCI_BASE_ADDRESS_0) / sizeof(uint32_t);

    if (dev->bar_size[ibar])
    {
      const uint32_t size = dev->bar_size[ibar];
      const uint32_t data = *(uint32_t*)msg->data;
      pcie_write_config_long(dev, msg->addr, data & ~(size - 1));
    }

    return ;
  }
  else if (msg->addr == PCI_ROM_ADDRESS)
  {
    return ;
  }

  /* default write case */
  switch (msg->width)
  {
  case 1: pcie_write_config_byte(dev, msg->addr, *(uint8_t*)msg->data); break ;
  case 2: pcie_write_config_word(dev, msg->addr, *(uint16_t*)msg->data); break ;
  case 4: pcie_write_config_long(dev, msg->addr, *(uint32_t*)msg->data); break ;
  default: break ;
  }
}

static void on_read_config
(pcie_dev_t* dev, const pcie_net_msg_t* msg, pcie_net_reply_t* reply)
{
  uint64_t data;

  reply->status = 0;
  switch (msg->width)
  {
  case 1: data = (uint64_t)pcie_read_config_byte(dev, msg->addr); break ;
  case 2: data = (uint64_t)pcie_read_config_word(dev, msg->addr); break ;
  case 4: data = (uint64_t)pcie_read_config_long(dev, msg->addr); break ;
  default: data = (uint64_t)-1; break ;
  }

  *(uint64_t*)reply->data = data;
}

static unsigned int on_msg_recv
(
 const pcie_net_msg_t* msg,
 pcie_net_reply_t* reply,
 void* opak
)
{
  pcie_dev_t* const dev = (pcie_dev_t*)opak;
  unsigned int must_reply = 0;

  PRINTF("%s(%u, 0x%lx, %u, %x)\n", __FUNCTION__, msg->op, msg->addr, msg->bar, msg->width);

  switch (msg->op)
  {
  case PCIE_NET_OP_READ_CONFIG:
    {
      on_read_config(dev, msg, reply);
      must_reply = 1;
      break ;
    }

  case PCIE_NET_OP_WRITE_CONFIG:
    {
      on_write_config(dev, msg);
      break ;
    }

  case PCIE_NET_OP_READ_MEM:
    must_reply = 1;
    reply->status = 0;
    *(uint64_t*)reply->data = (uint64_t)-1;
    if (msg->bar >= PCIE_BAR_COUNT) break ;
    if (dev->bar_size[msg->bar] == 0) break ;
    if (dev->bar_readfn[msg->bar] == NULL) break ;
    *(uint64_t*)reply->data = 0; /* remove bits due to (uint64_t)-1 */
    dev->bar_readfn[msg->bar]
      (msg->addr, (void*)reply->data, msg->width, dev->bar_data[msg->bar]);
    break ;

  case PCIE_NET_OP_WRITE_MEM:
    if (msg->bar >= PCIE_BAR_COUNT) break ;
    if (dev->bar_size[msg->bar] == 0) break ;
    if (dev->bar_writefn[msg->bar] == NULL) break ;
    dev->bar_writefn[msg->bar]
      (msg->addr, (void*)msg->data, msg->width, dev->bar_data[msg->bar]);
    break ;

  case PCIE_NET_OP_READ_IO:
    /* TODO: not implemented */
    reply->status = 0;
    *(uint64_t*)reply->data = (uint64_t)-1;
    must_reply = 1;
    break ;

  case PCIE_NET_OP_WRITE_IO:
    /* TODO: not implemented */
    break ;

  default:
    break ;
  }

  return must_reply;
}

int pcie_loop(pcie_dev_t* dev)
{
  return pcie_net_loop(&dev->net, on_msg_recv, dev);
}

int pcie_send_msi(pcie_dev_t* dev)
{
  uint8_t buf[offsetof(pcie_net_msg_t, data) + sizeof(uint64_t)];
  pcie_net_msg_t* const msg = (pcie_net_msg_t*)buf;

  msg->op = PCIE_NET_OP_MSI;
  msg->size = sizeof(uint64_t);
  *(uint64_t*)msg->data = 0;

  return pcie_net_send_msg(&dev->net, msg);
}
