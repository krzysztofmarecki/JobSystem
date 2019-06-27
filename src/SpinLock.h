#pragma once
#include <atomic>		// std::atomic<bool>
#include <emmintrin.h>	// _mm_pause()
class SpinLock {
	std::atomic<bool> m_lock = false;
public:
	void Lock() {
		// https://www.slideshare.net/ssuser052dd11/igc2018-amd-don-woligroski-why-ryzen
		// page 43
		while (true) {
			while (m_lock) {
				_mm_pause();
			}
			// exit only if we change from false, to true
			// i.e. until we aquire the (spin)lock
			if (!m_lock.exchange(true))
				break;
		}
	}
	void Unlock() {
		m_lock.store(false);
	}
	void lock() { return Lock(); }
	void unlock() { return Unlock(); }
	void Aquire() { return Lock(); }
	void Release() { return Unlock(); }
};