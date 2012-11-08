#ifndef PCIE_NET_H_INCLUDED
# define PCIE_NET_H_INCLUDED


#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>
#include <sys/types.h>


typedef struct pcie_net_header
{
  uint16_t size;
} __attribute__((packed)) pcie_net_header_t;

typedef struct pcie_net_msg
{
  /* arrange so that data can be page sized */
#define PCIE_NET_MSG_MAX_SIZE (offsetof(pcie_net_msg_t, data) + 0x1000)

  pcie_net_header_t header;

#define PCIE_NET_OP_READ_CONFIG 0
#define PCIE_NET_OP_WRITE_CONFIG 1
#define PCIE_NET_OP_READ_MEM 2
#define PCIE_NET_OP_WRITE_MEM 3
#define PCIE_NET_OP_READ_IO 4
#define PCIE_NET_OP_WRITE_IO 5
#define PCIE_NET_OP_INT 6
#define PCIE_NET_OP_MSI 7
#define PCIE_NET_OP_MSIX 8

  uint8_t op; /* in PCIE_NET_OP_XXX */
  uint8_t bar; /* in [0:5] */
  uint8_t width; /* access in 1, 2, 4, 8 */
  uint64_t addr;
  uint16_t size; /* data size, in bytes */
  uint8_t data[1];

} __attribute__((packed)) pcie_net_msg_t;

typedef struct pcie_net_reply
{
  pcie_net_header_t header;
  uint8_t status;
  uint8_t data[8];
} __attribute__((packed)) pcie_net_reply_t;

struct pcie_net;

/* return 1 if a reply must be sent */
typedef unsigned int (*pcie_net_recvfn_t)
(const pcie_net_msg_t*, pcie_net_reply_t*, void*);

typedef void (*pcie_net_taskfn_t)(void*);

typedef int (*pcie_net_evfn_t)(unsigned int, void*);

typedef struct pcie_net
{
#if (CONFIG_USE_UDP == 0)
  /* accepting socket fd */
  int server_fd;
#endif

  /* peer socket fd */
  int fd;

  /* event */
  int ev_fd;
  pcie_net_evfn_t ev_fn;
  void* ev_data;

  /* next task to be scheduled */
  struct timeval task_tm;
  pcie_net_taskfn_t task_fn;
  void* task_data;

} pcie_net_t;


int pcie_net_init
(pcie_net_t*, const char*, const char*, const char*, const char*);
int pcie_net_fini(pcie_net_t*);
int pcie_net_loop(pcie_net_t*, pcie_net_recvfn_t, void*);
int pcie_net_add_task
(pcie_net_t*, const struct timeval*, pcie_net_taskfn_t, void*);
int pcie_net_add_ev
(pcie_net_t*, int, pcie_net_evfn_t, void*);
ssize_t pcie_net_send_buf(pcie_net_t*, const void*, size_t);

static inline int pcie_net_send_msg(pcie_net_t* n, pcie_net_msg_t* m)
{
  const size_t size = offsetof(pcie_net_msg_t, data) + m->size;
  m->header.size = size;
  return pcie_net_send_buf(n, (const void*)m, size);
}

static inline int pcie_net_send_reply(pcie_net_t* n, pcie_net_reply_t* r)
{
  r->header.size = sizeof(*r);
  return pcie_net_send_buf(n, (const void*)r, sizeof(*r));
}

ssize_t pcie_net_recv_buf(pcie_net_t*, void*, size_t);

static inline int pcie_net_recv_msg(pcie_net_t* net, pcie_net_msg_t* m)
{
  /* -1 for error, 0 for success, 1 for neither a message, nor an error */
  const ssize_t n = pcie_net_recv_buf(net, m, PCIE_NET_MSG_MAX_SIZE);
  if (n < 0) return -1;
  else if (n == 0) return 1;
  return 0;
}

__attribute__((unused))
static inline int pcie_net_recv_reply(pcie_net_t* net, pcie_net_reply_t* r)
{
  /* -1 for error, 0 for success, 1 for neither a message, nor an error */
  const ssize_t n = pcie_net_recv_buf(net, r, sizeof(*r));
  if (n < 0) return -1;
  else if (n == 0) return 1;
  return 0;
}


#endif /* PCIE_NET_H_INCLUDED */
