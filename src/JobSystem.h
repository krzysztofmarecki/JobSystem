#pragma once
#include <cassert>		// assert
#include <optional>		// std::optional
#include <atomic>		// std::atomic
#include <mutex>		// std::lock_guard
#include <vector>		// std::vector
#include <unordered_map>// std::unordered_map

#include <Windows.h>    // ::CreateFiber, ::SwitchToFiber, ::ConvertThreadToFiber
						// HANDLE, DWORD_PTR, SetThreadAffinityMask, LPVOID

#include "types.h"
#include "SpinLock.h"	// SpinLock
#include "RingBuffer.h" // RingBuffer

const size_t g_sKiBStack = 512 * 1024;
const size_t g_sJobQueue = 1024;
const size_t g_sFiberPool = 160;
const size_t g_sWaitList = g_sFiberPool;
using PFiber = LPVOID;
class StatefullFiber;

class JobSystem {
public:
	// entry point for each job
	using EntryPoint = void(void* param);

	// counter used for synchronizng jobs
	using Counter = std::atomic<I32>;

	// jobs' priority
	enum class Priority {
		LOW = 0, NORMAL = 1, HIGH = 2
	};

	// declaration of each job
	struct Declaration {
		EntryPoint* m_pEntryPoint = nullptr;
		void*		m_param		  = nullptr;
		Priority	m_priority	  = Priority::LOW;
		Counter*	m_pCounter    = nullptr;
	};

	// kick jobs
	void KickJob(const Declaration& decl);
	void KickJobs(int count, const Declaration aDecl[]) {
		for (size_t i = 0; i < count; i++)
			KickJob(aDecl[i]);
	}
	
	// wait for counter to become 0
	void WaitForCounter(Counter* pCounter);

	// kick jobs and wait for completion
	void KickJobAndWait(Declaration& decl) {
		KickJobsAndWait(1, &decl);
	}
	void KickJobsAndWait(int count, Declaration aDecl[]);

	// for easy control of initialization and shut down order
	void Initialize(U32 numberOfThreads);
	void Terminate();

	// convenient function, so main thread can politely wait
	void Join() {
		for (std::thread& thread : m_workers)
			thread.join();
	}

private:
	std::optional<Declaration> PullJob();
	void AddPreviousFiberToPool(); // this is meant to be used only in waiting functions: KickJob(s)AndWait, WaitforCounter
	friend void WorkerMainLoop(void*); // PullJob
	friend void JobWrapper(JobSystem::Declaration declaration, JobSystem& rJobSystem); // m_waitList, m_waitListLock
	RingBuffer<Declaration, g_sJobQueue+1>*				m_pJobQueueLow = nullptr;
	RingBuffer<Declaration, g_sJobQueue+1>*				m_pJobQueueNormal = nullptr;
	RingBuffer<Declaration, g_sJobQueue+1>*				m_pJobQueueHigh = nullptr;
	RingBuffer<PFiber, g_sFiberPool+1>*					m_pFiberPool = nullptr;
	std::vector<std::thread>							m_workers;
	std::unordered_map<Counter*, class StatefullFiber*>	m_waitList;
	alignas(64) SpinLock								m_waitListLock;
	std::atomic<bool>									m_keepWorking = true;
};