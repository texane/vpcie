library ieee;
use ieee.std_logic_1164.all;
use work.pcie;

entity main is end main;

architecture rtl of main is
 -- generate options
 constant GENERATE_RST_CLK: boolean := true;
 constant GENERATE_STIMULI: boolean := false;

 -- synchronous logic
 signal rst: std_ulogic := '0';
 signal clk: std_ulogic;

 -- request signals
 signal req_en: std_ulogic;
 signal req_wr: std_ulogic;
 signal req_bar: std_ulogic_vector(pcie.BAR_WIDTH - 1 downto 0);
 signal req_addr: std_ulogic_vector(pcie.ADDR_WIDTH - 1 downto 0);
 signal req_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

 -- reply signals, one per rw register
 signal rep_en: std_ulogic;
 signal rep_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

 -- from dma memory write
 signal mwr_en: std_ulogic;
 signal mwr_data: std_ulogic_vector(pcie.DATA_WIDTH - 1 downto 0);

 -- reg_rw_msi
 signal msi_en: std_ulogic;

begin

 -- pcie endpoint
 pcie_endpoint: pcie.endpoint
 port map
 (
  rst => rst,
  clk => clk,
  req_en => req_en,
  req_wr => req_wr,
  req_bar => req_bar,
  req_addr => req_addr,
  req_data => req_data,
  rep_en => rep_en,
  rep_data => rep_data,
  -- TODO: mwr_en => mwr_en,
  -- TODO: mwr_data => mwr_data,
  msi_en => msi_en
 );

 -- dma module
 dma_entity: entity work.dma
 port map
 (
  rst => rst,
  clk => clk,
  req_en => req_en,
  req_wr => req_wr,
  req_bar => req_bar,
  req_addr => req_addr,
  req_data => req_data,
  rep_en => rep_en,
  rep_data => rep_data,
  mwr_en => mwr_en,
  mwr_data => mwr_data,
  msi_en => msi_en
 );

 -- reset and clock generation
 rst_clk_generate_1: if GENERATE_RST_CLK = true generate
 rst_clk_entity: entity work.rst_clk
 port map
 (
  rst => rst,
  clk => clk
 );
 end generate;

 -- unit testing
 stimul_generate: if GENERATE_STIMULI = true generate
 stimul_entity: entity work.stimul
 port map
 (
  rst => rst,
  clk => clk,
  req_en => req_en,
  req_wr => req_wr,
  req_bar => req_bar,
  req_addr => req_addr,
  req_data => req_data
 );
 end generate;

end rtl;
