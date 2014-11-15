/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AsynchronousCloseMonitor"

#include "AsynchronousCloseMonitor.h"
#include "cutils/log.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

/**
 * We use an intrusive doubly-linked list to keep track of blocked threads.
 * This gives us O(1) insertion and removal, and means we don't need to do any allocation.
 * (The objects themselves are stack-allocated.)
 * Waking potentially-blocked threads when a file descriptor is closed is O(n) in the total number
 * of blocked threads (not the number of threads actually blocked on the file descriptor in
 * question). For now at least, this seems like a good compromise for Android.
 */
static pthread_mutex_t blockedThreadListMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t blockedPollMutex = PTHREAD_MUTEX_INITIALIZER;
static AsynchronousCloseMonitor* blockedThreadList = NULL;

std::map<DWORD, UnlockPair*> AsynchronousCloseMonitor::unlockPairs;

/**
 * The specific signal chosen here is arbitrary, but bionic needs to know so that SIGRTMIN
 * starts at a higher value.
 */
#if defined(__APPLE__)
static const int BLOCKED_THREAD_SIGNAL = SIGUSR2;
#elif !defined(__MINGW32__) && !defined(__MINGW64__)
static const int BLOCKED_THREAD_SIGNAL = __SIGRTMIN + 2;
#else
#  include <windows.h>
#endif

#if !defined(__MINGW32__) && !defined(__MINGW64__)
static void blockedThreadSignalHandler(int /*signal*/) {
    // Do nothing. We only sent this signal for its side-effect of interrupting syscalls.
}
#else

VOID CALLBACK closeSocketApcCallback(ULONG_PTR socketParam) {
	closesocket(static_cast<SOCKET>(socketParam));
}
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)

#include "mingw-extensions.h"

UnlockPair::UnlockPair() {
	ScopedPthreadMutexLock pollLock(&blockedPollMutex);
	pushed = false;
	pushMutex = PTHREAD_MUTEX_INITIALIZER;
	
	int pipefd[2];
	pipe(pipefd);
	
	end1 = static_cast<SOCKET>(pipefd[0]);
	end2 = static_cast<SOCKET>(pipefd[1]);
	
	DWORD threadId = GetCurrentThreadId();
	AsynchronousCloseMonitor::unlockPairs.insert(std::pair<DWORD, UnlockPair*>(threadId, this));
}

void UnlockPair::push() {
	ScopedPthreadMutexLock pushLock(&pushMutex);
	if (!pushed) {
		char byteToSend = 123;
		//__mingw_printf("[sending]");
		int ret = send(end1, &byteToSend, 1, 0);
		if (ret != -1) {
			pushed = true;
		} else {
			__mingw_printf("Can't send a byte to the unlocking pair: %d", WSAGetLastError());
		}
	}
}

void UnlockPair::pop() {
	ScopedPthreadMutexLock pushLock(&pushMutex);
	if (pushed) {
		char byteToRecv;
		//__mingw_printf("[receiveing]");
		int ret = recv(end2, &byteToRecv, 1, 0);
		if (ret != -1) {
			pushed = false;
		} else {
			__mingw_printf("Can't receive a byte to the unlocking pair: %d", WSAGetLastError());
		}
	}
}

UnlockPair::~UnlockPair() {
	ScopedPthreadMutexLock pollLock(&blockedPollMutex);
	DWORD threadId = GetCurrentThreadId();
	closesocket(end1);
	closesocket(end2);
	AsynchronousCloseMonitor::unlockPairs.erase(threadId);
}
#endif

void AsynchronousCloseMonitor::init() {
#if !defined(__MINGW32__) && !defined(__MINGW64__)
    // Ensure that the signal we send interrupts system calls but doesn't kill threads.
    // Using sigaction(2) lets us ensure that the SA_RESTART flag is not set.
    // (The whole reason we're sending this signal is to unblock system calls!)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = blockedThreadSignalHandler;
    sa.sa_flags = 0;
    int rc = sigaction(BLOCKED_THREAD_SIGNAL, &sa, NULL);
    if (rc == -1) {
        ALOGE("setting blocked thread signal handler failed: %s", strerror(errno));
    }
#endif
}

void AsynchronousCloseMonitor::signalBlockedThreads(SOCKET fd) {
    ScopedPthreadMutexLock lock(&blockedThreadListMutex);
    for (AsynchronousCloseMonitor* it = blockedThreadList; it != NULL; it = it->mNext) {
        if (it->mFd == fd) {
            it->mSignaled = true;
#if !defined(__MINGW32__) && !defined(__MINGW64__)
            pthread_kill(it->mThread, BLOCKED_THREAD_SIGNAL);
#else
            HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, it->mThreadId);
			{
				ScopedPthreadMutexLock pollLock(&blockedPollMutex);

				if (unlockPairs.find(it->mThreadId) != unlockPairs.end()) {
					UnlockPair& up = *(unlockPairs.at(it->mThreadId));
					up.push();
				}
			}

            QueueUserAPC(closeSocketApcCallback, hThread, static_cast<ULONG_PTR>(it->mFd));
            CloseHandle(hThread);
#endif
            // Keep going, because there may be more than one thread...
        }
    }
}

bool AsynchronousCloseMonitor::wasSignaled() const {
    return mSignaled;
}

AsynchronousCloseMonitor::AsynchronousCloseMonitor(SOCKET fd) {
    ScopedPthreadMutexLock lock(&blockedThreadListMutex);
    // Who are we, and what are we waiting for?
#if !defined(__MINGW32__) && !defined(__MINGW64__)
    mThread = pthread_self();
#else
    mThreadId = GetCurrentThreadId();
#endif
    mFd = fd;
    mSignaled = false;
    // Insert ourselves at the head of the intrusive doubly-linked list...
    mPrev = NULL;
    mNext = blockedThreadList;
    if (mNext != NULL) {
        mNext->mPrev = this;
    }
    blockedThreadList = this;
}

AsynchronousCloseMonitor::~AsynchronousCloseMonitor() {
    ScopedPthreadMutexLock lock(&blockedThreadListMutex);
    // Unlink ourselves from the intrusive doubly-linked list...
    if (mNext != NULL) {
        mNext->mPrev = mPrev;
    }
    if (mPrev == NULL) {
        blockedThreadList = mNext;
    } else {
        mPrev->mNext = mNext;
    }
}
