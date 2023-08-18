#pragma once

#ifdef _WIN32

#include <intrin.h>
#include <windows.h>

#else

#include <x86intrin.h>
#include <unistd.h>
#include <string.h>

#endif

#include <cstdint>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <sstream>
#include <cstdio>
#include <array>
#include <cstring>

namespace ipc {

#ifdef _WIN32

// Largely adapted from https://stackoverflow.com/a/17387176
// Inefficient but who cares, everything is already going down if that gets called
static inline std::string winGetLastError(void) {
	//Get the error message ID, if any.
	DWORD errorMessageID = ::GetLastError();
	if (errorMessageID == 0) {
		return std::string(); //No error message has been recorded
	}

	LPSTR messageBuffer = nullptr;

	//Ask Win32 to give us the string version of that message ID.
	//The parameters we pass in, tell Win32 to create the buffer that holds the message for us (because we don't yet know how long the message string will be).
	size_t size = FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL
	);

	//Copy the error message into a std::string.
	std::string message(messageBuffer, size);

	//Free the Win32's string's buffer.
	LocalFree(messageBuffer);
	return message;
}

#endif

static inline std::string getCPUInfo(void)
{
	#ifdef _WIN32

	// From https://stackoverflow.com/a/64422512

        // 4 is essentially hardcoded due to the __cpuid function requirements.
        // NOTE: Results are limited to whatever the sizeof(int) * 4 is...
        std::array<int, 4> integerBuffer = {};
        constexpr size_t sizeofIntegerBuffer = sizeof(int) * integerBuffer.size();

        std::array<char, 64> charBuffer = {};

        // The information you wanna query __cpuid for.
        // https://learn.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex?view=vs-2019
        constexpr std::array<uint32_t, 3> functionIds = {
		// Manufacturer
		//  EX: "Intel(R) Core(TM"
		0x8000'0002,
		// Model
		//  EX: ") i7-8700K CPU @"
		0x8000'0003,
		// Clockspeed
		//  EX: " 3.70GHz"
		0x8000'0004
        };

        std::string cpu;

        for (int id : functionIds)
        {
		// Get the data for the current ID.
		__cpuid(integerBuffer.data(), id);

		// Copy the raw data from the integer buffer into the character buffer
		std::memcpy(charBuffer.data(), integerBuffer.data(), sizeofIntegerBuffer);

		// Copy that data into a std::string
		cpu += std::string(charBuffer.data());
        }

        return cpu;

	#else

	return "Not implemented";

	#endif
}

static inline void setRealtime(void) {
#ifdef _WIN32

	auto res = SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
	if (res == 0) {
		std::stringstream ss;
		ss << "ipc::setRealtime(_WIN32): SetPriorityClass: " << winGetLastError();
		throw std::runtime_error(ss.str());
	}

	auto priority = GetPriorityClass(GetCurrentProcess());
	if (priority == 0) {
		std::stringstream ss;
		ss << "ipc::setRealtime(_WIN32): GetPriorityClass: " << winGetLastError();
		throw std::runtime_error(ss.str());
	}
	if (priority != REALTIME_PRIORITY_CLASS) {
		std::stringstream ss;
		ss << "ipc::setRealtime(_WIN32): " << "System call did succeed but priority is still lower than required (have 0x" << std::hex << priority << "). Try running the program with administrator priviledges to have the realtime mode go through.";
		throw std::runtime_error(ss.str());
	}

	std::printf("ipc::setRealtime: Certified now running in realtime mode!\n");

#else

	std::printf("ipc::setRealtime: Disclaimer: it is advised not to run this in WSL unless WSL itself is running as realtime (which sounds like a crazy & horribly scary idea). Just pick up the Windows binary in this case.\n");

	auto res = nice(-20);
	if (res == -1) {
		std::stringstream ss;
		ss << "ipc::setRealtime(unistd.h): " << strerror(errno);
		throw std::runtime_error(ss.str());
	}

	std::printf("ipc::setRealtime: Nice value for the process is now %d\n", res);

#endif
}

static inline size_t getClockTimestamp(void) {
	return __rdtsc();
}

struct Duration {
	double lengthCycles;
	double lengthSeconds;

	Duration operator/(size_t n) const {
		return Duration{
			.lengthCycles = lengthCycles / static_cast<double>(n),
			.lengthSeconds = lengthSeconds / static_cast<double>(n)
		};
	}

	template <size_t N>
	static Duration average(const Duration (&samples)[N]) {
		Duration res = {
			.lengthCycles = 0.0,
			.lengthSeconds = 0.0
		};

		for (size_t i = 0; i < N; i++) {
			auto cur = samples[i];
			res.lengthCycles += cur.lengthCycles;
			res.lengthSeconds += cur.lengthSeconds;
		}

		return res / N;
	}

	double inferredFrequency(void) const {
		return lengthCycles / lengthSeconds;
	}

	double inferredFrequencyMHz(void) const {
		return inferredFrequency() / 1.0e6;
	}
};

class DurationMeasurer
{
	// How much time does sampling take
	Duration m_overhead;

	// How many dummy samplings to average to calibrate
	// Warning: do not set this too high, as std::this_thread::sleep_for may just skip
	static inline constexpr size_t calibrationIterationCount = 1 << 8;
	static inline constexpr double calibrationLengthSeconds = 4.0;

public:
	DurationMeasurer(void) :
		m_overhead({
			.lengthCycles = 0.0,
			.lengthSeconds = 0.0
		})
	{
	}

	// Only informative, do not use these timings yourself, measure already does the compensation by default
	Duration getOverhead(void) const {
		return m_overhead;
	}

	// Fn is `void (void)`
	template <typename Fn>
	Duration measure(Fn &&fn, bool compensate = true) const {
		auto beginChrono = std::chrono::high_resolution_clock::now();
		volatile auto begin = getClockTimestamp();
		fn();
		volatile auto end = getClockTimestamp();
		auto endChrono = std::chrono::high_resolution_clock::now();

		auto res = Duration{
			.lengthCycles = static_cast<double>(end - begin),
			.lengthSeconds = std::chrono::duration<double>(endChrono - beginChrono).count()
		};
		if (compensate) {
			if (res.lengthCycles > m_overhead.lengthCycles)
				res.lengthCycles -= m_overhead.lengthCycles;
			else
				res.lengthCycles = 0.0;

			if (res.lengthSeconds > m_overhead.lengthSeconds)
				res.lengthSeconds -= m_overhead.lengthSeconds;
			else
				res.lengthSeconds = 0.0;
		}
		return res;
	}

private:
	Duration computeCalibration(void) const {

		std::printf("ipc::DurationMeasurer::computeCalibration: Calibrating, this should take around %g seconds..\n", calibrationLengthSeconds);

		constexpr double lengthPerIteration = calibrationLengthSeconds / static_cast<double>(calibrationIterationCount);


		Duration durations[calibrationIterationCount];

		for (size_t i = 0; i < calibrationIterationCount; i++) {
			std::this_thread::sleep_for(std::chrono::duration<double>(lengthPerIteration));
			// Warmup
			for (size_t i = 0; i < 64; i++) {
				durations[i] = measure([](){}, false);
			}
			// Actual data
			durations[i] = measure([](){}, false);
		}

		std::printf("ipc::DurationMeasurer::computeCalibration: Calibration done.\n");

		return Duration::average(durations);
	}

public:
	void calibrate(void) {
		m_overhead = computeCalibration();
	}
};

}