/* minimalistic main: create pcie_glue thread then call ghdl_main */

#include <pthread.h>

extern int ghdl_main(int, char**, char**);
extern int pcie_glue_create_thread(pthread_t*);
extern void pcie_glue_join_thread(pthread_t);

int main(int ac, char** av, char** env)
{
  pthread_t thread_handle;

  if (pcie_glue_create_thread(&thread_handle)) return -1;
  ghdl_main(ac, av, env);
  pcie_glue_join_thread(thread_handle);
  return 0;
}
