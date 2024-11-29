#include "JobSystem.h"

thread_local U32						tl_workerThreadId = -1;
thread_local PFiber						tl_pCurrentFiber = nullptr;
thread_local PFiber						tl_pFiberToBeAddedToPoolAfterSwitch = nullptr;
thread_local StatefullFiber*			tl_pStatefullFiberToBeUnlockedAfterSwitch = nullptr;

// the only point of this class is because counter can get decremented to 0 after waiting fiber is added to wait list, but before that waiting fiber switched to another 
// (pulled from fiber pool), so we need this extra lock that we take in WaitForCounter, and release in WorkerMainLoop after switch is performed, so we can go back to trully awaiting fiber
class StatefullFiber {
public:
	explicit StatefullFiber(LPVOID pFiber) : m_pFiber(pFiber) {}
	PFiber GetRawFiber() { return m_pFiber; }
private:
	friend void WorkerMainLoop(void*);	// release the lock in main loop after switch was performed
	friend void JobWrapper(JobSystem::Declaration declaration, JobSystem& rJobSystem); // GetRawFiber
	friend class JobSystem; // whole class, because I can't make friend with private function from JobSystem, if this class isn't nested in JobSystem
	PFiber m_pFiber;
	SpinLock m_lock;
};




void WorkerMainLoop(void* job_system) {
	JobSystem & jobSystem = *reinterpret_cast<JobSystem*>(job_system);
	while (jobSystem.m_keepWorking) {
		if (tl_pStatefullFiberToBeUnlockedAfterSwitch != nullptr) {
			tl_pStatefullFiberToBeUnlockedAfterSwitch->m_lock.unlock();
			tl_pStatefullFiberToBeUnlockedAfterSwitch = nullptr;
		}

		// pull the job from queue
		std::optional<JobSystem::Declaration> decl = jobSystem.PullJob();
		if (decl) {
			JobWrapper(decl.value(), jobSystem);
		}
		else {
			_mm_pause();
		}
	}
}

void JobWrapper(JobSystem::Declaration declaration, JobSystem& rJobSystem) {

	// execute job
	declaration.m_pEntryPoint(declaration.m_param);

	// shorthand
	JobSystem::Counter* pCounter = declaration.m_pCounter;
	
	// if no associated Counter, we're done
	if (pCounter == nullptr)
		return;

	const bool wasCounterAddedToWaitList = !pCounter->m_signalAfterCompletion;

	I32 newCounterValue;
	{
		if (pCounter->m_signalAfterCompletion) {
			// notify needs to be under same lock as decrement because:
			// 1. multiple workers can finish job at the same, decrement counter and get timesliced before they check if counter reached 0
			// 2. one of them check that it's 0 and notify
			// 3. the guy notified resumes and free Counter's memory
			// 4. later, woken up other worker would checks whether released counter is 0, which could be "true" on garbage memory
			// and then tries to do notify_all() on released conditional variable, that is in fact a garbage memory
			std::lock_guard<std::mutex> lock(pCounter->m_mutex);
			newCounterValue = --(pCounter->m_counter);
			if (newCounterValue == 0)
				pCounter->m_condVar.notify_all();
		}
		else {
			newCounterValue = --(pCounter->m_counter);
		}
	}
	// DEREFERENCING COUNTER PAST THIS LINE IS FORBIDDEN, AS IT MIGHT POINT TO RELEASED MEMORY IN FIBER->THREAD NOTIFY SCENARIO

	if (!wasCounterAddedToWaitList) {
#ifdef _DEBUG
		rJobSystem.m_waitListLock.lock();
		assert(rJobSystem.m_waitList.find(pCounter) == rJobSystem.m_waitList.end());
		rJobSystem.m_waitListLock.unlock();
#endif
		return;
	}

	// decrement counter
	if (newCounterValue == 0) {
		rJobSystem.m_waitListLock.lock();
		auto foundIterator = rJobSystem.m_waitList.find(pCounter);
		if (foundIterator != rJobSystem.m_waitList.end()) {
			// take fiber from wait list
			StatefullFiber* pAwaitingFiber = foundIterator->second;
			assert(pAwaitingFiber->GetRawFiber() != nullptr);
			rJobSystem.m_waitList.erase(pCounter);
			rJobSystem.m_waitListLock.unlock(); // we have to relese it before we try to obtain the lock on fiber in order to avoid deadlock


			// in global lock scenario (naked Kick + WaitForCounter somewhere else), awaiting fiber (added to wait list) can actually could still not switch to
			// another fiber from pool, so we spin until that happen
			pAwaitingFiber->m_lock.lock();
			// and imediately unlock, because pAwaitingFiber is now trully awaiting and it was the only purpose of this lock
			pAwaitingFiber->m_lock.unlock();


			// save current fiber to be added to fiber pool after switch is done
			tl_pFiberToBeAddedToPoolAfterSwitch = tl_pCurrentFiber;
			tl_pCurrentFiber = pAwaitingFiber->GetRawFiber();
			// switch to fiber pulled from wait list
			::SwitchToFiber(pAwaitingFiber->GetRawFiber());

			// We push previous fiber to fiber pool only if we were on wait list and we came back from it.
			// Here, we wasn't, so we are back again only because someone else got pushed to wait list,
			// so we can't add him to pool, so tl_pFiberToBeAddedToPoolAfterSwitch has to be nullptr
			assert(tl_pFiberToBeAddedToPoolAfterSwitch == nullptr);
			assert(tl_pCurrentFiber != nullptr);
		}
		else {
			// This can happen if counter is decremented before JobSystem::WaitForCounter() add fiber to wait list,
			// or after fiber was added to wait list, but also after the WaitForCounter() noticed, that counter is 0 and already removed itself from wait list.
			// This situation is gonna be detecded in fiber that called JobSystem::WaitForCounter(),
			// so here, we just release m_waitListLock
			rJobSystem.m_waitListLock.unlock();
		}
	}
}

void JobSystem::KickJob(const Declaration & decl)
{
	// jobs Kicked from non Fiber has to have m_signalAfterCompletion == true
	// fire and forget jobs don't have counter
#ifdef _DEBUG
	if (!IsThisThreadAFiber() && decl.m_pCounter)
		assert(decl.m_pCounter->m_signalAfterCompletion);
#endif

	if (decl.m_priority == Priority::LOW)
		m_pJobQueueLow->PushBack(decl);
	else if (decl.m_priority == Priority::NORMAL)
		m_pJobQueueNormal->PushBack(decl);
	else if (decl.m_priority == Priority::HIGH)
		m_pJobQueueHigh->PushBack(decl);
	else
		assert(false && "UNHANDLED JOB PRIORITY");
}

bool JobSystem::IsThisThreadAFiber()
{
	return tl_pCurrentFiber != nullptr;
}

std::optional<JobSystem::Declaration> JobSystem::PullJob()
{
	std::optional<JobSystem::Declaration> declaration = m_pJobQueueHigh->PopFront();
	if (!declaration)
		declaration = m_pJobQueueNormal->PopFront();
	if (!declaration)
		declaration = m_pJobQueueLow->PopFront();
	return declaration;
}

void JobSystem::AddPreviousFiberToPool()
{
	// back again, add fiber that we switched from to fiber pool, set it to nullptr afterwards;
	// tl_pFiberToBeAddedToPoolAfterSwitch cannot be null, because we can get here only, because someone pulled us
	// from wait list and then switch to us, so we have to add previous fiber to fiber pool
	assert(tl_pCurrentFiber != nullptr);
	assert(tl_pFiberToBeAddedToPoolAfterSwitch != nullptr);
	m_pFiberPool->PushBack(tl_pFiberToBeAddedToPoolAfterSwitch);
	tl_pFiberToBeAddedToPoolAfterSwitch = nullptr;
}

void JobSystem::WaitForCounter(Counter* pCounter)
{
	// fiber cannot wait on counter created on thread and vice versa
	assert(pCounter->m_signalAfterCompletion == !IsThisThreadAFiber());
	if (IsThisThreadAFiber())
		WaitForCounterFromFiber(pCounter);
	else
		pCounter->Wait();
}

void JobSystem::WaitForCounterFromFiber(Counter * pCounter)
{
	StatefullFiber statefullFiber(tl_pCurrentFiber);
	statefullFiber.m_lock.lock();

	// add itself to wait list
	assert(tl_pCurrentFiber != nullptr);
	m_waitListLock.lock();
	m_waitList[pCounter] = &statefullFiber;
	m_waitListLock.unlock();

	if (pCounter->GetCounter() == 0) {
		std::lock_guard<SpinLock> guard(m_waitListLock);
		// we are here in one of 2 scenarios:
		// 1st - jobs was completed before we added ourselfs to wait list, or jobs were completed after we added ourselfs to wait list, but last job didn't take a m_waitListLock before us,
		// so we just remove ourselves from wait list and continue execution
		// 2nd - jobs were completed after we added ourselfs to wait list and last job took m_waitListLock before us removed us from wait list,
		// and now it's spinning on StatefullFiber::m_lock, so we have to switch to free fiber, so we go to another fiber (and then releasing fiber lock) as fast as possible

		auto foundIterator = m_waitList.find(pCounter);
		
		if (foundIterator != m_waitList.end()) {
			// 1st scenario
			// jobs were already completed, we remove ourselves from wait list and continue execution
			m_waitList.erase(pCounter);
			return;
		}
		// 2nd scenario and counter not equal to 0 has the same logic, thats why it's outside if statement to remove code duplication
	}
	
	// pop free fiber
	std::optional<PFiber> newFiber = m_pFiberPool->PopFront();
	assert(newFiber.has_value());
	tl_pCurrentFiber = newFiber.value();
	// do not add ourselft to tl_pFiberToBeAddedToPoolAfterSwitch, because last job is gonna switch to ass

	// fiber we switch to will unlock the lock on statefullFiber in WorkerMainLoop
	tl_pStatefullFiberToBeUnlockedAfterSwitch = &statefullFiber;

	::SwitchToFiber(newFiber.value());

	AddPreviousFiberToPool();
}


void JobSystem::KickJobsAndWait(int count, Declaration aDecl[])
{
	Counter counter(count);

	for (size_t i = 0; i < count; i++) {
		assert(aDecl[i].m_pCounter == nullptr);
		aDecl[i].m_pCounter = &counter;
	}
	KickJobs(count, aDecl);
	WaitForCounter(&counter);
}

void JobSystem::Initialize(U32 numberOfThreads)
{
	// init job queues
	m_pJobQueueLow	  = new RingBuffer<Declaration, g_sJobQueue+1>;
	m_pJobQueueNormal = new RingBuffer<Declaration, g_sJobQueue+1>;
	m_pJobQueueHigh   = new RingBuffer<Declaration, g_sJobQueue+1>;
	// init fiber pool
	m_pFiberPool = new RingBuffer<PFiber, g_sFiberPool + 1>;
	for (size_t i = 0; i < m_pFiberPool->Size(); i++)
		m_pFiberPool->PushBack( ::CreateFiber(g_sKiBStack, (LPFIBER_START_ROUTINE)WorkerMainLoop, this) );
	// reserve memory for workers and fiberPool
	m_workers.reserve(numberOfThreads);
	m_waitList.reserve(g_sWaitList);
	// init workers
	for (U32 i = 0; i < numberOfThreads; i++) {
		m_workers.emplace_back([this, i] {
			tl_workerThreadId = i;
			tl_pCurrentFiber = ::ConvertThreadToFiber(nullptr);
			// got to main loop
			WorkerMainLoop(this);
		});
		// set affinity
		HANDLE handle = reinterpret_cast<HANDLE>( m_workers[i].native_handle() );
		DWORD_PTR affinityMask = DWORD_PTR(1) << i;
		DWORD_PTR result = SetThreadAffinityMask(handle, affinityMask);
		assert(result != 0);
	}
}

void JobSystem::JoinAndTerminate()
{
	m_keepWorking = false;
	for (std::thread& thread : m_workers) {
		assert(thread.joinable());
		thread.join();
	}

	delete m_pJobQueueLow;
	delete m_pJobQueueNormal;
	delete m_pJobQueueHigh;

	delete m_pFiberPool;
}

void JobSystem::Counter::Wait()
{
	assert(!IsThisThreadAFiber());
	std::unique_lock<std::mutex> lock(m_mutex);
	m_condVar.wait(lock, [this] {return m_counter == 0; });
}
