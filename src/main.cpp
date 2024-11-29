#include <iostream>		// std::cout
#include <string>		// std::string

#include "JobSystem.h"	// JobSystem

JobSystem g_jobSystem;

void TheMostCreativeWayToCalculateFibonacci(void* pNumberVoid) {
	int* pNumber = reinterpret_cast<int*>(pNumberVoid);
	int n = *pNumber;
	if (n > 1) {
		int fibNMinus1 = n-1;
		int fibNMinus2 = n-2;
		JobSystem::Declaration adecl[2];
		adecl[0].m_pEntryPoint = TheMostCreativeWayToCalculateFibonacci;
		adecl[0].m_param = &fibNMinus1;
		adecl[1].m_pEntryPoint = TheMostCreativeWayToCalculateFibonacci;
		adecl[1].m_param = &fibNMinus2;

		g_jobSystem.KickJobsAndWait(2, adecl);
		n = fibNMinus1 + fibNMinus2;
		*pNumber = n;
	}
	std::cout << n << "\n";
}

int main() {
	const int numberOfThreads = std::thread::hardware_concurrency();
	g_jobSystem.Initialize(numberOfThreads);
	int n = 11;
	JobSystem::Declaration decl;
	decl.m_pEntryPoint = TheMostCreativeWayToCalculateFibonacci;
	decl.m_param = &n;
	g_jobSystem.KickJobAndWait(decl);

	g_jobSystem.JoinAndTerminate();
}