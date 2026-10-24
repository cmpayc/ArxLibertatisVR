/*
 * Copyright 2026 Arx Libertatis VR contributors <kazakov.s.fl@gmail.com>
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/AndroidCrashLog.h"

#include <cstdint>
#include <cstring>
#include <iterator>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

namespace platform {

namespace {

const int CrashSignals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP };
const size_t MaxFrames = 64;

int g_crashLog = -1;
struct sigaction g_previous[std::size(CrashSignals)];
char g_signalStack[64 * 1024];

void writeText(const char * text) {
	ssize_t result = write(g_crashLog, text, std::strlen(text));
	(void)result;
}

void writeHex(uintptr_t value) {
	char buffer[2 + 16 + 1];
	char * end = buffer + sizeof(buffer) - 1;
	char * p = end;
	*p = '\0';
	do {
		*--p = "0123456789abcdef"[value & 0xf];
		value >>= 4;
	} while(value);
	*--p = 'x';
	*--p = '0';
	writeText(p);
}

void writeNumber(int value) {
	char buffer[16];
	char * p = buffer + sizeof(buffer) - 1;
	*p = '\0';
	bool negative = value < 0;
	unsigned magnitude = negative ? unsigned(-value) : unsigned(value);
	do {
		*--p = char('0' + magnitude % 10);
		magnitude /= 10;
	} while(magnitude);
	if(negative) {
		*--p = '-';
	}
	writeText(p);
}

void writeFrame(size_t index, uintptr_t pc) {
	writeText("[CRASH]   #");
	writeNumber(int(index));
	writeText(" ");
	Dl_info info;
	if(dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_fname) {
		const char * name = std::strrchr(info.dli_fname, '/');
		writeText(name ? name + 1 : info.dli_fname);
		writeText(" + ");
		writeHex(pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
		if(info.dli_sname) {
			writeText(" (");
			writeText(info.dli_sname);
			writeText(")");
		}
	} else {
		writeHex(pc);
	}
	writeText("\n");
}

struct Backtrace {
	uintptr_t frames[MaxFrames];
	size_t count;
};

_Unwind_Reason_Code unwindFrame(_Unwind_Context * context, void * arg) {
	Backtrace & backtrace = *static_cast<Backtrace *>(arg);
	uintptr_t pc = _Unwind_GetIP(context);
	if(pc && backtrace.count < MaxFrames) {
		backtrace.frames[backtrace.count++] = pc;
	}
	return backtrace.count < MaxFrames ? _URC_NO_REASON : _URC_END_OF_STACK;
}

const char * signalName(int signal) {
	switch(signal) {
		case SIGSEGV: return "SIGSEGV";
		case SIGBUS: return "SIGBUS";
		case SIGFPE: return "SIGFPE";
		case SIGILL: return "SIGILL";
		case SIGABRT: return "SIGABRT";
		case SIGTRAP: return "SIGTRAP";
		default: return "signal";
	}
}

void crashHandler(int signal, siginfo_t * info, void * context) {

	writeText("[CRASH] ");
	writeText(signalName(signal));
	writeText(" (code ");
	writeNumber(info ? info->si_code : 0);
	writeText(") at address ");
	writeHex(info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0);
	writeText("\n");

	#if defined(__aarch64__)
	if(context) {
		const ucontext_t * ucontext = static_cast<const ucontext_t *>(context);
		writeText("[CRASH] Faulting instruction:\n");
		writeFrame(0, uintptr_t(ucontext->uc_mcontext.pc));
		writeText("[CRASH] Return address (lr):\n");
		writeFrame(0, uintptr_t(ucontext->uc_mcontext.regs[30]));
	}
	#endif

	writeText("[CRASH] Stack:\n");
	Backtrace backtrace;
	backtrace.count = 0;
	_Unwind_Backtrace(unwindFrame, &backtrace);
	for(size_t i = 0; i < backtrace.count; i++) {
		writeFrame(i, backtrace.frames[i]);
	}
	fsync(g_crashLog);

	// Let the previous handler write the system crash report, or terminate
	for(size_t i = 0; i < std::size(CrashSignals); i++) {
		if(CrashSignals[i] == signal) {
			sigaction(signal, &g_previous[i], nullptr);
		}
	}

}

} // anonymous namespace

void installAndroidCrashLog(const fs::path & logFile) {

	g_crashLog = open(logFile.string().c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
	if(g_crashLog < 0) {
		return;
	}

	// Also for stack overflows
	stack_t stack;
	stack.ss_sp = g_signalStack;
	stack.ss_size = sizeof(g_signalStack);
	stack.ss_flags = 0;
	sigaltstack(&stack, nullptr);

	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_sigaction = crashHandler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	for(size_t i = 0; i < std::size(CrashSignals); i++) {
		sigaction(CrashSignals[i], &action, &g_previous[i]);
	}

}

} // namespace platform
