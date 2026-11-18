# cmake -DIN=file.bin -DOUT=file.inc -P bin2inc.cmake: writes the bytes as a comma-separated C initializer list.
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" hex "${hex}")
file(WRITE "${OUT}" "${hex}\n")
