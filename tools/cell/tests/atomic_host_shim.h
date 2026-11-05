#pragma once
// Build the production Android registry/futex path on a glibc host. The only
// platform shim is bionic's pthread TID accessor; no wait behavior is replaced.
#if defined(ANDROID) && !defined(__ANDROID__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#define pthread_gettid_np(unused) static_cast<unsigned>(::syscall(SYS_gettid))
#endif
