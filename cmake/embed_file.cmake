# Writes a C++ source with a file's bytes as an array: cmake -DINPUT=f -DOUTPUT=out.cpp -DSYMBOL=name -P embed_file.cmake
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR size "${length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT} (cmake/embed_file.cmake)\n#include <cstddef>\nextern const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\nextern const std::size_t ${SYMBOL}Size = ${size};\n")
