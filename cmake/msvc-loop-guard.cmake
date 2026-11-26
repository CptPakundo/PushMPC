# Guard against an MSVC x64 optimiser bug (seen in cl 19.44 and 19.52 at /O2 and /Ox): in a counted loop whose body
# starts with "if (i == K) <call or store>;" followed by a comparison of i, the loop unroller can treat iteration K as
# the first one and drop the iterations before it. /d2NoLUR turns that unroller off (measured cost: about -0.02 % of the
# instructions); it is an undocumented switch, so a toolset without it stops the build with error C1007 instead of
# silently going back to the bug. tests/msvc-loop-guard.cpp checks it on every Windows CI build.
# Included by every CMake project of this repository, and passed to PushFPS's build as CMAKE_PROJECT_INCLUDE. Not for
# /GL builds (the switch does not reach link-time code generation; nothing here uses /GL).
add_compile_options($<$<COMPILE_LANG_AND_ID:C,MSVC>:/d2NoLUR> $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/d2NoLUR>)
