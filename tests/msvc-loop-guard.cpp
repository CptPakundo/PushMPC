// Canary for an MSVC x64 optimiser bug (cl 19.44 and 19.52 at /O2 and /Ox): the loop below must call f() 20 times.
// With the bug, the loop unroller treats iteration 45 (the "if (s == 45)" guard) as the first one and iterations 40..44
// are lost: 15 calls. /d2NoLUR (cmake/msvc-loop-guard.cmake, on every MSVC target) avoids it.
//   cl /nologo /O2 /Ob2 /DNDEBUG /EHsc /MT /d2NoLUR msvc-loop-guard.cpp   -> exit 0, "calls 20"
//   the same without /d2NoLUR                                             -> exit 1 on an affected compiler
#include <cstdio>

static int calls = 0;
__declspec(noinline) void once() { std::puts("once"); }
__declspec(noinline) void f(int s, bool first) { (void)s; (void)first; ++calls; }

int main()
{
    for (int s = 40; s < 60; ++s) { if (s == 45) once(); f(s, s == 40); }
    std::printf("calls %d (expected 20)\n", calls);
    return calls == 20 ? 0 : 1;
}
