library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;


package pcie_glue is

 procedure pcie_glue_poll_rx_fifo
 (
  is_read: out unsigned(7 downto 0);
  bar: out unsigned(7 downto 0);
  addr: out unsigned(63 downto 0);
  data: out unsigned(63 downto 0);
  size: out unsigned(15 downto 0)
 );
 attribute foreign of pcie_glue_poll_rx_fifo:
  procedure is "VHPIDIRECT pcie_glue_poll_rx_fifo";

 procedure pcie_glue_send_reply
 (
  data: in unsigned(63 downto 0);
  size: in unsigned(15 downto 0)
 );
 attribute foreign of pcie_glue_send_reply:
  procedure is "VHPIDIRECT pcie_glue_send_reply";

 procedure pcie_glue_send_msi;
 attribute foreign of pcie_glue_send_msi:
  procedure is "VHPIDIRECT pcie_glue_send_msi";

 procedure pcie_glue_send_write
 (
  addr: in unsigned(63 downto 0);
  data: in unsigned(63 downto 0);
  size: in unsigned(15 downto 0)
 );
 attribute foreign of pcie_glue_send_write:
  procedure is "VHPIDIRECT pcie_glue_send_write";

 type pcie_glue_buf_t is array(0 to 1023) of unsigned(7 downto 0);

 procedure pcie_glue_send_write_buf
 (
  addr: in unsigned(63 downto 0);
  buf: in pcie_glue_buf_t;
  size: in unsigned(15 downto 0)
 );
 attribute foreign of pcie_glue_send_write_buf:
  procedure is "VHPIDIRECT pcie_glue_send_write_buf";

end package;

package body pcie_glue is

 procedure pcie_glue_poll_rx_fifo
 (
  is_read: out unsigned(7 downto 0);
  bar: out unsigned(7 downto 0);
  addr: out unsigned(63 downto 0);
  data: out unsigned(63 downto 0);
  size: out unsigned(15 downto 0)
 )
 is begin
  assert false report "VHPI" severity failure;
 end pcie_glue_poll_rx_fifo;

 procedure pcie_glue_send_reply
 (
  data: in unsigned(63 downto 0);
  size: in unsigned(15 downto 0)
 )
 is begin
  assert false report "VHPI" severity failure;
 end pcie_glue_send_reply;

 procedure pcie_glue_send_msi
 is begin
  assert false report "VHPI" severity failure;
 end pcie_glue_send_msi;

 procedure pcie_glue_send_write
 (
  addr: in unsigned(63 downto 0);
  data: in unsigned(63 downto 0);
  size: in unsigned(15 downto 0)
 )
 is begin
  assert false report "VHPI" severity failure;
 end pcie_glue_send_write;

 procedure pcie_glue_send_write_buf
 (
  addr: in unsigned(63 downto 0);
  buf: in pcie_glue_buf_t;
  size: in unsigned(15 downto 0)
 )
 is begin
  assert false report "VHPI" severity failure;
 end pcie_glue_send_write_buf;

end pcie_glue;
