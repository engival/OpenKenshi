config_save: config.cpp's save drops a setting equal to its static default (kenshi-k16.2.243.1; PLAYER_GUIDE "Options and the config file").

A `key=value` line for an option whose value equals the option's static compiled-in default is dropped on save (a setting left at its
default follows a future default change); a value set away from the default is saved, a comment, an unknown key (another build's, a
retired one) and a free-form raw key (`window.<layout>.*`) keep their lines. Only static defaults count: no value detected at runtime
is an option default. `kclient --print-defaults` prints every option with its static default and help line as config-file text and
exits (no window, no data, the config file is not read and not written).

run.sh OUT (CXX, BUILD): builds the unit check twice against client/src/config.cpp, once with t.int default 5 and once with 6, and
runs the second on the first's file (an away value outlives a changed default); then runs the built client's --print-defaults against
a config file that sets another value and checks the listing is the static default and the file is untouched. No game data and no
display are needed; OUT holds the logs and the built checks.
