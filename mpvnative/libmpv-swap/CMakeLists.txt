# The libmpv forwarder (pushmpc-libmpv-forward.dll, installed into a player's folder as its libmpv-2.dll): MSVC x64,
# kernel32 only, no C runtime. The thunks (MASM) and the .def come from exports.txt at configure time.
#   cmake -S mpvnative/libmpv-swap -B build-fwd -A x64 [-DLIBMPV=<our libmpv-2.dll>]   (LIBMPV: every name must be one
#   of its exports, checked with dumpbin)
cmake_minimum_required(VERSION 3.20)
project(pushmpc_libmpv_forward C ASM_MASM)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/msvc-loop-guard.cmake")   # MSVC loop optimiser bug guard (/d2NoLUR)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/exports.txt lines REGEX "^mpv_")
list(LENGTH lines n)
if(n LESS 40)
    message(FATAL_ERROR "exports.txt lists only ${n} functions")
endif()

if(LIBMPV)
    find_program(DUMPBIN dumpbin)
    if(DUMPBIN)
        execute_process(COMMAND ${DUMPBIN} /nologo /exports ${LIBMPV} OUTPUT_VARIABLE dump)
        foreach(f IN LISTS lines)
            if(NOT dump MATCHES " ${f}\r?\n")
                message(FATAL_ERROR "${LIBMPV} does not export ${f}")
            endif()
        endforeach()
        message(STATUS "all ${n} exports found in ${LIBMPV}")
    endif()
endif()

set(gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(h "/* generated from exports.txt */\n#define PM_EXPORT_COUNT ${n}\nstatic const char *const pm_export_names[PM_EXPORT_COUNT] = {\n")
set(asm "; generated from exports.txt: one thunk per libmpv export (mpv_create is forward.c's own)\nEXTERN pm_slots:QWORD\nEXTERN pm_resolve_all:PROC\n.code\n")
set(def "LIBRARY libmpv-2.dll\nEXPORTS\n")
set(i 0)
foreach(f IN LISTS lines)
    string(APPEND h "    \"${f}\",\n")
    string(APPEND def "    ${f}\n")
    if(f STREQUAL "mpv_create")
        string(APPEND h "#define PM_IDX_CREATE ${i}\n")
    else()
        math(EXPR off "${i} * 8")
        string(APPEND asm "${f} PROC\n    mov rax, QWORD PTR pm_slots+${off}\n    test rax, rax\n    jz lazy_${i}\n    jmp rax\nlazy_${i}:\n    mov eax, ${i}\n    jmp pm_lazy\n${f} ENDP\n")
    endif()
    if(f STREQUAL "mpv_load_config_file")
        string(APPEND h "#define PM_IDX_LOAD_CONFIG ${i}\n")
    endif()
    math(EXPR i "${i} + 1")
endforeach()
string(APPEND h "};\n")
string(APPEND def "    pushmpc_libmpv_status\n")
# the first call: argument registers saved, every slot resolved, the target called with the same arguments
string(APPEND asm [=[
pm_lazy PROC
    push rcx
    push rdx
    push r8
    push r9
    push rax
    sub rsp, 60h
    movdqu XMMWORD PTR [rsp+20h], xmm0
    movdqu XMMWORD PTR [rsp+30h], xmm1
    movdqu XMMWORD PTR [rsp+40h], xmm2
    movdqu XMMWORD PTR [rsp+50h], xmm3
    call pm_resolve_all
    movdqu xmm0, XMMWORD PTR [rsp+20h]
    movdqu xmm1, XMMWORD PTR [rsp+30h]
    movdqu xmm2, XMMWORD PTR [rsp+40h]
    movdqu xmm3, XMMWORD PTR [rsp+50h]
    add rsp, 60h
    pop rax
    pop r9
    pop r8
    pop rdx
    pop rcx
    lea r10, pm_slots
    jmp QWORD PTR [r10+rax*8]
pm_lazy ENDP
END
]=])
file(WRITE ${gen}/thunks.h "${h}")
file(WRITE ${gen}/thunks.asm "${asm}")
file(WRITE ${gen}/forward.def "${def}")

add_library(pushmpc-libmpv-forward SHARED forward.c ${gen}/thunks.asm ${gen}/forward.def)
target_include_directories(pushmpc-libmpv-forward PRIVATE ${gen})
set_target_properties(pushmpc-libmpv-forward PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
target_compile_options(pushmpc-libmpv-forward PRIVATE $<$<COMPILE_LANGUAGE:C>:/GS- /Oi- /W4 /wd4100>)
target_link_options(pushmpc-libmpv-forward PRIVATE /NODEFAULTLIB /ENTRY:DllMain /SAFESEH:NO)
target_link_libraries(pushmpc-libmpv-forward PRIVATE kernel32)
set_target_properties(pushmpc-libmpv-forward PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/out)
