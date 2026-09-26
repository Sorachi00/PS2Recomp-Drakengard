#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace GSPerf
{
    using Clock = std::chrono::steady_clock;
    // Completed frame submissions from Drakengard's frame routine, not GS
    // register writes, VBlanks, draw calls or changes in pixel contents.
    inline std::atomic<uint64_t> gameFrames{0};

    inline void recordDrakengardFrameSubmission(uint32_t returnPc, int32_t result)
    {
        // SLES_523.22: FUN_0028be98 ends its frame with sceDmaSend at
        // 0x28c0a0 (return 0x28c0a8), after drawing and updating the display.
        // Count only an accepted submission, once even if the EE yielded.
        if (returnPc == 0x0028c0a8u && result == 0)
            gameFrames.fetch_add(1, std::memory_order_relaxed);
    }

    inline double framesPerSecond(uint64_t frames, Clock::duration elapsed)
    {
        return frames / std::chrono::duration<double>(elapsed).count();
    }

}
