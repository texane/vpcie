#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include "sbone_dev.h"
#include "../k/sbone_ioctl.h"


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


static inline int get_irqcount(int fd, size_t* count)
{
  sbone_ioctl_get_irqcount_t arg;
  if (ioctl(fd, SBONE_IOCTL_GET_IRQCOUNT, &arg))
  {
    perror("ioctl");
    PERROR();
    return -1;
  }
  *count = arg.count;
  return 0;
}

static inline int clear_irqcount(int fd)
{
  size_t fu;
  return get_irqcount(fd, &fu);
}

static inline uint64_t r(sbone_dev_t* d, uint64_t a)
{
  /* read register at addr bar1, a */

#if 0 /* 64 bits accesses not supported by qemu */
  return *(const volatile uint64_t*)(d->bar_addrs[1] + a);
#else
  return *(const volatile uint32_t*)(d->bar_addrs[1] + a);
#endif
}

static inline void w(sbone_dev_t* d, uint64_t a, uint64_t x)
{
  /* write register at bar1, a */

#if 0 /* 64 bits accesses not supported by qemu */
  *(volatile uint64_t*)(d->bar_addrs[1] + a) = x;
#else
  *(volatile uint32_t*)(d->bar_addrs[1] + a) = x;
#endif
}

static inline uint64_t e(unsigned int i)
{
  /* expand (uint8_t)i to 64 bits */
  uint64_t x;
  memset((void*)&x, i, sizeof(x));
  return x;
}

/* main */

int main(int ac, char** av)
{
  /* TODO: sbone_dev_foreach */
  static const int dom = 0x00;
  static const int bus = 0x00;
  static const int _dev = 0x04;
  static const int fun = 0x00;

  unsigned int i;
  sbone_dev_t dev;

  if (sbone_dev_open_pcie(&dev, dom, bus, _dev, fun))
  {
    PERROR();
    return -1;
  }

  if (dev.bar_sizes[1] == 0)
  {
    PERROR();
    sbone_dev_close(&dev);
    return -1;
  }

  for (i = 0; i < 4; ++i) w(&dev, i * 8, e(i));
  printf("%lx\n", r(&dev, 0x10));
  printf("%lx\n", r(&dev, 0x18));

  /* test irq by writting to the irq triggering register */
  usleep(1000000);
  clear_irqcount(dev.fd);
  w(&dev, 0x20, 0);
  while (1)
  {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(dev.fd, &fds);
    if (select(dev.fd + 1, &fds, NULL, NULL, NULL) > 0)
    {
      size_t nirqs = 0;
      get_irqcount(dev.fd, &nirqs);
      printf("got interrupt: %lu\n", nirqs);
      break ;
    }

    /* retry */
  }

  sbone_dev_close(&dev);

  return 0;
}
