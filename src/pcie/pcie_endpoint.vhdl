library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library work;
use work.pcie;
use std.textio.all;

entity endpoint is
 port
 (
  rst: in std_ulogic;
  clk: in std_ulogic;

  req_en: out std_ulogic;
  req_wr: out std_ulogic;
  req_bar: out std_ulogic_vector(pcie.BAR_WIDTH - 1 downto 0);
  req_addr: out std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);
  req_data: out std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

  rep_en: in std_ulogic;
  rep_data: in std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

  mwr_en: in std_ulogic;
  mwr_addr: in std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);
  mwr_data: in std_ulogic_vector(pcie.PAYLOAD_WIDTH - 1 downto 0);
  mwr_size: in std_ulogic_vector(pcie.SIZE_WIDTH - 1 downto 0);

  msi_en: in std_ulogic
 );
end entity;


architecture behav of endpoint is
begin

 process(rst, clk)
  variable l: line;
  variable var_is_read: unsigned(7 downto 0);
  variable var_bar: unsigned(7 downto 0);
  variable var_addr: unsigned(63 downto 0);
  variable var_data: unsigned(63 downto 0);
  variable var_size: unsigned(15 downto 0);
  variable var_mwr_data: unsigned(pcie.PAYLOAD_WIDTH - 1 downto 0);
 begin
  if rst = '1' then
  elsif rising_edge(clk) then

   req_en <= '0';
   req_wr <= '0';
   req_bar <= (others => '0');
   req_addr <= (others => '0');

   -- msi
   if msi_en = '1' then
    write(l, String'("MSI"));
    writeline(output, l);
    work.pcie.glue_send_msi;
   end if;

   -- mwr
   if mwr_en = '1' then
    write(l, String'("MWR"));
    writeline(output, l);
    var_addr := unsigned(mwr_addr);
    var_mwr_data := unsigned(mwr_data);
    var_size := unsigned(mwr_size);
    work.pcie.glue_send_write(var_addr, var_mwr_data, var_size);
   end if;

   -- reply
   if rep_en = '1' then
    write(l, String'("replying"));
    writeline(output, l);
    var_data := unsigned(rep_data);
    work.pcie.glue_send_reply(var_data);
   end if;

   work.pcie.glue_poll_rx_fifo
    (var_is_read, var_bar, var_addr, var_data, var_size);

   -- there is an access
   if var_size /= "00" then
    req_en <= '1';
    req_bar <= std_ulogic_vector(var_bar(2 downto 0));
    req_addr <= std_ulogic_vector(var_addr);

    write(l, String'("size "));
    write(l, integer'image(to_integer(var_size)));
    writeline(output, l);

    -- write access
    if var_is_read = "00" then
     req_data <= std_ulogic_vector(var_data);
     req_wr <= '1';
    end if;

   end if; -- var_size
  end if; -- rising_edge
 end process;

end behav;
