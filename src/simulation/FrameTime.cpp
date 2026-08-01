#include "FrameTime.h"
#include "common/Assert.h"
#include <cstdlib>
#include <string.h>

void FrameTime::BeginFrame()
{
	PushSpanInner("Frame time", lastFrameEndAt ? *lastFrameEndAt : Clock::now());
}

void FrameTime::EndFrame()
{
	PopSpan();
	assert(activeSpans.empty());
	lastAveragedSpans.clear();
	std::map<ByteString, double> lastDurationAverages;
	std::vector<AveragedSpan> averagedSpans;
	std::swap(durationAverages, lastDurationAverages);
	for (auto &span : retiredSpans)
	{
		auto currDuration = double(std::chrono::duration_cast<std::chrono::nanoseconds>(span.duration).count());
		auto prevDuration = lastDurationAverages[span.name];
		auto duration = prevDuration + (currDuration - prevDuration) * 0.05;
		durationAverages[span.name] = duration;
		averagedSpans.push_back({ span.level, span.name, duration });
	}
	retiredSpans.clear();
	std::swap(averagedSpans, lastAveragedSpans);
	MaybeDump();
	lastFrameEndAt = Clock::now();
}

void FrameTime::MaybeDump()
{
	if (!dumpChecked)
	{
		// * Resolved once: getenv per frame would show up in the very measurement we are taking.
		dumpChecked = true;
		if (auto *path = std::getenv("TPT_FRAMETIME_CSV"))
		{
			dumpFile = std::fopen(path, "w");
		}
	}
	if (!dumpFile)
	{
		return;
	}
	dumpFrameCounter += 1;
	// * The durations are exponentially smoothed with alpha 0.05 in EndFrame, so the first
	//   frames are still converging on the true cost and the set of spans is not stable yet
	//   (some phases only run on certain ticks). Skip the warm-up before writing the header,
	//   then emit one row per frame so the run can be aggregated afterwards.
	constexpr int warmupFrames = 120;
	if (dumpFrameCounter <= warmupFrames)
	{
		return;
	}
	if (!dumpHeaderWritten)
	{
		dumpHeaderWritten = true;
		std::fprintf(dumpFile, "frame");
		for (auto &span : lastAveragedSpans)
		{
			std::fprintf(dumpFile, ",%s", span.name);
		}
		std::fprintf(dumpFile, "\n");
	}
	std::fprintf(dumpFile, "%d", dumpFrameCounter);
	for (auto &span : lastAveragedSpans)
	{
		// * Nanoseconds to microseconds, matching the unit the debug HUD reports.
		std::fprintf(dumpFile, ",%.2f", span.duration / 1000.0);
	}
	std::fprintf(dumpFile, "\n");
	std::fflush(dumpFile);
}

void FrameTime::PushSpanInner(const char *name, Clock::time_point now)
{
	retiredSpans.push_back({ int(activeSpans.size()), name, {} });
	activeSpans.push_back({ int(retiredSpans.size()) - 1, now });
}

void FrameTime::PushSpan(const char *name)
{
	assert(!activeSpans.empty());
	PushSpanInner(name, Clock::now());
}

void FrameTime::PopSpan()
{
	assert(!activeSpans.empty());
	auto now = Clock::now();
	retiredSpans[activeSpans.back().retiredIndex].duration = now - activeSpans.back().begin;
	activeSpans.pop_back();
}
