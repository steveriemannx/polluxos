2 brandX ! 1 brandY ! \ Initialize brand placement defaults

: brand+ ( x y c-addr/u -- x y' )
  2swap 2dup at-xy 2swap \ position the cursor
  type \ print to the screen
  1+ \ increase y for next time we're called
;

: brand ( x y -- ) \ "PolluxOS" brand in B/W (11 rows x 48 columns)

  s"   ____            _       _" brand+
  s"  |  _ \          | |     | |" brand+
  s"  | |_) |   ___   | |     | |      _   _  __  __" brand+
  s"  |  __/   / _ \  | |     | |     | | | | \ \/ /" brand+
  s"  |_|     | (_) | |_|     |_|     | |_| |  >  <" brand+
  s"           \___/                  \__,_| /_/\_\" brand+
  s"                   ___    ____" brand+
  s"                  / _ \  / ___|" brand+
  s"                 | | | | \___ \" brand+
  s"                 | |_| |  ___) |" brand+
  s"                  \___/  |____/" brand+

  2drop
;
