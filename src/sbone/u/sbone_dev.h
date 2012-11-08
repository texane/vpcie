#ifndef SBONE_DEV_H_INCLUDED
# define SBONE_DEV_H_INCLUDED


#include <stdint.h>
#include <sys/types.h>
#include "sbone_err.h"


typedef struct sbone_dev
{
  int fd;

  /* numa node id */
  int nid;

  /* pcie related */
  uintptr_t bar_addrs[6];
  size_t bar_sizes[6];

} sbone_dev_t;


/* device */

sbone_err_t sbone_dev_open_pcie(sbone_dev_t*, int, int, int, int);
sbone_err_t sbone_dev_close(sbone_dev_t*);

static inline int sbone_dev_fd(const sbone_dev_t* dev)
{ return dev->fd; }


#endif /* ! SBONE_DEV_H_INCLUDED */
