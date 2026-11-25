# cmake -DIN=<binary> -DOUT=<file.cpp> -P bin2cpp.cmake: the kernels' fatbin as a C++ array (16-byte aligned, as
# cuModuleLoadData wants its image).
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR bytes "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" body "${body}")
file(WRITE "${OUT}" "// generated from nvl_kernels.cu by bin2cpp.cmake\nextern \"C\" {\nalignas(16) extern const unsigned char nvl_kernels_fatbin[] = {\n${body}\n};\nextern const unsigned long nvl_kernels_fatbin_size = ${bytes}ul;\n}\n")
