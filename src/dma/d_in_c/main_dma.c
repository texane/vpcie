#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "pcie.h"

/* simple dma engine

   bar[0] 32 bits registers:
   0. DMA_REG_CTL
   1. DMA_REG_STA
   2. DMA_REG_ADL
   3. DMA_REG_ADH
   4. DMA_REG_BAZ

   operating theory:
   the DMA engine is connected to an internal 32KB BRAM memory
   whose contents are initialized once at main time with an increasing
   pattern such that the byte at bram[i] = i.
   DMA_REG_BAZ[7:0] contains a value that is added to bram[i] when the
   transfer occur. it is defaulted to 0.
   DMA_REG_CTL[15:0] contains the size to transfer, in bytes.
   together, DMA_REG_ADL and DMA_REG_ADH form the 64 bits address
   to write the data to (low and high parts).
   when DMA_REG_CTL[31] is set, the transfer starts. DMA_REG_STA[31]
   is automatically cleared when the transfer starts, and set to one
   when the transfer ends. DMA_REG_STA[15:0] is updated with the byte
   count actually transfered.
   if set to 1 DMA_REG_CTL[30], a MSI occurs when the transfer ends.
*/

typedef struct dma
{
  pcie_dev_t dev;

  /* device registers */
#define DMA_REG_CTL 0
#define DMA_REG_STA 1
#define DMA_REG_ADL 2
#define DMA_REG_ADH 3
#define DMA_REG_BAZ 4
#define DMA_REG_COUNT 5
  uint32_t regs[DMA_REG_COUNT];

  /* bram (must be a multiple of page size) */
  uint8_t bram[8 * 0x1000];

  /* context for the dma completion callback */
  uint32_t saved_ctl;
  uint32_t saved_adh;
  uint32_t saved_adl;
  uint32_t saved_baz;

} dma_t;

#define DMA_REG_ADDR(__r) ((DMA_REG_ ## __r) * sizeof(uint32_t))

#define CONFIG_DEBUG 1
#if CONFIG_DEBUG
#include <stdio.h>
#define PRINTF(__s, ...) \
do { printf(__s, ## __VA_ARGS__); } while (0)
#define PERROR() printf("[!] %d\n", __LINE__)
#define ASSERT(__x) if (!(__x)) printf("[!] %d\n", __LINE__)
#else
#define PRINTF(__s, ...)
#define PERROR()
#define ASSERT(__x)
#endif

static void on_read(uint64_t addr, void* data, size_t size, void* opak)
{
  dma_t* const dma = (dma_t*)opak;

  PRINTF("%s (0x%lx)\n", __FUNCTION__, (unsigned long)addr);

  switch (addr)
  {
  case DMA_REG_ADDR(CTL):
  case DMA_REG_ADDR(STA):
  case DMA_REG_ADDR(ADL):
  case DMA_REG_ADDR(ADH):
  case DMA_REG_ADDR(BAZ):
    memcpy(data, (uint8_t*)dma->regs + addr, size);
    break ;

  default:
    memset(data, 0xff, size);
    break ;
  }
}

static void finalize_transfer(void* opak)
{
  pcie_net_msg_t* m;
  dma_t* const dma = (dma_t*)opak;
  pcie_dev_t* const dev = &dma->dev;
  unsigned int n;
  unsigned int i;
  unsigned int j;
  const uint8_t* p;
  size_t size_per_m;

  PRINTF("%s\n", __FUNCTION__);

  /* dma writes are multiple of page size. limit to 1 page per message. */
  ASSERT(PCIE_NET_MSG_MAX_SIZE - offsetof(pcie_net_msg_t, data));
  ASSERT(!((PCIE_NET_MSG_MAX_SIZE - offsetof(pcie_net_msg_t, data)) % 0x1000));
  size_per_m = 0x1000;

  if ((m = malloc(PCIE_NET_MSG_MAX_SIZE)) == NULL) return ;
  m->op = PCIE_NET_OP_WRITE_MEM;
  m->addr = ((uint64_t)dma->saved_adh << 32) | (uint64_t)dma->saved_adl;

  /* do the actual dma transfer */
  n = sizeof(dma->bram) / size_per_m;
  p = dma->bram;
  m->size = size_per_m;
  for (i = 0; i < n; ++i)
  {
    memcpy(m->data, p, size_per_m);
    /* TODO: one pass */
    for (j = 0; j < size_per_m; ++j) m->data[j] += (uint8_t)dma->saved_baz;
    pcie_net_send_msg(&dev->net, m);
    p += size_per_m;
    m->addr += size_per_m;
  }

  m->size = sizeof(dma->bram) % size_per_m;
  if (m->size)
  {
    memcpy(m->data, p, m->size);
    /* TODO: one pass */
    for (j = 0; j < m->size; ++j) m->data[j] += (uint8_t)dma->saved_baz;
    pcie_net_send_msg(&dev->net, m);
  }

  free(m);

  /* set byte count transmited, and clar transfer in progress flag. */
  dma->regs[DMA_REG_STA] = (1 << 31) | (dma->saved_ctl & 0xffff);

  /* send MSI if enabled */
  if (dma->saved_ctl & (1 << 30)) pcie_send_msi(dev);
}

static void on_write(uint64_t addr, const void* data, size_t size, void* opak)
{
  dma_t* const dma = (dma_t*)opak;
  pcie_dev_t* const dev = &dma->dev;

  PRINTF("%s (0x%lx)\n", __FUNCTION__, (unsigned long)addr);

  /* common to all registers */
  memcpy((uint8_t*)dma->regs + addr, data, size);

  if (addr == DMA_REG_ADDR(CTL))
  {
    const uint32_t r = dma->regs[DMA_REG_CTL];

    /* start transfer */
    if (r & (1 << 31))
    {
      /* capture context */
      dma->saved_ctl = r;
      dma->saved_adl = dma->regs[DMA_REG_ADL];
      dma->saved_adh = dma->regs[DMA_REG_ADH];
      dma->saved_baz = dma->regs[DMA_REG_BAZ];

      dma->regs[DMA_REG_STA] = 0;

      /* simulate some delay in operation with blocking */
      pcie_add_task(dev, 1000, finalize_transfer, dma);
    }
  }
}


/* device entry point */

int main(int ac, char** av)
{
  const char* const laddr = av[1];
  const char* const lport = av[2];
  const char* const raddr = av[3];
  const char* const rport = av[4];

  unsigned int i;

  dma_t dma;

  /* initialize bram, increasing pattern */
  for (i = 0; i < sizeof(dma.bram); ++i) dma.bram[i] = (uint8_t)i;

  if (pcie_init_net(&dma.dev, laddr, lport, raddr, rport) == -1) return -1;

  pcie_set_vendorid(&dma.dev, 0x2a2a);
  pcie_set_deviceid(&dma.dev, 0x2b2b);
  pcie_set_bar(&dma.dev, 0, 0x100, on_read, on_write, &dma);

  pcie_loop(&dma.dev);

  pcie_fini(&dma.dev);

  return 0;
}
