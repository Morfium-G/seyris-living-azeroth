#include "GrassPerf.hpp"

#include <windows.h>

#include <algorithm>
#include <vector>

namespace wxl_livingazeroth::grassperf
{
    namespace
    {
        constexpr size_t kHistory = 300; // ~5 s at 60 fps

        Frame  g_current;
        double g_passBegin = -1, g_lastDrawEnd = -1, g_lastFrameEnd = -1;
        std::vector<Frame> g_history;
        size_t g_next = 0;
    }

    double Now()
    {
        static const double msPerTick = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            return 1000.0 / static_cast<double>(f.QuadPart);
        }();
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) * msPerTick;
    }

    void OnPassBegin() { g_passBegin = Now(); g_lastDrawEnd = -1; }
    void OnChunk()     { ++g_current.chunks; }

    void OnBuild(double ms, unsigned plants, unsigned vertices)
    {
        ++g_current.builds;
        g_current.buildMs += ms;
        g_current.worstBuildMs = std::max(g_current.worstBuildMs, ms);
        g_current.plantsBuilt += plants;
        g_current.verticesBuilt += vertices;
    }

    void OnDraw(double submitMs)
    {
        ++g_current.draws;
        g_current.drawSubmitMs += submitMs;
        g_lastDrawEnd = Now();
    }

    void OnInstancedDraw(unsigned drawCalls, unsigned plants)
    {
        ++g_current.instSlots;
        g_current.instDrawCalls += drawCalls;
        g_current.instPlants += plants;
    }

    void OnInstanceBuild(double ms, double deviceMs)
    {
        ++g_current.instBuilds;
        g_current.instBuildMs += ms;
        g_current.instBuildDeviceMs += deviceMs;
    }

    void OnFrameEnd()
    {
        const double now = Now();
        if (g_lastFrameEnd >= 0) g_current.frameMs = now - g_lastFrameEnd;
        g_lastFrameEnd = now;
        if (g_passBegin >= 0 && g_lastDrawEnd >= g_passBegin) g_current.passMs = g_lastDrawEnd - g_passBegin;

        if (g_current.frameMs > 0)
        {
            if (g_history.size() < kHistory) g_history.push_back(g_current);
            else g_history[g_next] = g_current;
            g_next = (g_next + 1) % kHistory;
        }
        g_current = Frame{};
        g_passBegin = -1;
    }

    Summary Summarize()
    {
        Summary s;
        s.frames = static_cast<unsigned>(g_history.size());
        if (g_history.empty()) return s;

        std::vector<double> times;
        times.reserve(g_history.size());
        for (const Frame& f : g_history)
        {
            times.push_back(f.frameMs);
            s.average.frameMs += f.frameMs;      s.average.passMs += f.passMs;
            s.average.buildMs += f.buildMs;      s.average.builds += f.builds;
            s.average.plantsBuilt += f.plantsBuilt; s.average.verticesBuilt += f.verticesBuilt;
            s.average.draws += f.draws;          s.average.drawSubmitMs += f.drawSubmitMs;
            s.average.chunks += f.chunks;
            s.average.instSlots += f.instSlots;  s.average.instDrawCalls += f.instDrawCalls;
            s.average.instPlants += f.instPlants; s.average.instBuilds += f.instBuilds;
            s.average.instBuildMs += f.instBuildMs; s.average.instBuildDeviceMs += f.instBuildDeviceMs;
            if (f.frameMs > s.worst.frameMs) s.worst = f;
        }
        const double n = static_cast<double>(g_history.size());
        s.average.frameMs /= n; s.average.passMs /= n; s.average.buildMs /= n; s.average.drawSubmitMs /= n;
        s.average.instBuildMs /= n; s.average.instBuildDeviceMs /= n;
        s.average.instSlots = static_cast<unsigned>(s.average.instSlots / n + 0.5);
        s.average.instDrawCalls = static_cast<unsigned>(s.average.instDrawCalls / n + 0.5);
        s.average.instPlants = static_cast<unsigned>(s.average.instPlants / n + 0.5);
        s.average.instBuilds = static_cast<unsigned>(s.average.instBuilds / n + 0.5);
        s.average.builds = static_cast<unsigned>(s.average.builds / n + 0.5);
        s.average.plantsBuilt = static_cast<unsigned>(s.average.plantsBuilt / n + 0.5);
        s.average.verticesBuilt = static_cast<unsigned>(s.average.verticesBuilt / n + 0.5);
        s.average.draws = static_cast<unsigned>(s.average.draws / n + 0.5);
        s.average.chunks = static_cast<unsigned>(s.average.chunks / n + 0.5);

        std::nth_element(times.begin(), times.begin() + times.size() / 2, times.end());
        s.medianFrameMs = times[times.size() / 2];

        double spikeTime = 0, spikeBuild = 0;
        for (const Frame& f : g_history)
            if (f.frameMs > 2.0 * s.medianFrameMs)
            {
                ++s.spikeFrames;
                spikeTime += f.frameMs;
                spikeBuild += f.buildMs;
            }
        s.spikeBuildShare = spikeTime > 0 ? spikeBuild / spikeTime : 0;
        return s;
    }

    void Reset()
    {
        g_history.clear();
        g_next = 0;
        g_current = Frame{};
    }
}
