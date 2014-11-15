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

#ifndef ASYNCHRONOUS_CLOSE_MONITOR_H_included
#define ASYNCHRONOUS_CLOSE_MONITOR_H_included

#include <map>

#include "ScopedPthreadMutexLock.h"
#include <pthread.h>

#if !defined(__MINGW32__) && !defined(__MINGW64__)
#   define SOCKET int
#else
#   include <Winsock2.h>
#   include <windows.h>
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
class UnlockPair {
private:
	pthread_mutex_t pushMutex;
	bool pushed;
public:
	SOCKET end1, end2;
	UnlockPair();
	~UnlockPair();
	void push();
	void pop();
};
#endif

/**
 * AsynchronousCloseMonitor helps implement Java's asynchronous close semantics.
 *
 * AsynchronousCloseMonitor::init must be called before anything else.
 *
 * Every blocking I/O operation must be surrounded by an AsynchronousCloseMonitor
 * instance. For example:
 *
 *   {
 *     AsynchronousCloseMonitor monitor(fd);
 *     byteCount = ::read(fd, buf, sizeof(buf));
 *   }
 *
 * To interrupt all threads currently blocked on file descriptor 'fd', call signalBlockedThreads:
 *
 *   AsynchronousCloseMonitor::signalBlockedThreads(fd);
 *
 * To test to see if the interruption was due to the signalBlockedThreads call:
 *
 *   monitor.wasSignaled();
 */
class AsynchronousCloseMonitor {
#if defined(__MINGW32__) || defined(__MINGW64__)
private:
	friend class UnlockPair;
	static std::map<DWORD, UnlockPair*> unlockPairs;
#endif
public:
    AsynchronousCloseMonitor(SOCKET fd);
    ~AsynchronousCloseMonitor();
    bool wasSignaled() const;

    static void init();

    static void signalBlockedThreads(SOCKET fd);
private:
    AsynchronousCloseMonitor* mPrev;
    AsynchronousCloseMonitor* mNext;
#if !defined(__MINGW32__) && !defined(__MINGW64__)
    pthread_t mThread;
#else
    DWORD mThreadId;
#endif
    SOCKET mFd;
    bool mSignaled;

    // Disallow copy and assignment.
    AsynchronousCloseMonitor(const AsynchronousCloseMonitor&);
    void operator=(const AsynchronousCloseMonitor&);
};

#endif  // ASYNCHRONOUS_CLOSE_MONITOR_H_included
