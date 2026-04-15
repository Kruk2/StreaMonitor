#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — N_m3u8DL-RE external recorder
// Shells out to the N_m3u8DL-RE binary for HLS recording.
// Used as an alternative to the native FFmpeg HLS recorder.
// ─────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "config/config.h"
#include <spdlog/spdlog.h>
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <map>
#include <filesystem>

namespace sm
{

    struct NM3U8DLResult
    {
        bool success = false;
        std::string error;
        std::string outputPath;
        uint64_t bytesWritten = 0;
    };

    class NM3U8DLRecorder
    {
    public:
        explicit NM3U8DLRecorder(const AppConfig &config);

        // Check if the N_m3u8DL-RE binary is available
        static bool isAvailable(const std::string &binaryPath);

        // Record an HLS stream to file. Blocks until finished or cancelled.
        NM3U8DLResult record(const std::string &hlsUrl,
                             const std::string &outputPath,
                             CancellationToken &cancel,
                             const std::string &userAgent = "",
                             const std::map<std::string, std::string> &headers = {});

        void setLogger(std::shared_ptr<spdlog::logger> lg) { log_ = std::move(lg); }

    private:
        const AppConfig &config_;
        std::shared_ptr<spdlog::logger> log_ = spdlog::default_logger();

        std::vector<std::string> buildArgs(const std::string &hlsUrl,
                                           const std::string &outputDir,
                                           const std::string &outputName,
                                           const std::string &userAgent,
                                           const std::map<std::string, std::string> &headers) const;

        int runProcess(const std::vector<std::string> &args,
                       CancellationToken &cancel,
                       const std::string &pipeEnv = "");

        void logExternalLine(const std::string &line);
    };

} // namespace sm
