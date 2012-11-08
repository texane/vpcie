#ifndef SBONE_IOCTL_H_INCLUDED
# define SBONE_IOCTL_H_INCLUDED


#define SBONE_IOCTL_MAGIC  's'

#define SBONE_IOCTL_BASE 0
#define SBONE_IOCTL_GET_IRQCOUNT \
  _IO(SBONE_IOCTL_MAGIC, SBONE_IOCTL_BASE + 0)

typedef struct sbone_ioctl_get_irqcount
{
  size_t count;
} sbone_ioctl_get_irqcount_t;


#endif /* SBONE_IOCTL_H_INCLUDED */
