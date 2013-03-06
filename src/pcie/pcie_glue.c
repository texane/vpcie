#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include "pcie.h"


#define CONFIG_DEBUG 1
#if CONFIG_DEBUG
#include <stdio.h>
#define PRINTF(__s, ...)  \
do { printf(__s, ## __VA_ARGS__); } while (0)
#define PERROR() printf("[!] %s %d\n", __FUNCTION__, __LINE__)
#else
#define PRINTF(__s, ...)
#define PERROR()
#endif


/* fifos shared between io and ghdl thread */

typedef struct fnode
{
  struct fnode* next;

  /* bar access, reply or message to send */
  union
  {
    /* bar access */
    struct
    {
      /* TODO: use a sleeping mechanism instead */
      /* TODO: avoid false sharing */
      volatile unsigned int is_replied;

      unsigned int is_read;
      unsigned int bar;
      uint64_t addr;
      unsigned int size;
      uint8_t data[8];
    } bar_access;

    /* message to send */
    pcie_net_msg_t msg;

  } u;

  /* nothing must follow, since msg is variable sized */

} fnode_t;

typedef struct fifo
{
  fnode_t* volatile head;
  fnode_t* volatile tail;
  pthread_mutex_t lock;
} fifo_t;

static int fifo_init(fifo_t* f)
{
  f->head = NULL;
  f->tail = NULL;
  pthread_mutex_init(&f->lock, NULL);
  return 0;
}

static void fifo_fini(fifo_t* f)
{
  fnode_t* i = f->head;

  while (i)
  {
    fnode_t* const tmp = i;
    i = i->next;
    free(tmp);
  }

  pthread_mutex_destroy(&f->lock);
}

static inline fnode_t* fifo_alloc_node(size_t size)
{
  fnode_t* const node = malloc(size);
  node->next = NULL;
  return node;
}

static fnode_t* fifo_alloc_access_node
(
 unsigned int is_read,
 unsigned int bar,
 uint64_t addr,
 size_t size,
 const void* data
)
{
  fnode_t* const node = fifo_alloc_node(sizeof(fnode_t));
  node->u.bar_access.is_replied = 0;
  node->u.bar_access.is_read = is_read;
  node->u.bar_access.bar = bar;
  node->u.bar_access.addr = addr;
  node->u.bar_access.size = size;
  memcpy(node->u.bar_access.data, data, size);
  return node;
}

static void fifo_push_node(fifo_t* f, fnode_t* n)
{
  /* push tail, pop head */

  pthread_mutex_lock(&f->lock);
  if (f->head == NULL) f->head = n;
  else f->tail->next = n;
  f->tail = n;
  pthread_mutex_unlock(&f->lock);
}


/* io thread, get/put requests from/to network */

typedef struct thread_context
{
  /* pcie device */
  pcie_dev_t dev;

  /* from ghdl to network */
  fifo_t tx_fifo;

  /* signal: ghdl pushed something, exit required ... */
  int ev_fds[2];

  /* from network to ghdl */
  fifo_t rx_fifo;

  /* single being replied node */
  fnode_t* volatile reply_node;

  volatile unsigned int state;

} thread_context_t;


/* device access routines */

static void on_bar_read
(unsigned int bar, uint64_t addr, void* data, size_t size, void* opak)
{
  thread_context_t* const c = opak;
  fnode_t* const node = fifo_alloc_access_node(1, bar, addr, size, data);

  PRINTF("%s\n", __FUNCTION__);

  /* will be popped on next poll */
  fifo_push_node(&c->rx_fifo, node);

  /* TODO: use a pthread_event */
  PRINTF(">> is_replied == 0\n");
  while (node->u.bar_access.is_replied == 0) usleep(10000);
  PRINTF(">> is_replied == 1\n");

  if (size > node->u.bar_access.size) size = node->u.bar_access.size;
  memcpy(data, node->u.bar_access.data, size);

  free(node);
}

static void on_bar_write
(unsigned int bar, uint64_t addr, const void* data, size_t size, void* opak)
{
  thread_context_t* const c = opak;
  fnode_t* const node = fifo_alloc_access_node(0, bar, addr, size, data);

  PRINTF("%s\n", __FUNCTION__);

  fifo_push_node(&c->rx_fifo, node);
}

#define DEFINE_BAR_HANDLERS(__n)				\
__attribute__((unused)) static void on_bar ## __n ## _read	\
(uint64_t addr, void* data, size_t size, void* opak)		\
{ on_bar_read(__n, addr, data, size, opak); }			\
__attribute__((unused)) static void on_bar ## __n ## _write	\
(uint64_t addr, const void* data, size_t size, void* opak)	\
{ on_bar_write(__n, addr, data, size, opak); }

DEFINE_BAR_HANDLERS(0);
DEFINE_BAR_HANDLERS(1);
DEFINE_BAR_HANDLERS(2);
DEFINE_BAR_HANDLERS(3);
DEFINE_BAR_HANDLERS(4);
DEFINE_BAR_HANDLERS(5);


#define EVK_BASE 0x2a2a2a2a
static const unsigned int evk_quit = EVK_BASE + 0;
static const unsigned int evk_push = EVK_BASE + 1;

static int on_event(unsigned int key, void* opak)
{
  thread_context_t* const c = opak;
  pcie_dev_t* const dev = &c->dev;

  fnode_t* head;

  if (key == evk_quit)
  {
    return -1;
  }

  /* else, process tx_fifo nodes */

  pthread_mutex_lock(&c->tx_fifo.lock);
  head = c->tx_fifo.head;
  c->tx_fifo.head = NULL;
  c->tx_fifo.tail = NULL;
  pthread_mutex_unlock(&c->tx_fifo.lock);

  /* pushed at tail, process head first */

  while (head)
  {
    fnode_t* const pos = head;
    head = head->next;
    pcie_net_send_msg(&dev->net, &pos->u.msg);
    free(pos);
  }

  return 0;
}


/* thread entry point and singleton context */

static thread_context_t g_thread_context;

static void* thread_entry(void* fu)
{
  thread_context_t* const c = &g_thread_context;
  pcie_dev_t* const dev = &c->dev;
  unsigned int bar_size[PCIE_BAR_COUNT] = { 0, };
  pcie_readfn_t bar_readfn[PCIE_BAR_COUNT];
  pcie_writefn_t bar_writefn[PCIE_BAR_COUNT];
  unsigned int vendor_id = 0;
  unsigned int device_id = 0;
  unsigned int i;
  const char* s;
  const char* laddr;
  const char* lport;
  const char* raddr;
  const char* rport;

  if ((laddr = getenv("PCIE_INET_LADDR")) == NULL)
    laddr = "127.0.0.1";

  if ((lport = getenv("PCIE_INET_LPORT")) == NULL)
    lport = "42425";

  if ((raddr = getenv("PCIE_INET_RADDR")) == NULL)
    raddr = "127.0.0.1";

  if ((rport = getenv("PCIE_INET_RPORT")) == NULL)
    rport = "42424";

  if ((s = getenv("PCIE_VENDOR_ID")) != NULL)
    vendor_id = (unsigned int)strtoul(s, NULL, 16);

  if ((s = getenv("PCIE_DEVICE_ID")) != NULL)
    device_id = (unsigned int)strtoul(s, NULL, 16);

#if (PCIE_BAR_COUNT >= 1)
  if ((s = getenv("PCIE_BAR0_SIZE")) != NULL)
    bar_size[0] = (size_t)strtoul(s, NULL, 16);
  bar_readfn[0] = on_bar0_read;
  bar_writefn[0] = on_bar0_write;
#endif

#if (PCIE_BAR_COUNT >= 2)
  if ((s = getenv("PCIE_BAR1_SIZE")) != NULL)
    bar_size[1] = (size_t)strtoul(s, NULL, 16);
  bar_readfn[1] = on_bar1_read;
  bar_writefn[1] = on_bar1_write;
#endif

#if (PCIE_BAR_COUNT >= 3)
  if ((s = getenv("PCIE_BAR2_SIZE")) != NULL)
    bar_size[2] = (size_t)strtoul(s, NULL, 16);
  bar_readfn[2] = on_bar2_read;
  bar_writefn[2] = on_bar2_write;
#endif

#if (PCIE_BAR_COUNT >= 4)
  if ((s = getenv("PCIE_BAR3_SIZE")) != NULL)
    bar_size[3] = (size_t)strtoul(s, NULL, 16);
  bar_readfn[3] = on_bar3_read;
  bar_writefn[3] = on_bar3_write;
#endif

#if (PCIE_BAR_COUNT >= 5)
  if ((s = getenv("PCIE_BAR4_SIZE")) != NULL)
    bar_size[4] = (size_t)strtoul(s, NULL, 16);
  bar_readfn[4] = on_bar4_read;
  bar_writefn[4] = on_bar4_write;
#endif

#if (PCIE_BAR_COUNT >= 6)
  if ((s = getenv("PCIE_BAR5_SIZE")) != NULL)
    bar_size[5] = (size_t)strtoul(s, NULL, 16);
  bar_readfn[5] = on_bar5_read;
  bar_writefn[5] = on_bar5_write;
#endif

  pipe(c->ev_fds);

  fifo_init(&c->tx_fifo);
  fifo_init(&c->rx_fifo);
  c->reply_node = NULL;

  /* state is inited already done by spawner */
  /* c->state = 0; */

  pcie_init_net(dev, laddr, lport, raddr, rport);
  pcie_add_event(dev, c->ev_fds[0], on_event, c);
  pcie_set_vendorid(dev, vendor_id);
  pcie_set_deviceid(dev, device_id);
  for (i = 0; i < PCIE_BAR_COUNT; ++i)
    pcie_set_bar(dev, i, bar_size[i], bar_readfn[i], bar_writefn[i], c);

  /* commit the status */
  c->state = 1;

  while (c->state != 0) pcie_loop(dev);

  pcie_fini(dev);

  fifo_fini(&c->tx_fifo);
  fifo_fini(&c->rx_fifo);

  close(c->ev_fds[0]);
  close(c->ev_fds[1]);

  return NULL;
}


/* ghdl argument conversion routines, refer to ghpi.h */

struct int_bounds
{
  int left;
  int right;
  char dir;
  unsigned int len;
};

typedef struct fat_pointer
{
  void* base;
  struct int_bounds* bounds;
} fat_pointer_t;

static const uint8_t glue_levels[] = { 2, 3 };

#define UINT_TO_LOGIC_TEMPLATE(__w)					  \
__attribute__((unused))							  \
static void uint ## __w ## _to_logic(uint ## __w ## _t x, uint8_t* buf)	  \
{									  \
  ssize_t i;								  \
  for (i = __w - 1; i >= 0; --i, ++buf) *buf = glue_levels[(x >> i) & 1]; \
}

UINT_TO_LOGIC_TEMPLATE(8);
UINT_TO_LOGIC_TEMPLATE(16);
UINT_TO_LOGIC_TEMPLATE(32);
UINT_TO_LOGIC_TEMPLATE(64);

#define LOGIC_TO_UINT_TEMPLATE(__w)					  \
__attribute__((unused))							  \
static uint ## __w ## _t logic_to_uint ## __w(const uint8_t* buf)	  \
{									  \
  ssize_t i;								  \
  uint ## __w ## _t x = 0;						  \
  for (i = __w - 1; i >= 0; --i, ++buf)					  \
  {									  \
    if (*buf == glue_levels[1]) x |= ((uint ## __w ## _t)1) << i;	  \
  }									  \
  return x;								  \
}

LOGIC_TO_UINT_TEMPLATE(8);
LOGIC_TO_UINT_TEMPLATE(16);
LOGIC_TO_UINT_TEMPLATE(32);
LOGIC_TO_UINT_TEMPLATE(64);


static void logic_to_buf(const uint8_t* data, size_t data_size, uint8_t* buf)
{
  /* convert data[data_size] in logic format to buf */
  size_t i;
  for (i = 0; i < data_size; ++i) buf[i] = logic_to_uint8(data + i * 8);
}


/* routines exported for ghdl thread */

static fnode_t* alloc_write_node(uint8_t op, uint64_t addr, uint16_t size)
{
  fnode_t* node;
  pcie_net_msg_t* m;

  node = fifo_alloc_node(offsetof(fnode_t, u.msg.data) + size);

  m = &node->u.msg;
  m->op = op;
  m->size = size;
  m->addr = addr;

  return node;
}

void pcie_glue_send_msi(void)
{
  thread_context_t* const c = &g_thread_context;
  fnode_t* node;

  node = alloc_write_node(PCIE_NET_OP_MSI, 0, sizeof(uint64_t));
  memset(node->u.msg.data, 0, sizeof(uint64_t));

  fifo_push_node(&c->tx_fifo, node);
  write(c->ev_fds[1], &evk_push, sizeof(evk_push));
}

void pcie_glue_send_write_buf
(const uint8_t* _addr, const fat_pointer_t* buf, const uint8_t* _data_size)
{
  /* addr: uint64_t */
  /* data_size: uint16_t */

  thread_context_t* const c = &g_thread_context;
  fnode_t* node;

  const uint64_t addr = logic_to_uint64(_addr);
  const uint16_t data_size = logic_to_uint16(_data_size);
  /* const size_t data_size = buf->bounds->len; */
  size_t i;

  node = alloc_write_node(PCIE_NET_OP_WRITE_MEM, addr, data_size);

  for (i = 0; i < data_size; ++i)
    node->u.msg.data[i] = logic_to_uint8((uint8_t*)buf->base + i);

  fifo_push_node(&c->tx_fifo, node);
  write(c->ev_fds[1], &evk_push, sizeof(evk_push));
}

void pcie_glue_send_write
(const uint8_t* _addr, const uint8_t* _data, const uint8_t* _data_size)
{
  /* addr: uint64_t */
  /* data[0 < data_size < PCIE_PAYLOAD_WIDTH] */
  /* data_size: uint16_t */

  thread_context_t* const c = &g_thread_context;
  fnode_t* node;

  const uint64_t addr = logic_to_uint64(_addr);
  const uint16_t data_size = logic_to_uint16(_data_size);

  PRINTF("%s, %u@0x%lx\n", __FUNCTION__, data_size, addr);

  node = alloc_write_node(PCIE_NET_OP_WRITE_MEM, addr, data_size);

  logic_to_buf(_data, data_size, node->u.msg.data);

  fifo_push_node(&c->tx_fifo, node);
  write(c->ev_fds[1], &evk_push, sizeof(evk_push));
}

void pcie_glue_send_reply(const uint8_t* _data)
{
  /* data: uint64_t */

  thread_context_t* const c = &g_thread_context;
  fnode_t* const node = c->reply_node;
  const uint64_t data = logic_to_uint64(_data);

  PRINTF("%s, 0x%lx\n", __FUNCTION__, data);

  /* should_not_occur */
  if (node == NULL)
  {
    PRINTF("%s: more_replies_than_requests\n", __FUNCTION__);
    return ;
  }
  /* should_not_occur */

  c->reply_node = NULL;
  memcpy(node->u.bar_access.data, &data, sizeof(uint64_t));

  /* io thread is waiting on this for a reply */
  __sync_synchronize();
  node->u.bar_access.is_replied = 1;
  /* node can no longer be accessed, may be freed */
}

void pcie_glue_poll_rx_fifo
(uint8_t* is_read, uint8_t* bar, uint8_t* addr, uint8_t* data, uint8_t* size)
{
  /* is_read: uint8_t */
  /* bar: uint8_t */
  /* addr: uint64_t */
  /* data: uint64_t */
  /* size: uint16_t */

  thread_context_t* const c = &g_thread_context;
  fnode_t* node;

  /* pop head first */
  pthread_mutex_lock(&c->rx_fifo.lock);
  if ((node = c->rx_fifo.head))
  {
    c->rx_fifo.head = node->next;
    if (c->rx_fifo.head == NULL) c->rx_fifo.tail = NULL;
  }
  pthread_mutex_unlock(&c->rx_fifo.lock);

  if (node)
  {
    PRINTF("%s\n", __FUNCTION__);
    PRINTF(". is_read: %x\n", node->u.bar_access.is_read);
    PRINTF(". bar: %x\n", node->u.bar_access.bar);
    PRINTF(". addr: %lx\n", node->u.bar_access.addr);
    PRINTF(". size: %x\n", node->u.bar_access.size);

    uint8_to_logic((uint8_t)node->u.bar_access.is_read, is_read);
    uint8_to_logic((uint8_t)node->u.bar_access.bar, bar);
    uint64_to_logic((uint64_t)node->u.bar_access.addr, addr);
    uint16_to_logic((uint16_t)node->u.bar_access.size, size);

    /* release only if write access. otherwise put in reply_node. */
    if (node->u.bar_access.is_read == 0)
    {
      uint64_t x = 0;
      memcpy(&x, node->u.bar_access.data, node->u.bar_access.size);
      PRINTF(". data: 0x%lx\n", x);
      uint64_to_logic(x, data);
      free(node);
    }
    else
    {
      /* should_not_occur */
      if (c->reply_node != NULL)
      {
	PRINTF("arg->reply_node != NULL\n");
	free(node);
	goto empty_case;
      }
      /* should_not_occur */

      c->reply_node = node;
    }
  }
  else
  {
  empty_case:
    uint8_to_logic(0, is_read);
    uint8_to_logic(0, bar);
    uint64_to_logic(0, data);
    uint16_to_logic(0, size);
  }
}


/* called by main */

int pcie_glue_create_thread(pthread_t* thread_handle)
{
  void* thread_res;

  /* init before spawning */
  g_thread_context.state = 0;

  if (pthread_create(thread_handle, NULL, thread_entry, NULL))
  {
    PERROR();
    return -1;
  }

  while (g_thread_context.state == 0) ;

  /* thread start failed, do not wait */
  if (g_thread_context.state != 1)
  {
    PERROR();
    pthread_join(*thread_handle, &thread_res);
    return -1;
  }

  return 0;
}

void pcie_glue_join_thread(pthread_t thread_handle)
{
  void* thread_res;

  g_thread_context.state = 0;
  __sync_synchronize();
  write(g_thread_context.ev_fds[1], &evk_quit, sizeof(evk_quit));

  pthread_join(thread_handle, &thread_res);
}
