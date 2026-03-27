#include "videostats.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <QtGlobal>

void VideoStatsText::addVideoStats(VIDEO_STATS& src, VIDEO_STATS& dst)
{
    dst.receivedFrames += src.receivedFrames;
    dst.decodedFrames += src.decodedFrames;
    dst.renderedFrames += src.renderedFrames;
    dst.totalFrames += src.totalFrames;
    dst.networkDroppedFrames += src.networkDroppedFrames;
    dst.pacerDroppedFrames += src.pacerDroppedFrames;
    dst.totalReassemblyTimeUs += src.totalReassemblyTimeUs;
    dst.totalDecodeTimeUs += src.totalDecodeTimeUs;
    dst.totalPacerTimeUs += src.totalPacerTimeUs;
    dst.totalRenderTimeUs += src.totalRenderTimeUs;

    if (dst.minHostProcessingLatency == 0) {
        dst.minHostProcessingLatency = src.minHostProcessingLatency;
    }
    else if (src.minHostProcessingLatency != 0) {
        dst.minHostProcessingLatency = qMin(dst.minHostProcessingLatency, src.minHostProcessingLatency);
    }
    dst.maxHostProcessingLatency = qMax(dst.maxHostProcessingLatency, src.maxHostProcessingLatency);
    dst.totalHostProcessingLatency += src.totalHostProcessingLatency;
    dst.framesWithHostProcessingLatency += src.framesWithHostProcessingLatency;

    if (!LiGetEstimatedRttInfo(&dst.lastRtt, &dst.lastRttVariance)) {
        dst.lastRtt = 0;
        dst.lastRttVariance = 0;
    }
    else {
        SDL_assert(dst.lastRtt > 0);
    }

    if (!dst.measurementStartUs) {
        dst.measurementStartUs = src.measurementStartUs;
    }

    SDL_assert(dst.measurementStartUs <= src.measurementStartUs);

    double timeDiffSecs = (double)(LiGetMicroseconds() - dst.measurementStartUs) / 1000000.0;
    dst.totalFps = (double)dst.totalFrames / timeDiffSecs;
    dst.receivedFps = (double)dst.receivedFrames / timeDiffSecs;
    dst.decodedFps = (double)dst.decodedFrames / timeDiffSecs;
    dst.renderedFps = (double)dst.renderedFrames / timeDiffSecs;
}

void VideoStatsText::stringifyVideoStats(int videoFormat,
                                         int width,
                                         int height,
                                         VIDEO_STATS& stats,
                                         char* output,
                                         int length)
{
    int offset = 0;
    const char* codecString;
    int ret;

    output[offset] = 0;

    switch (videoFormat)
    {
    case VIDEO_FORMAT_H264:
        codecString = "H.264";
        break;

    case VIDEO_FORMAT_H264_HIGH8_444:
        codecString = "H.264 4:4:4";
        break;

    case VIDEO_FORMAT_H265:
        codecString = "HEVC";
        break;

    case VIDEO_FORMAT_H265_REXT8_444:
        codecString = "HEVC 4:4:4";
        break;

    case VIDEO_FORMAT_H265_MAIN10:
        codecString = LiGetCurrentHostDisplayHdrMode() ? "HEVC 10-bit HDR" : "HEVC 10-bit SDR";
        break;

    case VIDEO_FORMAT_H265_REXT10_444:
        codecString = LiGetCurrentHostDisplayHdrMode() ? "HEVC 10-bit HDR 4:4:4" : "HEVC 10-bit SDR 4:4:4";
        break;

    case VIDEO_FORMAT_AV1_MAIN8:
        codecString = "AV1";
        break;

    case VIDEO_FORMAT_AV1_HIGH8_444:
        codecString = "AV1 4:4:4";
        break;

    case VIDEO_FORMAT_AV1_MAIN10:
        codecString = LiGetCurrentHostDisplayHdrMode() ? "AV1 10-bit HDR" : "AV1 10-bit SDR";
        break;

    case VIDEO_FORMAT_AV1_HIGH10_444:
        codecString = LiGetCurrentHostDisplayHdrMode() ? "AV1 10-bit HDR 4:4:4" : "AV1 10-bit SDR 4:4:4";
        break;

    default:
        SDL_assert(false);
        codecString = "UNKNOWN";
        break;
    }

    if (stats.receivedFps > 0) {
        ret = snprintf(&output[offset],
                       length - offset,
                       "Video stream: %dx%d %.2f FPS (Codec: %s)\n"
                       "Incoming frame rate from network: %.2f FPS\n"
                       "Decoding frame rate: %.2f FPS\n"
                       "Rendering frame rate: %.2f FPS\n",
                       width,
                       height,
                       stats.totalFps,
                       codecString,
                       stats.receivedFps,
                       stats.decodedFps,
                       stats.renderedFps);
        if (ret < 0 || ret >= length - offset) {
            SDL_assert(false);
            return;
        }

        offset += ret;
    }

    if (stats.framesWithHostProcessingLatency > 0) {
        ret = snprintf(&output[offset],
                       length - offset,
                       "Host processing latency min/max/average: %.1f/%.1f/%.1f ms\n",
                       (float)stats.minHostProcessingLatency / 10,
                       (float)stats.maxHostProcessingLatency / 10,
                       (float)stats.totalHostProcessingLatency / 10 / stats.framesWithHostProcessingLatency);
        if (ret < 0 || ret >= length - offset) {
            SDL_assert(false);
            return;
        }

        offset += ret;
    }

    if (stats.renderedFrames != 0) {
        char rttString[32];

        if (stats.lastRtt != 0) {
            snprintf(rttString, sizeof(rttString), "%u ms (variance: %u ms)", stats.lastRtt, stats.lastRttVariance);
        }
        else {
            snprintf(rttString, sizeof(rttString), "N/A");
        }

        ret = snprintf(&output[offset],
                       length - offset,
                       "Frames dropped by your network connection: %.2f%%\n"
                       "Frames dropped due to network jitter: %.2f%%\n"
                       "Average network latency: %s\n"
                       "Average decoding time: %.2f ms\n"
                       "Average frame queue delay: %.2f ms\n"
                       "Average rendering time (including monitor V-sync latency): %.2f ms\n",
                       stats.totalFrames != 0 ? (float)stats.networkDroppedFrames / stats.totalFrames * 100 : 0.0f,
                       stats.decodedFrames != 0 ? (float)stats.pacerDroppedFrames / stats.decodedFrames * 100 : 0.0f,
                       rttString,
                       stats.decodedFrames != 0 ? (double)(stats.totalDecodeTimeUs / 1000.0) / stats.decodedFrames : 0.0,
                       stats.renderedFrames != 0 ? (double)(stats.totalPacerTimeUs / 1000.0) / stats.renderedFrames : 0.0,
                       stats.renderedFrames != 0 ? (double)(stats.totalRenderTimeUs / 1000.0) / stats.renderedFrames : 0.0);
        if (ret < 0 || ret >= length - offset) {
            SDL_assert(false);
        }
    }
}
