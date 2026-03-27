#pragma once

#include "decoder.h"

namespace VideoStatsText {

void addVideoStats(VIDEO_STATS& src, VIDEO_STATS& dst);

void stringifyVideoStats(int videoFormat,
                         int width,
                         int height,
                         VIDEO_STATS& stats,
                         char* output,
                         int length);

}
