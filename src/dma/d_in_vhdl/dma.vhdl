--
-- pcie bar and address comparator
--


library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.pcie;


entity bar_addr_cmp is
 generic
 (
  GENERIC_BAR: natural;
  GENERIC_ADDR: natural
 );
 port
 (
  bar: in std_ulogic_vector(pcie.BAR_WIDTH - 1 downto 0);
  addr: in std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);
  is_eq: out std_ulogic
 );
end bar_addr_cmp;


architecture rtl of bar_addr_cmp is
begin
 -- combinatory logic
 process(bar, addr)
 begin
  is_eq <= '0';
  if (unsigned(bar) = GENERIC_BAR) and (unsigned(addr) = GENERIC_ADDR) then
   is_eq <= '1';
  end if;
 end process;
end rtl;


--
-- pcie write only register
--


library ieee;
use ieee.std_logic_1164.all;
use work.pcie;


entity reg_wo is
 generic
 (
  GENERIC_BAR: natural;
  GENERIC_ADDR: natural
 );
 port
 (
  -- synchronous logic
  rst: in std_ulogic;
  clk: in std_ulogic;

  -- clear contents
  clr_en: in std_ulogic;

  -- pcie request
  req_en: in std_ulogic;
  req_wr: in std_ulogic;
  req_bar: in std_ulogic_vector(pcie.BAR_WIDTH - 1 downto 0);
  req_addr: in std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);
  req_data: in std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

  -- latched data
  reg_data: out std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0)
 );
end entity;


architecture rtl of reg_wo is
 signal data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);
 signal is_eq: std_ulogic;
 signal is_en: std_ulogic;
begin

 bar_addr_cmp_entity: entity work.bar_addr_cmp
 generic map
 (
  GENERIC_BAR => GENERIC_BAR,
  GENERIC_ADDR => GENERIC_ADDR
 )
 port map
 (
  bar => req_bar,
  addr => req_addr,
  is_eq => is_eq
 );

 is_en <= is_eq and req_en and req_wr;

 process(rst, clk)
 begin
  if rst = '1' then
   data <= (others => '0');
  elsif rising_edge(clk) then
   if clr_en = '1' then
    data <= (others => '0');
   elsif is_en = '1' then
    data <= req_data;
   end if;
   reg_data <= data;
  end if;
 end process;

end rtl;


--
-- pcie readwrite register
--

library ieee;
use ieee.std_logic_1164.all;
use work.pcie;


entity reg_ro is
 generic
 (
  GENERIC_BAR: natural;
  GENERIC_ADDR: natural
 );
 port
 (
  -- synchronous logic
  rst: in std_ulogic;
  clk: in std_ulogic;

  -- set contents
  set_en: in std_ulogic;
  set_data: in std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

  -- pcie request
  req_en: in std_ulogic;
  req_wr: in std_ulogic;
  req_bar: in std_ulogic_vector(pcie.BAR_WIDTH - 1 downto 0);
  req_addr: in std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);

  -- pcie reply
  rep_en: out std_ulogic;
  rep_data: out std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);

  -- latched data
  reg_data: out std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0)
 );
end entity;


architecture rtl of reg_ro is
 signal data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);
 signal is_eq: std_ulogic;
 signal is_en: std_ulogic;
begin

 bar_addr_cmp_entity: entity work.bar_addr_cmp
 generic map
 (
  GENERIC_BAR => GENERIC_BAR,
  GENERIC_ADDR => GENERIC_ADDR
 )
 port map
 (
  bar => req_bar,
  addr => req_addr,
  is_eq => is_eq
 );

 is_en <= is_eq and req_en and (not req_wr);

 process(rst, clk)
 begin
  if rst = '1' then
   data <= (others => '0');
  elsif rising_edge(clk) then
   -- always disable, even if not selected
   rep_en <= '0';

   -- is_en when read request
   if is_en = '1' then
    rep_data <= data;
    rep_en <= '1';
   end if; -- is_en

   reg_data <= data;

  end if; -- rising_edge
 end process;

end rtl;


--
-- dma engine
--
-- bar[1] 32 bits registers:
-- 0. DMA_REG_CTL
-- 1. DMA_REG_STA
-- 2. DMA_REG_ADL
-- 3. DMA_REG_ADH
-- 4. DMA_REG_BAZ
--


library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;
use work.pcie;


entity dma is
 generic
 (
  -- address in bar[0]
  GENERIC_ADDR: natural := 16#00#
 );
 port
 (
  rst: in std_ulogic;
  clk: in std_ulogic;

  req_en: in std_ulogic;
  req_wr: in std_ulogic;
  req_bar: in std_ulogic_vector(pcie.BAR_WIDTH - 1 downto 0);
  req_addr: in std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);
  req_data: in std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

  rep_en: out std_ulogic;
  rep_data: out std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);

  mwr_en: out std_ulogic;
  mwr_addr: out std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);
  mwr_data: out std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);
  mwr_size: out std_ulogic_vector(pcie.SIZE_WIDTH - 1 downto 0);

  msi_en: out std_ulogic
 );
end entity;


architecture rtl of dma is

 -- register latched values
 signal ctl_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);
 signal sta_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);
 signal adl_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);
 signal adh_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);
 signal baz_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

 -- status register signals
 signal sta_set_en: std_ulogic;
 signal sta_set_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

 -- clear control register
 signal ctl_clr_en: std_ulogic;

 -- dma engine
 type dma_state_t is (idle, write, done);
 attribute enum_encoding: string;
 attribute enum_encoding of dma_state_t : type is ("001 010 100");
 signal dma_state: dma_state_t;
 signal dma_next_state: dma_state_t;
 signal dma_start: std_ulogic;
 signal dma_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);
 signal dma_addr: unsigned(pcie.ADDR_WIDTH - 1 downto 0);
 signal dma_size: unsigned(15 downto 0);
 signal dma_off: unsigned(15 downto 0);

 constant DMA_BLOCK_SIZE: unsigned(15 downto 0) := x"0008";

begin

 dma_start <= ctl_data(31);

 -- state register
 process(rst, clk)
 begin
  if rst = '1' then
   dma_state <= idle;
  elsif rising_edge(clk) then
   dma_state <= dma_next_state;
  end if;
 end process;

 -- next state logic
 process(dma_state, ctl_data)
  variable l: line;
 begin
  case dma_state is
   when idle =>
    write(l, String'("idle"));
    writeline(output, l);
    if dma_start = '1' then
     dma_next_state <= write;
    end if;
   when write =>
    write(l, String'("write"));
    writeline(output, l);
    dma_next_state <= write;
    if dma_off = dma_size then
     dma_next_state <= done;
    end if;
   when done =>
    write(l, String'("done"));
    writeline(output, l);
    dma_next_state <= idle;
   when others =>
    write(l, String'("others"));
    writeline(output, l);
    dma_next_state <= idle;
  end case;
 end process;

 -- moore output logic
 process(dma_state)
 begin
  mwr_en <= '0';
  msi_en <= '0';
  sta_set_en <= '0';
  ctl_clr_en <= '0';

  case dma_state is
   when idle =>
    dma_off <= (others => '0');
    dma_size <= unsigned(sta_data(15 downto 0));
    dma_addr <= unsigned(adh_data(63 downto 32) & adl_data(31 downto 0));
   when write =>
    sta_set_data(31 downto 0) <= x"8000" & std_ulogic_vector(dma_off);
    sta_set_en <= '1';
    ctl_clr_en <= '1';
    mwr_en <= '1';
    mwr_addr <= std_ulogic_vector(dma_addr);

    -- TODO
    mwr_data <= (others => '0');
    mwr_size <= std_ulogic_vector(DMA_BLOCK_SIZE);
    -- TODO

    -- update dma addr and offset
    dma_off <= dma_off + DMA_BLOCK_SIZE;
    dma_addr <= dma_addr + DMA_BLOCK_SIZE;

   when done =>
    sta_set_data(31 downto 0) <= sta_set_data(31 downto 0) and x"7fffffff";
    sta_set_en <= '1';

    -- TODO: look for msi_en in ctl register
    msi_en <= '1';
    -- TODO: look for msi_en in ctl register

   when others =>
  end case;
 end process;

 -- registers instanciation

 dma_reg_ctl: entity work.reg_wo
 generic map
 (
  GENERIC_BAR => 0,
  GENERIC_ADDR => 16#00#
 )
 port map
 (
  rst => rst,
  clk => clk,
  clr_en => ctl_clr_en,
  req_en => req_en,
  req_wr => req_wr,
  req_bar => req_bar,
  req_addr => req_addr,
  req_data => req_data,
  reg_data => ctl_data
 );

 dma_reg_sta: entity work.reg_ro
 generic map
 (
  GENERIC_BAR => 0,
  GENERIC_ADDR => 16#04#
 )
 port map
 (
  rst => rst,
  clk => clk,
  set_en => sta_set_en,
  set_data => sta_set_data,
  req_en => req_en,
  req_wr => req_wr,
  req_bar => req_bar,
  req_addr => req_addr,
  rep_en => rep_en,
  rep_data => rep_data,
  reg_data => sta_data
 );

 dma_reg_adl: entity work.reg_wo
 generic map
 (
  GENERIC_BAR => 0,
  GENERIC_ADDR => 16#08#
 )
 port map
 (
  rst => rst,
  clk => clk,
  clr_en => '0',
  req_en => req_en,
  req_wr => req_wr,
  req_bar => req_bar,
  req_addr => req_addr,
  req_data => req_data,
  reg_data => adl_data
 );

 dma_reg_adh: entity work.reg_wo
 generic map
 (
  GENERIC_BAR => 0,
  GENERIC_ADDR => 16#0c#
 )
 port map
 (
  rst => rst,
  clk => clk,
  clr_en => '0',
  req_en => req_en,
  req_wr => req_wr,
  req_bar => req_bar,
  req_addr => req_addr,
  req_data => req_data,
  reg_data => adh_data
 );

 dma_reg_baz: entity work.reg_wo
 generic map
 (
  GENERIC_BAR => 0,
  GENERIC_ADDR => 16#10#
 )
 port map
 (
  rst => rst,
  clk => clk,
  clr_en => '0',
  req_en => req_en,
  req_wr => req_wr,
  req_bar => req_bar,
  req_addr => req_addr,
  req_data => req_data,
  reg_data => baz_data
 );


end rtl;
