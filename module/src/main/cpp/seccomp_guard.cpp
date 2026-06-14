#include "seccomp_guard.h"
#include "logger.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/ucontext.h>

namespace {
struct sigaction g_previous_sigsys_action{};
bool g_guard_installed = false;

#if defined(__aarch64__)
constexpr long kDuckKsuProbeSyscall = 142;
constexpr unsigned long kDuckProbeArg0 = 0xdeadbeefUL;
constexpr unsigned long kDuckProbeArg1 = 0xcafebabeUL;

bool is_duck_ksu_probe(siginfo_t *info, void *context) {
    if (!info || !context) return false;
    if (info->si_code != SYS_SECCOMP) return false;

    auto *uc = static_cast<ucontext_t *>(context);
    const auto syscall_no = static_cast<long>(uc->uc_mcontext.regs[8]);
    const auto arg0 = static_cast<unsigned long>(uc->uc_mcontext.regs[0]);
    const auto arg1 = static_cast<unsigned long>(uc->uc_mcontext.regs[1]);
    return syscall_no == kDuckKsuProbeSyscall && arg0 == kDuckProbeArg0 && arg1 == kDuckProbeArg1;
}

void skip_blocked_syscall(void *context) {
    auto *uc = static_cast<ucontext_t *>(context);
    uc->uc_mcontext.regs[0] = static_cast<unsigned long>(-ENOSYS);
    uc->uc_mcontext.pc += 4;
}
#endif

void call_previous_handler(int signo, siginfo_t *info, void *context) {
    if (g_previous_sigsys_action.sa_flags & SA_SIGINFO) {
        if (g_previous_sigsys_action.sa_sigaction) {
            g_previous_sigsys_action.sa_sigaction(signo, info, context);
            return;
        }
    } else if (g_previous_sigsys_action.sa_handler == SIG_IGN) {
        return;
    } else if (g_previous_sigsys_action.sa_handler && g_previous_sigsys_action.sa_handler != SIG_DFL) {
        g_previous_sigsys_action.sa_handler(signo);
        return;
    }

    signal(signo, SIG_DFL);
    raise(signo);
}

void sigsys_handler(int signo, siginfo_t *info, void *context) {
#if defined(__aarch64__)
    if (signo == SIGSYS && is_duck_ksu_probe(info, context)) {
        skip_blocked_syscall(context);
        yukari_log_info("suppressed blocked Duck KSU supercall probe");
        return;
    }
#else
    (void)info;
    (void)context;
#endif
    call_previous_handler(signo, info, context);
}
} // namespace

void install_seccomp_probe_guard() {
    if (g_guard_installed) return;

    struct sigaction action{};
    action.sa_sigaction = sigsys_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_NODEFER;

    if (sigaction(SIGSYS, &action, &g_previous_sigsys_action) != 0) {
        yukari_log_error("failed to install SIGSYS guard errno=%d", errno);
        return;
    }

    g_guard_installed = true;
    yukari_log_info("SIGSYS guard installed");
}
