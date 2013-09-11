isim force add {/rashpa_top/clk} 1 -value 0 -radix bin -time 500 ns -repeat 1 us
isim force add {/rashpa_top/rst} 0 -time 0 -value 1 -time 1 ns -value 0 -time 10 ns
# isim force add {/tb/ida} 00000010 -time 0 -radix bin
# isim force add {/tb/den} 0 -time 0 -value 1 -time 2 us -value 0 -time 3 us -value 1 -time 4 us
wave add /rashpa_top
run 30 us
