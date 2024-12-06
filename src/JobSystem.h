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

class JobSystem {
public:
	// entry point for each job
	using EntryPoint = void(void* param);

	// jobs' priority
	enum class Priority {
		LOW = 0, NORMAL = 1, HIGH = 2
	};

	class Counter;

	// declaration of each job
	struct Declaration {
		EntryPoint*			m_pEntryPoint = nullptr;
		void*				m_param = nullptr;
		Priority			m_priority = Priority::LOW;
		JobSystem::Counter*	m_pCounter = nullptr;
	};


	// kick jobs
	void KickJobs(int count, const Declaration aDecl[]);
	void KickJob(const Declaration& decl) {
		KickJobs(1, &decl);
	}
	
	// wait for counter to become 0
	void WaitForCounter(Counter* pCounter);

	// kick jobs and wait for completion
	void KickJobsAndWait(int count, Declaration aDecl[]);
	void KickJobAndWait(Declaration& decl) {
		KickJobsAndWait(1, &decl);
	}

	// for easy control of initialization and shut down order
	void Initialize(U32 numberOfThreads);
	void JoinAndTerminate();

private:
	std::optional<Declaration> PullJob();
	void AddPreviousFiberToPool(); // this is meant to be used only in waiting functions: KickJob(s)AndWait, WaitforCounter
	void WaitForCounterFromFiber(Counter* pCounter);
	void KickJobWithoutNotifingWorkers(const Declaration& decl);
	void NotifyOneWorker();
	void NotifyAllWorkers();
	static bool IsThisThreadAFiber();

	friend void WorkerMainLoop(void*); // PullJob
	friend void JobWrapper(JobSystem::Declaration declaration, JobSystem& rJobSystem); // m_waitList, m_waitListLock

	RingBuffer<Declaration, g_sJobQueue+1>*				m_pJobQueueLow = nullptr;
	RingBuffer<Declaration, g_sJobQueue+1>*				m_pJobQueueNormal = nullptr;
	RingBuffer<Declaration, g_sJobQueue+1>*				m_pJobQueueHigh = nullptr;
	RingBuffer<PFiber, g_sFiberPool+1>*					m_pFiberPool = nullptr;
	std::vector<std::thread>							m_workers;
	std::unordered_map<Counter*, class StatefullFiber*>	m_waitList;
	alignas(64) SpinLock								m_waitListLock;
	alignas(64) std::atomic<bool>						m_keepWorking = true;
	std::mutex											m_workersMainLoopMutex;
	std::condition_variable								m_workersMainLoopConditionVariable;


public:
	// counter used for synchronizng jobs
	class Counter {
		std::atomic<I32>		m_counter;

		// whether fiber->thread notify is needed after m_counter reaches 0
		bool					m_signalAfterCompletion = false;

		// used only if needed for fiber->thread communication
		std::mutex				m_mutex;
		std::condition_variable m_condVar;

		I32 GetCounter() const { return m_counter; }
		void Wait();

		friend void JobWrapper(JobSystem::Declaration declaration, JobSystem& rJobSystem); // GetCounter, NotifyIfNeeded 
		friend void JobSystem::KickJobWithoutNotifingWorkers(const Declaration& decl); // assert SignalAfterCompetion
		friend void JobSystem::WaitForCounter(Counter* pCounter); // Wait()
		friend void JobSystem::WaitForCounterFromFiber(Counter* pCounter); // GetCounter
	public:
		explicit Counter(I32 counter) : m_counter(counter), m_signalAfterCompletion(!JobSystem::IsThisThreadAFiber()) {}
	};
};
