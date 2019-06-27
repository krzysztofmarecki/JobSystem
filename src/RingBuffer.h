#pragma once
#include <atomic>		// std::atomic<bool>
#include <optional>		// std::optional<>
#include <mutex>		// std::lock_guard
#include "SpinLock.h"	// SpinLock

template <class T, size_t capacity>
class RingBuffer {
public:

	void PushBack(T data) {
		std::lock_guard<SpinLock> guard(m_writerSpinLock);
		size_t next = (m_head + 1) % capacity;
		assert(next != m_tail); // if already full, this will fail
		m_queue[m_head] = data;
		m_head = next;
	}

	std::optional<T> PopFront() {
		if (m_head != m_tail) {
			std::lock_guard<SpinLock> guard(m_readerSpinLock);
			if (m_head != m_tail) {
				T data = m_queue[m_tail];
				m_tail = (m_tail + 1) % capacity;
				return data;
			}
		}
		return std::nullopt;
	}
	// -1, because if m_head == m_tail, the RingBuffer is empty,
	// if m_head == (m_tail - 1), the RingBuffer is full
	constexpr size_t Size() { return capacity - 1; }

private:
	T& operator[](size_t i) { return m_queue[i]; }
	friend class JobSystem;
	T								m_queue[capacity];
	alignas(64) SpinLock			m_writerSpinLock;
	alignas(64) SpinLock			m_readerSpinLock;
	// atomics are needed, so updates for m_head and m_tail won't
	// get rearanged and we can easly have two seperate locks for
	// readers and writers
	alignas(64)	std::atomic<size_t>	m_head = 0;
	alignas(64) std::atomic<size_t>	m_tail = 0;
};