// Grass performance measurement: where does ground-effect time go? Times the client's per-layer
// grass builds (baking placed plants into vertex buffers when chunks stream in), the grass pass
// (from its per-frame setup to its last layer draw), draw-call submission, and the frame itself.
// Keeps ~5 seconds of frames so spikes can be matched against their breakdown.
#pragma once

#include <cstdint>

namespace wxl_livingazeroth::grassperf
{
    struct Frame
    {
        double   frameMs = 0;       // Present to Present
        double   passMs = 0;        // grass pass: per-frame setup .. last layer draw
        double   buildMs = 0;       // total layer builds this frame
        double   worstBuildMs = 0;  // slowest single layer build
        unsigned builds = 0;        // layer builds (slot vertex-buffer fills)
        unsigned plantsBuilt = 0;   // placed instances baked
        unsigned verticesBuilt = 0; // vertices written
        unsigned draws = 0;         // layer draw calls
        double   drawSubmitMs = 0;  // CPU time inside the layer draw calls
        unsigned chunks = 0;        // grass chunks set up
        // Instanced renderer (counted inside the layer draws above).
        unsigned instSlots = 0;     // layer draws done instanced
        unsigned instDrawCalls = 0; // device draw calls they issued (one per doodad model)
        unsigned instPlants = 0;    // plants drawn instanced
        unsigned instBuilds = 0;    // slot instance buffers built
        double   instBuildMs = 0;
        double   instBuildDeviceMs = 0; // of which creating + filling D3D buffers
    };

    // --- hooks call these (main thread) ---
    void OnPassBegin();                                   // per-frame grass setup
    void OnChunk();                                       // per grass chunk
    double Now();                                         // high-resolution milliseconds
    void OnBuild(double ms, unsigned plants, unsigned vertices);
    void OnDraw(double submitMs);
    void OnInstancedDraw(unsigned drawCalls, unsigned plants);
    void OnInstanceBuild(double ms, double deviceMs);
    void OnFrameEnd();                                    // Present

    // --- panel ---
    struct Summary
    {
        unsigned frames = 0;
        Frame    average;
        Frame    worst;             // the frame with the highest frameMs
        unsigned spikeFrames = 0;   // frames over 2x the median frame time
        double   spikeBuildShare = 0; // of all spike-frame time, the fraction spent building grass
        double   medianFrameMs = 0;
    };
    Summary Summarize();
    void Reset();
}
