// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — N_m3u8DL-RE external recorder implementation
// ─────────────────────────────────────────────────────────────────

#include "downloaders/n_m3u8dl_recorder.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <string>
#include <array>
#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
#define sm_popen _popen
#define sm_pclose _pclose
#else
#define sm_popen popen
#define sm_pclose pclose
#endif

namespace
{
    double probeStartTime(const fs::path &ffprobePath, const fs::path &mediaFile)
    {
        std::string cmd = "\"" + ffprobePath.string() + "\""
                          " -v error"
                          " -show_entries format=start_time"
                          " -of default=noprint_wrappers=1:nokey=1"
                          " \"" + mediaFile.string() + "\"";

        FILE *pipe = sm_popen(cmd.c_str(), "r");
        if (!pipe)
            return 0.0;

        char buf[256];
        std::string output;
        while (fgets(buf, sizeof(buf), pipe))
            output += buf;
        sm_pclose(pipe);

        try
        {
            return std::stod(output);
        }
        catch (...)
        {
            return 0.0;
        }
    }

    fs::path deriveFFprobePath(const fs::path &ffmpegPath)
    {
        std::string stem = ffmpegPath.stem().string();
        std::string ext = ffmpegPath.extension().string();
        auto pos = stem.find("ffmpeg");
        if (pos != std::string::npos)
        {
            stem.replace(pos, 6, "ffprobe");
            return ffmpegPath.parent_path() / (stem + ext);
        }
        return "ffprobe";
    }
} // anonymous namespace

namespace sm
{

    NM3U8DLRecorder::NM3U8DLRecorder(const AppConfig &config)
        : config_(config)
    {
    }

    void NM3U8DLRecorder::logExternalLine(const std::string &line)
    {
        // N_m3u8DL-RE format: "HH:MM:SS.mmm LEVEL: message"
        // or "Unhandled exception: ..."
        if (line.find("ERROR") != std::string::npos ||
            line.find("Unhandled exception") != std::string::npos)
        {
            log_->error("[N_m3u8DL] {}", line);
        }
        else if (line.find("WARN ") != std::string::npos)
        {
            log_->warn("[N_m3u8DL] {}", line);
        }
        else
        {
            log_->debug("[N_m3u8DL] {}", line);
        }
    }

    bool NM3U8DLRecorder::isAvailable(const std::string &binaryPath)
    {
        if (binaryPath.empty())
            return false;

        // Check if it's an absolute path that exists
        if (fs::exists(binaryPath))
            return true;

        // Try running it with --version to see if it's on PATH
#ifdef _WIN32
        std::string cmd = "\"" + binaryPath + "\" --version >nul 2>&1";
#else
        std::string cmd = "\"" + binaryPath + "\" --version >/dev/null 2>&1";
#endif
        return std::system(cmd.c_str()) == 0;
    }

    std::vector<std::string> NM3U8DLRecorder::buildArgs(
        const std::string &hlsUrl,
        const std::string &outputDir,
        const std::string &outputName,
        const std::string &userAgent,
        const std::map<std::string, std::string> &headers) const
    {
        std::vector<std::string> args;
        args.push_back(config_.n_m3u8dlPath);
        args.push_back(hlsUrl);

        args.push_back("--save-dir");
        args.push_back(outputDir);

        args.push_back("--save-name");
        args.push_back(outputName);

        // Tmp dir inside output dir to keep things clean
        args.push_back("--tmp-dir");
        args.push_back((fs::path(outputDir) / ".tmp_nm3u8dl").string());

        // Binary merge with real-time merge preserves broadcast timestamps
        // in separate .mp4 (video) and .m4a (audio) sidecar files. We then
        // post-mux with computed A/V offset trimming for perfect sync.
        args.push_back("--live-real-time-merge");
        args.push_back("--binary-merge");

        // Thread count — use 4 for live to avoid overwhelming CDN
        args.push_back("--thread-count");
        args.push_back("4");

        // Retry
        args.push_back("--download-retry-count");
        args.push_back("5");

        // Auto-select best streams
        args.push_back("--auto-select");

        // Inherit auth/query params from input URL to child playlist/segments.
        // Required for CB split A/V where audio chunklist URLs may omit signed
        // query tokens in relative URIs.
        args.push_back("--append-url-params");

        // Delete temp files when done
        args.push_back("--del-after-done");

        // No update check
        args.push_back("--disable-update-check");

        // No ANSI colors in log output
        args.push_back("--no-ansi-color");

        // Log level — keep INFO so we capture everything, but we filter on our side
        args.push_back("--log-level");
        args.push_back("INFO");

        // No -M: we handle post-recording mux ourselves with A/V offset
        // correction. N_m3u8DL-RE produces raw .mp4 + .m4a sidecars.

        // Chunk-by-duration: let N_m3u8DL-RE stop cleanly at the limit.
        // Each chunk gets independent A/V offset correction during post-mux.
        if (config_.recordingMode == 2 && config_.chunkDurationMin > 0)
        {
            int mins = config_.chunkDurationMin;
            int h = mins / 60;
            int m = mins % 60;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02d:%02d:00", h, m);
            args.push_back("--live-record-limit");
            args.push_back(buf);
        }

        // User agent
        if (!userAgent.empty())
        {
            args.push_back("-H");
            args.push_back("User-Agent: " + userAgent);
        }
        else if (!config_.userAgent.empty())
        {
            args.push_back("-H");
            args.push_back("User-Agent: " + config_.userAgent);
        }

        // Custom headers
        for (const auto &[key, val] : headers)
        {
            args.push_back("-H");
            args.push_back(key + ": " + val);
        }

        // Proxy
        if (config_.proxyEnabled && !config_.proxies.empty())
        {
            const auto &proxy = config_.proxies[0];
            if (!proxy.url.empty())
            {
                args.push_back("--custom-proxy");
                args.push_back(proxy.url);
            }
        }

        // FFmpeg binary path (for muxing)
        if (!config_.ffmpegPath.empty() && config_.ffmpegPath != "ffmpeg")
        {
            args.push_back("--ffmpeg-binary-path");
            args.push_back(config_.ffmpegPath.string());
        }

        return args;
    }

#ifdef _WIN32
    int NM3U8DLRecorder::runProcess(const std::vector<std::string> &args,
                                    CancellationToken &cancel,
                                    const std::string &pipeEnv)
    {
        // Disable ffmpeg pipe to fix audio desync with split A/V streams
        if (!pipeEnv.empty())
            SetEnvironmentVariableA("N_M3U8DL_NO_FFMPEG_PIPE", pipeEnv.c_str());

        // Build command line string
        std::string cmdLine;
        for (const auto &arg : args)
        {
            if (!cmdLine.empty())
                cmdLine += ' ';
            // Quote arguments that contain spaces
            if (arg.find(' ') != std::string::npos)
                cmdLine += '"' + arg + '"';
            else
                cmdLine += arg;
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe, hWritePipe;
        CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;

        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr,
                                 TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(hWritePipe);

        if (!ok)
        {
            CloseHandle(hReadPipe);
            log_->error("Failed to start N_m3u8DL-RE process");
            return -1;
        }

        // Read output in a loop, check for cancellation
        char buf[4096];
        DWORD bytesRead;
        while (true)
        {
            if (cancel.isCancelled())
            {
                TerminateProcess(pi.hProcess, 1);
                break;
            }

            DWORD avail = 0;
            PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr);
            if (avail > 0)
            {
                if (ReadFile(hReadPipe, buf, std::min<DWORD>(avail, sizeof(buf) - 1), &bytesRead, nullptr) && bytesRead > 0)
                {
                    buf[bytesRead] = '\0';
                    // Log each line
                    std::string output(buf, bytesRead);
                    auto pos = output.find('\n');
                    while (pos != std::string::npos)
                    {
                        auto line = output.substr(0, pos);
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();
                        if (!line.empty())
                            logExternalLine(line);
                        output = output.substr(pos + 1);
                        pos = output.find('\n');
                    }
                    if (!output.empty())
                        logExternalLine(output);
                }
            }

            // Check if process exited
            DWORD exitCode;
            if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);

        return static_cast<int>(exitCode);
    }
#else
    int NM3U8DLRecorder::runProcess(const std::vector<std::string> &args,
                                    CancellationToken &cancel,
                                    const std::string &pipeEnv)
    {
        // Build argv for execvp
        std::vector<const char *> argv;
        for (const auto &a : args)
            argv.push_back(a.c_str());
        argv.push_back(nullptr);

        int pipefd[2];
        if (pipe(pipefd) == -1)
        {
            log_->error("Failed to create pipe for N_m3u8DL-RE");
            return -1;
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            close(pipefd[0]);
            close(pipefd[1]);
            log_->error("Failed to fork N_m3u8DL-RE process");
            return -1;
        }

        if (pid == 0)
        {
            // Child
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            if (!pipeEnv.empty())
                setenv("N_M3U8DL_NO_FFMPEG_PIPE", pipeEnv.c_str(), 1);
            execvp(argv[0], const_cast<char *const *>(argv.data()));
            _exit(127);
        }

        // Parent
        close(pipefd[1]);

        // Set pipe to non-blocking
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

        char buf[4096];
        std::string lineBuffer;

        while (true)
        {
            if (cancel.isCancelled())
            {
                kill(pid, SIGTERM);
                // Give N_m3u8DL-RE up to 15s to finish muxing audio+video
                {
                    bool exited = false;
                    for (int i = 0; i < 30 && !exited; ++i)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        int st;
                        if (waitpid(pid, &st, WNOHANG) != 0)
                            exited = true;
                    }
                    if (!exited)
                        kill(pid, SIGKILL);
                }
                break;
            }

            struct pollfd pfd{pipefd[0], POLLIN, 0};
            int ret = poll(&pfd, 1, 200); // 200ms timeout

            if (ret > 0 && (pfd.revents & POLLIN))
            {
                ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
                if (n > 0)
                {
                    buf[n] = '\0';
                    lineBuffer.append(buf, n);

                    // Process complete lines
                    size_t pos;
                    while ((pos = lineBuffer.find('\n')) != std::string::npos)
                    {
                        auto line = lineBuffer.substr(0, pos);
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();
                        if (!line.empty())
                            logExternalLine(line);
                        lineBuffer = lineBuffer.substr(pos + 1);
                    }
                }
                else if (n == 0)
                {
                    break; // EOF
                }
            }

            // Check if child exited
            int status;
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid)
            {
                // Drain remaining output
                while (true)
                {
                    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
                    if (n <= 0)
                        break;
                    buf[n] = '\0';
                    lineBuffer.append(buf, n);
                }
                if (!lineBuffer.empty())
                    logExternalLine(lineBuffer);

                close(pipefd[0]);
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        }

        // Wait for child after cancellation
        int status;
        waitpid(pid, &status, 0);
        close(pipefd[0]);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif

    NM3U8DLResult NM3U8DLRecorder::record(
        const std::string &hlsUrl,
        const std::string &outputPath,
        CancellationToken &cancel,
        const std::string &userAgent,
        const std::map<std::string, std::string> &headers)
    {
        NM3U8DLResult result;

        fs::path outPath(outputPath);
        std::string outputDir = outPath.parent_path().string();
        std::string outputName = outPath.stem().string();

        // Ensure output directory exists
        std::error_code ec;
        fs::create_directories(outputDir, ec);

        log_->info("Starting N_m3u8DL-RE recording: {} → {}", hlsUrl, outputPath);

        auto args = buildArgs(hlsUrl, outputDir, outputName, userAgent, headers);

        // Disable FFmpeg pipe so N_m3u8DL-RE uses binary merge, producing
        // separate .mp4 (video) and .m4a (audio) files with broadcast
        // timestamps preserved. We handle muxing ourselves with A/V offset
        // correction for perfect sync.
        std::string pipeEnvStr = "1";

        // Log the command
        {
            std::string cmdStr;
            for (const auto &a : args)
            {
                if (!cmdStr.empty())
                    cmdStr += ' ';
                cmdStr += a;
            }
            log_->info("N_m3u8DL-RE command: {}", cmdStr);
        }

        int exitCode = runProcess(args, cancel, pipeEnvStr);

        if (cancel.isCancelled())
        {
            log_->info("N_m3u8DL-RE recording cancelled");
            result.success = true; // partial recording is still valid
        }
        else if (exitCode == 0)
        {
            log_->info("N_m3u8DL-RE recording completed successfully");
            result.success = true;
        }
        else
        {
            if (exitCode == 127)
                log_->error("N_m3u8DL-RE binary not found or cannot execute: {}",
                            config_.n_m3u8dlPath.string());
            else
                log_->error("N_m3u8DL-RE exited with code {}", exitCode);
            result.error = "N_m3u8DL-RE exited with code " + std::to_string(exitCode);
        }

        // --- Post-recording: mux split A/V sidecar files with offset correction ---
        fs::path videoSidecar = fs::path(outputDir) / (outputName + ".mp4");
        fs::path audioSidecar = fs::path(outputDir) / (outputName + ".m4a");
        fs::path desiredPath(outputPath);
        std::string ffmpegStr = "\"" + config_.ffmpegPath.string() + "\"";
        bool isMp4 = desiredPath.extension() == ".mp4";
        std::string movflags = isMp4 ? " -movflags +faststart" : "";

        if (fs::exists(videoSidecar, ec) && fs::exists(audioSidecar, ec))
        {
            // Split A/V stream: probe start_time, compute offset, trim-mux
            fs::path ffprobePath = deriveFFprobePath(config_.ffmpegPath);
            double vStart = probeStartTime(ffprobePath, videoSidecar);
            double aStart = probeStartTime(ffprobePath, audioSidecar);
            double delta = std::abs(aStart - vStart);

            log_->info("Split A/V: video_start={:.3f}s audio_start={:.3f}s delta={:.3f}s",
                       vStart, aStart, delta);

            // Rename video sidecar if it collides with output path
            fs::path videoInput = videoSidecar;
            if (videoSidecar == desiredPath)
            {
                videoInput = fs::path(outputDir) / (outputName + ".video.mp4");
                fs::rename(videoSidecar, videoInput, ec);
            }

            std::string vIn = "\"" + videoInput.string() + "\"";
            std::string aIn = "\"" + audioSidecar.string() + "\"";
            std::string out = "\"" + outputPath + "\"";

            std::string muxCmd;
            std::string d = std::to_string(delta);
            if (aStart > vStart && delta > 0.0)
            {
                log_->info("Trimming {:.3f}s from video start to align with audio", delta);
                muxCmd = ffmpegStr + " -y -ss " + d + " -i " + vIn +
                         " -i " + aIn +
                         " -c copy -map 0:v -map 1:a -shortest" +
                         movflags + " " + out;
            }
            else if (vStart > aStart && delta > 0.0)
            {
                log_->info("Trimming {:.3f}s from audio start to align with video", delta);
                muxCmd = ffmpegStr + " -y -i " + vIn +
                         " -ss " + d + " -i " + aIn +
                         " -c copy -map 0:v -map 1:a -shortest" +
                         movflags + " " + out;
            }
            else
            {
                muxCmd = ffmpegStr + " -y -i " + vIn + " -i " + aIn +
                         " -c copy -map 0:v -map 1:a -shortest" +
                         movflags + " " + out;
            }

            log_->info("Post-mux: {}", muxCmd);
            int muxRc = std::system(muxCmd.c_str());

            if (muxRc == 0 && fs::exists(outputPath, ec))
            {
                result.outputPath = outputPath;
                result.bytesWritten = fs::file_size(outputPath, ec);
                log_->info("Post-mux complete: {} ({} bytes)",
                           desiredPath.filename().string(), result.bytesWritten);
                fs::remove(videoInput, ec);
                fs::remove(audioSidecar, ec);
            }
            else
            {
                log_->warn("Post-mux failed (exit {}), keeping sidecar files", muxRc);
                result.outputPath = videoInput.string();
                result.bytesWritten = fs::file_size(videoInput, ec);
            }
        }
        else if (fs::exists(videoSidecar, ec))
        {
            // Single-track stream (no audio sidecar)
            if (videoSidecar.string() != outputPath)
            {
                std::string cmd = ffmpegStr + " -y -i \"" + videoSidecar.string() +
                                  "\" -c copy" + movflags + " \"" + outputPath + "\"";
                int rc = std::system(cmd.c_str());
                if (rc == 0 && fs::exists(outputPath, ec))
                {
                    fs::remove(videoSidecar, ec);
                    result.outputPath = outputPath;
                    result.bytesWritten = fs::file_size(outputPath, ec);
                }
                else
                {
                    result.outputPath = videoSidecar.string();
                    result.bytesWritten = fs::file_size(videoSidecar, ec);
                }
            }
            else
            {
                result.outputPath = videoSidecar.string();
                result.bytesWritten = fs::file_size(videoSidecar, ec);
            }
        }
        else
        {
            // Fallback: scan for any output file
            for (const auto &ext : {".mkv", ".mp4", ".ts"})
            {
                fs::path candidate = fs::path(outputDir) / (outputName + ext);
                if (fs::exists(candidate, ec))
                {
                    result.outputPath = candidate.string();
                    result.bytesWritten = fs::file_size(candidate, ec);
                    break;
                }
            }
        }

        // Clean up temp directory
        fs::path tmpDir = fs::path(outputDir) / ".tmp_nm3u8dl";
        if (fs::exists(tmpDir, ec))
            fs::remove_all(tmpDir, ec);

        return result;
    }

} // namespace sm
