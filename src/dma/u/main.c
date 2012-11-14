#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/mman.h>


/* maximum dev count at same time */
#define CONFIG_MAX_DEV 1

/* thin device abstraction layer */

#include "kdma_ioctl.h"

static inline int pci_dev_open(void)
{
  return open("/dev/kdma0", O_RDWR | O_NONBLOCK);
}

static inline void pci_dev_close(int fd)
{
  close(fd);
}

static int pci_dev_readwrite
(
 int fd,
 unsigned int bar, uintptr_t off,
 uint32_t* addr, unsigned int wcount,
 unsigned long op
)
{
  kdma_ioctl_readwrite_t readwrite;

  readwrite.bar = bar;
  readwrite.off = off;
  readwrite.addr = (uintptr_t)addr;
  readwrite.word32_count = wcount;

  if (ioctl(fd, op, (unsigned long)(uintptr_t)&readwrite))
  {
    printf("readwrite error\n");
    return -1;
  }

  return 0;
}

static inline int pci_dev_write
(int fd, unsigned int bar, uintptr_t off, uint32_t* addr, unsigned int wcount)
{
  return pci_dev_readwrite(fd, bar, off, addr, wcount, KDMA_IOCTL_WRITE_BAR);
}

static inline int pci_dev_read
(int fd, unsigned int bar, uintptr_t off, uint32_t* addr, unsigned int wcount)
{
  return pci_dev_readwrite(fd, bar, off, addr, wcount, KDMA_IOCTL_READ_BAR);
}

static int pci_dev_poll_int(int fd, uint32_t* sta)
{
  kdma_ioctl_irqcontext_t irqcontext;
  const int e = ioctl(fd, KDMA_IOCTL_GET_IRQCONTEXT, &irqcontext);
  if (e) { perror("ioctl"); return -1; }
  *sta = irqcontext.sta;
  return 0;
}

static inline int pci_dev_write_word32
(int fd, unsigned int bar, uintptr_t off, uint32_t data)
{
  return pci_dev_write(fd, bar, off, &data, 1);
}

static inline int pci_dev_read_word32
(int fd, unsigned int bar, uintptr_t off, uint32_t* data)
{
  return pci_dev_read(fd, bar, off, data, 1);
}


/* dma api */

#define CONFIG_PAGE_SIZE 4096

typedef struct dma_handle
{
  int fd;
  int nid;
} dma_handle_t;

typedef struct dma_buf
{
  size_t size;
  unsigned char* vaddr;
  uintptr_t paddr;
} dma_buf_t;

typedef struct dma_io
{
  dma_buf_t* buf;

  /* statistics */
  uint64_t start_ticks;
  uint64_t stop_ticks;

  /* baz register */
  uint32_t baz;

  /* status register at completion time */
  uint32_t sta;

} dma_io_t;


/* helper routines */

static inline unsigned int dword_to_byte(unsigned int n)
{
#define DWORD_SIZE 4
  return n * DWORD_SIZE;
}

static inline unsigned int byte_to_dword(unsigned int n)
{
  /* warning: rounded down */
  return n / DWORD_SIZE;
}

static int dma_open(dma_handle_t* h)
{
  /* init the dma subsystem */

  if ((h->fd = pci_dev_open()) == -1) return -1;

  /* TODO:  retrieve NUMA infomation */
  h->nid = -1;

  return 0;
}

static void dma_close(dma_handle_t* h)
{
  /* finalize the dma subsystem */
  pci_dev_close(h->fd);
  h->fd = -1;
}

static int dma_alloc_buf(dma_handle_t* h, dma_buf_t* buf, size_t size)
{
  /* size the size in bytes */

  int err;
  int rem;
  kdma_ioctl_pmem_t pmem;

  /* round size to a page_size (thus dword) multiple */
  rem = size % CONFIG_PAGE_SIZE;
  if (rem) size += CONFIG_PAGE_SIZE - rem;

  buf->size = size;

  pmem.nid = h->nid;
  pmem.word32_count = byte_to_dword(size);
  if ((err = ioctl(h->fd, KDMA_IOCTL_PMEM_ALLOC, &pmem))) return err;

  pmem.vaddr = (uintptr_t)mmap
  (
   NULL, size,
   PROT_READ | PROT_WRITE,
   MAP_SHARED | MAP_NORESERVE,
   h->fd,
   pmem.paddr
  );

  if (pmem.vaddr == (uintptr_t)MAP_FAILED)
  {
    ioctl(h->fd, KDMA_IOCTL_PMEM_FREE, &pmem);
    return errno;
  }

  buf->vaddr = (void*)pmem.vaddr;
  buf->paddr = pmem.paddr;

  return 0;
}

static void dma_free_buf(dma_handle_t* h, dma_buf_t* buf)
{
  kdma_ioctl_pmem_t pmem;
  munmap(buf->vaddr, buf->size);
  pmem.paddr = buf->paddr;
  ioctl(h->fd, KDMA_IOCTL_PMEM_FREE, &pmem);
}

static int poll_int_blocking(dma_handle_t* h, uint32_t* sta)
{
  /* ms a timeout, in milliseconds */

  fd_set fds;

  FD_ZERO(&fds);
  FD_SET(h->fd, &fds);
  if (select(h->fd + 1, &fds, NULL, NULL, NULL) <= 0) return -1;

  return pci_dev_poll_int(h->fd, sta);
}

static int dma_sync_pmem(dma_handle_t* h, dma_buf_t* buf)
{
  kdma_ioctl_pmem_t pmem;

  pmem.word32_count = byte_to_dword(buf->size);
  pmem.paddr = buf->paddr;
  return ioctl(h->fd, KDMA_IOCTL_PMEM_SYNC, (unsigned long)(uintptr_t)&pmem);
}

static int dma_start_read(dma_handle_t* h, dma_io_t* io, dma_buf_t* buf)
{
  /* initiate a dma read transfer into buf */

#define DMA_REG_BAR 0x01
#define DMA_REG_CTL 0x00
#define DMA_REG_STA 0x04
#define DMA_REG_ADL 0x08
#define DMA_REG_ADH 0x0c
#define DMA_REG_BAZ 0x10

  uint32_t x;

  io->baz = rand() % 0x100;

  io->buf = buf;

  /* synchronize physical memory with cache hierarchy */
  dma_sync_pmem(h, buf);

  /* set baz seed */
  pci_dev_write_word32(h->fd, DMA_REG_BAR, DMA_REG_BAZ, io->baz);

  /* set 64 bits address */
  x = (uint32_t)(buf->paddr >> 32);
  pci_dev_write_word32(h->fd, DMA_REG_BAR, DMA_REG_ADH, x);
  x = (uint32_t)(buf->paddr & 0xffffffff);
  pci_dev_write_word32(h->fd, DMA_REG_BAR, DMA_REG_ADL, x);

  /* set size, start transfer, enable msi */
  x = (1 << 31) | (1 << 30) | buf->size;
  pci_dev_write_word32(h->fd, DMA_REG_BAR, DMA_REG_CTL, x);

  return 0;
}

static int dma_wait_io(dma_handle_t* h, dma_io_t* io)
{
  /* wait for a previously initiated dma transfer */

  /* poll registers for completion (dx_stat.running == 0) */
  while (1)
  {
    if (poll_int_blocking(h, &io->sta) < 0)
    {
      printf("poll_int_blocking TIMEOUT or ERROR\n");
      return -1;
    }

    /* transfer done */
    if (io->sta & (1 << 31)) break ;
  }

  /* synchronize physical memory with cache hierarchy */
  dma_sync_pmem(h, io->buf);

  return 0;
}


/* fill memory with a known pattern */

__attribute__((unused)) static unsigned int seed = 0;

static int fill_mem(dma_handle_t* h, dma_buf_t* buf)
{
  /* TODO */
  return 0;
}


/* for testing */

static void reset_buffer(dma_buf_t* buf)
{
  unsigned int i;
  for (i = 0; i < buf->size; ++i) buf->vaddr[i] = 0x2a;
}

static int check_buf(const dma_buf_t* buf, unsigned int baz)
{
  unsigned int i;

#if 0 /* use for C device */
  for (i = 0; i < buf->size; ++i)
  {
    /* increasing pattern filled by main_dma */
    if ((uint8_t)(i + baz) != buf->vaddr[i])
    {
      printf("check_buf error at %u 0x%x\n", i, buf->vaddr[i]);
      return -1;
    }
  }
#else /* use for VHDL device */

#define PCIE_PAYLOAD_WIDTH 128
  for (i = 0; i < buf->size; ++i)
  {
    if (buf->vaddr[i] != (uint8_t)baz)
    {
      printf("check_buf error at %u 0x%x\n", i, buf->vaddr[i]);
      return -1;
    }
  }

#endif /* use for X device */

  return 0;
}

static int check_io(dma_io_t* io)
{
  /* check memory against known values */

  if ((io->sta & 0xffff) != io->buf->size)
  {
    printf("invalid count: 0x%x\n", (unsigned int)io->sta & 0xffff);
    return -1;
  }

  return check_buf(io->buf, io->baz);
}


static inline double to_payload_mbps(unsigned long size, uint64_t usecs)
{
  /* convert size per usecs in megabyte per second.
     the returned value is for payload data only, not taking PCIE
     meta into account.
   */

  return (1000000 * (double)size) / ((double)usecs * 1024 * 1024);
}


static inline double to_raw_mbps(unsigned long size, uint64_t usecs)
{
  /* convert size per usecs in megabyte per second.
     the returned value is a guess for raw PCIE data, ie. payload and meta
  */

  static const double coding_overhead = 1.2;

#if 0 /* for io read access */
  static const uint64_t max_payload_size = 4;
#else /* for dma access */
  static const uint64_t max_payload_size = 128;
#endif

  /* framing symbols(3) +
     data seq number (2) + data lcrc (4) +
     tlp header (12) + tlp digest (4)
  */
  static const uint64_t meta_size = 25;

  uint64_t npackets;
  uint64_t total_size;

  npackets = size / max_payload_size;
  if (size % 128) ++npackets;

  total_size = (uint64_t)
    ((double)(npackets * meta_size + size) * coding_overhead);

  return (1000000 * (double)total_size) / ((double)usecs * 1024 * 1024);
}


static double to_mtps(unsigned long size, uint64_t usecs)
{
  const double mbps = to_raw_mbps(size, usecs);
  return mbps *  8;
}


static inline uint64_t rdtsc(void)
{
  uint32_t a, d;
  __asm__ __volatile__
  (
   "rdtsc \n\t"
   : "=a"(a), "=d"(d)
  );
  return ((uint64_t)d << 32) | a;
}

static inline uint64_t sub_ticks(uint64_t a, uint64_t b)
{
  if (a > b) return UINT64_MAX - a + b;
  return b - a;
}

static uint64_t get_cpu_hz(void)
{
  /* return the cpu freq, in mhz */

  static uint64_t cpu_hz = 0;
  unsigned int i;
  unsigned int n;
  struct timeval tms[3];
  uint64_t ticks[2];
  uint64_t all_ticks[10];

  if (cpu_hz != 0) return cpu_hz;

  n = sizeof(all_ticks) / sizeof(all_ticks[0]);
  for (i = 0; i < n; ++i)
  {
    gettimeofday(&tms[0], NULL);
    ticks[0] = rdtsc();
    while (1)
    {
      gettimeofday(&tms[1], NULL);
      timersub(&tms[1], &tms[0], &tms[2]);

      if (tms[2].tv_usec >= 9998)
      {
	ticks[1] = rdtsc();
	all_ticks[i] = sub_ticks(ticks[0], ticks[1]);
	break ;
      }
    }
  }

  cpu_hz = 0;
  for (i = 0; i < n; ++i) cpu_hz += all_ticks[i];
  cpu_hz *= 10;

  return cpu_hz;
}

static inline uint64_t ticks_to_us(uint64_t ticks)
{
#if 0 /* pc_oliver */
# define CONFIG_CPU_MHZ 2793066
#elif 0 /* comex006 */
# define CONFIG_CPU_MHZ 1299841
#endif
  return (ticks * 1000000) / get_cpu_hz();
}


/* main */

int main(int ac, char** av)
{
  /* assume 1 <= ndev <= CONFIG_MAX_DEV */
  const unsigned int ndev = 1;

  int err = -1;
  dma_handle_t h[CONFIG_MAX_DEV];
  dma_buf_t buf[CONFIG_MAX_DEV];
  dma_io_t io[CONFIG_MAX_DEV];
  unsigned int i;
  unsigned int j;

  uint64_t start_ticks;
  uint64_t stop_ticks;
  uint64_t diff_ticks;
  uint64_t this_usecs;
  uint64_t total_usecs;

  uint64_t total_size;

  /* init DMAs */
  for (j = 0; j < ndev; ++j)
  {
    if (dma_open(&h[j])) goto on_error_0;

#define CONFIG_MEM_SIZE (32 * 1024) /* in bytes */
    if (dma_alloc_buf(&h[j], &buf[j], CONFIG_MEM_SIZE))
      goto on_error_2;

    reset_buffer(&buf[j]);
  }

  total_usecs = 0;

  /* infinite loop */
  for (i = 0; 1; ++i)
  {
    /* reset memories */
    for (j = 0; j < ndev; ++j)
    {
      if (fill_mem(&h[j], &buf[j])) goto on_error_1;
      reset_buffer(&buf[j]);
    }

    start_ticks = rdtsc();

    /* TODO: handle errors */

    /* start transfers */
    for (j = 0; j < ndev; ++j)
    {
      dma_start_read(&h[j], &io[j], &buf[j]);
      io[j].start_ticks = rdtsc();
    }

    /* wait for all the transfer to be done */
    for (j = 0; j < ndev; ++j)
    {
      dma_wait_io(&h[j], &io[j]);
      io[j].stop_ticks = rdtsc();
    }

    stop_ticks = rdtsc();

    /* per device report */
    for (j = 0; j < ndev; ++j)
    {
      diff_ticks = sub_ticks(io[j].start_ticks, io[j].stop_ticks);
      this_usecs = ticks_to_us(diff_ticks);
      total_usecs += this_usecs;
      total_size = buf[j].size;
      printf("-- %u\n", j);
      printf("time         : %llu\n", (unsigned long long)this_usecs);
      printf("payload_mBps : %lf\n", to_payload_mbps(total_size, this_usecs));
      printf("mtps         : %lf\n", to_mtps(total_size, this_usecs));
    }
    printf("\n");

    /* check buffers */
    for (j = 0; j < ndev; ++j)
    {
      if (io[j].buf && check_io(&io[j]))
      {
	printf("error at %u\n", i);
	return -1;
	break ;
      }
    }
  }

  /* success */
  err = 0;

 on_error_1:
  for (j = 0; j < ndev; ++j) dma_free_buf(&h[j], &buf[j]);
 on_error_2:
  for (j = 0; j < ndev; ++j) dma_close(&h[j]);
 on_error_0:
  return err;
}
