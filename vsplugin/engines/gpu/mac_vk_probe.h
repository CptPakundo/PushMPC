// SPDX-License-Identifier: GPL-3.0-or-later
// macOS: may Vulkan (MoltenVK) be used in this process? MoltenVK's vkCreateInstance can abort the whole process inside
// Metal (the macOS Intel runner's virtual GPU: abort in -[_MTLDevice newArgumentEncoderWithArguments:] from
// MVKPhysicalDevice::initMetalFeatures, mpv-native run 45953906677), which no caller can catch. So before the first
// instance, vk_context.cpp MakeInstance runs pushmpc-vulkan-probe (mac_vk_probe.c, next to the module that holds this
// code; PUSHMPC_VK_PROBE=<path> to name it, =0 to skip) with the library it loaded: the same instance, each device
// and one submit, in a process of its own. The verdict:
//   the probe printed PMVKPROBE-OK            Vulkan may run here (a clean Vulkan error then comes up in-process)
//   it ended without it (a crash, an abort)   no Vulkan in this process (remembered for the process): the caller
//                                             gets the reason and the engine falls back (portable-cpu)
//   no answer within 20 s                     no Vulkan this time, not remembered (a loaded machine is no verdict)
//   no probe next to the module               as before: Vulkan in-process, one log line says the probe is missing
#pragma once
#if defined(__APPLE__)
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern char** environ;

namespace pmvs::gpu {

namespace macprobe_detail {
inline std::string HelperPath()
{
    if (const char* e = std::getenv("PUSHMPC_VK_PROBE")) if (*e && std::strcmp(e, "1") != 0) return e;
    Dl_info di;
    if (!dladdr(reinterpret_cast<void*>(&HelperPath), &di) || !di.dli_fname) return {};
    std::string dir = di.dli_fname;
    return dir.substr(0, dir.rfind('/') + 1) + "pushmpc-vulkan-probe";
}
inline std::string LastLine(const std::string& out)
{
    std::string s = out;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    const size_t p = s.rfind('\n');
    return p == std::string::npos ? s : s.substr(p + 1);
}
} // namespace macprobe_detail

// true: Vulkan may be used in this process. false: why says what the probe saw. note: one line for the log (also
// when true: a missing probe), empty when there is nothing to say.
inline bool MacVulkanProbe(const std::string& lib, std::string& why, std::string& note)
{
    static std::mutex m;
    static int verdict = 0;   // 0 not known, 1 may run, -1 must not run (the probe died)
    static std::string reason;
    std::lock_guard<std::mutex> lk(m);
    note.clear();
    if (verdict == 1) return true;
    if (verdict == -1) { why = reason; return false; }
    const char* off = std::getenv("PUSHMPC_VK_PROBE");
    if (off && std::strcmp(off, "0") == 0) { verdict = 1; return true; }
    const std::string helper = macprobe_detail::HelperPath();
    struct stat st;
    // an archive or a copy may have dropped the exec bit (Kodi's zip install, an upload artifact): put it back
    if (!helper.empty() && stat(helper.c_str(), &st) == 0 && !(st.st_mode & S_IXUSR)) chmod(helper.c_str(), st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
    if (helper.empty() || stat(helper.c_str(), &st) != 0 || !(st.st_mode & S_IXUSR)) {
        note = "Vulkan probe: no pushmpc-vulkan-probe next to the engine (" + helper + "): Vulkan opens in-process unchecked";
        verdict = 1;
        return true;
    }
    int fd[2];
    if (pipe(fd) != 0) { note = "Vulkan probe: pipe failed: Vulkan opens in-process unchecked"; verdict = 1; return true; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    posix_spawnattr_t sa;
    posix_spawnattr_init(&sa);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&sa, POSIX_SPAWN_CLOEXEC_DEFAULT);   // only stdout / stderr reach the probe
#endif
    pid_t pid = 0;
    char* argv[] = { const_cast<char*>(helper.c_str()), const_cast<char*>(lib.c_str()), nullptr };
    const int rc = posix_spawn(&pid, helper.c_str(), &fa, &sa, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&sa);
    close(fd[1]);
    if (rc != 0) {
        close(fd[0]);
        note = std::string("Vulkan probe: cannot start ") + helper + " (" + std::strerror(rc) + "): Vulkan opens in-process unchecked";
        verdict = 1;
        return true;
    }
    // the verdict from the pipe, not from waitpid (a host may reap or ignore its children)
    std::string out;
    bool eof = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!eof) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd p{ fd[0], POLLIN, 0 };
        const int pr = poll(&p, 1, int(left));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) break;
        char b[512];
        const ssize_t n = read(fd[0], b, sizeof(b));
        if (n > 0) out.append(b, size_t(n));
        else if (n == 0 || errno != EINTR) eof = true;
        if (out.size() > 65536) out.erase(0, out.size() - 65536);
    }
    close(fd[0]);
    const bool ok = out.find("PMVKPROBE-OK") != std::string::npos;
    int status = 0;
    pid_t w = 0;
    if (!eof) { kill(pid, SIGKILL); w = waitpid(pid, &status, 0); }
    else for (int i = 0; i < 50 && (w = waitpid(pid, &status, WNOHANG)) == 0; ++i) usleep(10000);
    if (ok) { verdict = 1; return true; }
    if (!eof) {   // a hang: no verdict to keep
        why = "the Vulkan probe gave no answer in 20 s (" + macprobe_detail::LastLine(out) + ")";
        return false;
    }
    char how[64] = "it ended";
    if (w == pid && WIFSIGNALED(status)) std::snprintf(how, sizeof(how), "signal %d", WTERMSIG(status));
    else if (w == pid && WIFEXITED(status)) std::snprintf(how, sizeof(how), "exit %d", WEXITSTATUS(status));
    reason = std::string("Vulkan crashed in its probe (") + how + ", last step: " + macprobe_detail::LastLine(out) + "): MoltenVK cannot run on this Mac's GPU";
    verdict = -1;
    why = reason;
    return false;
}

} // namespace pmvs::gpu
#endif
