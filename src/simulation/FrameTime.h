#pragma once
#include "common/String.h"
#include <vector>
#include <chrono>
#include <cstdio>
#include <map>
#include <optional>

class FrameTime
{
	using Clock = std::chrono::high_resolution_clock;

	std::optional<Clock::time_point> lastFrameEndAt;
	struct ActiveSpan
	{
		int retiredIndex;
		Clock::time_point begin;
	};
	std::vector<ActiveSpan> activeSpans;
	struct RetiredSpan
	{
		int level;
		const char *name;
		Clock::duration duration;
	};
	std::vector<RetiredSpan> retiredSpans;
	std::map<ByteString, double> durationAverages;
	struct AveragedSpan
	{
		int level;
		const char *name;
		double duration;
	};
	std::vector<AveragedSpan> lastAveragedSpans;

	// * Optional CSV dump of the averaged spans, enabled by setting TPT_FRAMETIME_CSV to a
	//   path. The spans are otherwise only readable from the on-screen debug HUD, which is
	//   no use for batch measurement; profiling a change to the solver needs numbers that
	//   can be diffed between builds. Off unless the variable is set, so normal play is
	//   unaffected.
	std::FILE *dumpFile = nullptr;
	bool dumpChecked = false;
	bool dumpHeaderWritten = false;
	int dumpFrameCounter = 0;
	void MaybeDump();

	void BeginFrame();
	void EndFrame();
	void PushSpanInner(const char *name, Clock::time_point now);
	void PushSpan(const char *name);
	void PopSpan();

public:

	const std::vector<AveragedSpan> &GetLastSpans()
	{
		return lastAveragedSpans;
	}

	class Span
	{
		FrameTime *frameTime;

	public:
		Span(FrameTime *newFrameTime, const char *name) : frameTime(newFrameTime)
		{
			if (frameTime)
			{
				frameTime->PushSpan(name);
			}
		}

		~Span()
		{
			if (frameTime)
			{
				frameTime->PopSpan();
			}
		}

		Span(const Span &) = delete;
		Span &operator =(const Span &) = delete;
	};

	struct Frame
	{
		FrameTime *frameTime;

		Frame(FrameTime *newFrameTime) : frameTime(newFrameTime)
		{
			if (frameTime)
			{
				frameTime->BeginFrame();
			}
		}

		~Frame()
		{
			if (frameTime)
			{
				frameTime->EndFrame();
			}
		}

		Frame(const Frame &) = delete;
		Frame &operator =(const Frame &) = delete;
	};
};
