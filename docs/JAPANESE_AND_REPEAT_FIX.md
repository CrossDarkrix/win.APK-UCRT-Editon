# Japanese UI and MediaPlayer repeat fix

The language choice now updates the live language flags before persistence and rebuilds the active Settings sheet without immediately overwriting the selection from the registry. Visible library and Settings strings are translated using their exact source keys. MediaPlayer keeps native looping and now also watches looping playback for unexpected end-of-stream; if a backend reaches EOS, it seeks to zero, reapplies looping, and restarts without firing completion.
