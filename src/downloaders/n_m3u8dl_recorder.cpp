// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — N_m3u8DL-RE external recorder implementation
// ─────────────────────────────────────────────────────────────────

#include "downloaders/n_m3u8dl_recorder.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <array>
#include <cstdio>
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

        // --live-pipe-mux forces LiveRealTimeMerge on, which preserves
        // broadcast timestamps ensuring audio/video stay in sync.
        // Combined with N_M3U8DL_NO_FFMPEG_PIPE=1 (set in record()), this
        // bypasses the ffmpeg named-pipe approach that causes A/V desync
        // and instead uses N_m3u8DL-RE's internal binary merger.
        // (Reference: KFERMercer/ctbcap#54, ctbcap#56)
        args.push_back("--live-pipe-mux");

        // Thread count — use 4 for live to avoid overwhelming CDN
        args.push_back("--thread-count");
        args.push_back("4");

        // Retry
        args.push_back("--download-retry-count");
        args.push_back("5");

        // Auto-select best streams
        args.push_back("--auto-select");

        // Delete temp files when done
        args.push_back("--del-after-done");

        // No update check
        args.push_back("--disable-update-check");

        // No ANSI colors in log output
        args.push_back("--no-ansi-color");

        // Log level — keep INFO so we capture everything, but we filter on our side
        args.push_back("--log-level");
        args.push_back("INFO");

        // Post-recording mux: remux binary-merged tracks into desired container.
        // This runs AFTER recording completes (not during live stream).
        std::string ext;
        switch (config_.container)
        {
        case ContainerFormat::MP4:
            ext = "mp4";
            break;
        case ContainerFormat::TS:
            ext = "ts";
            break;
        default:
            ext = "mkv";
            break;
        }
        args.push_back("-M");
        args.push_back("format=" + ext);

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

        // N_M3U8DL_NO_FFMPEG_PIPE=1 disables the ffmpeg named-pipe
        // approach that causes audio desync with split audio/video streams.
        // N_m3u8DL-RE's internal binary merger preserves broadcast timestamps,
        // then -M remuxes into the desired container after recording.
        // (Fix for: KFERMercer/ctbcap#54, ctbcap#56)
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

        // Check expected output path first, then scan for alternatives
        if (fs::exists(outputPath, ec))
        {
            result.outputPath = outputPath;
            result.bytesWritten = fs::file_size(outputPath, ec);
        }
        else
        {
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

        // If we got a .ts file but wanted a different container, remux with ffmpeg
        if (!result.outputPath.empty())
        {
            fs::path found(result.outputPath);
            fs::path desired(outputPath);
            if (found.extension() == ".ts" && desired.extension() != ".ts" &&
                result.bytesWritten > 0)
            {
                log_->info("Remuxing {} → {}", found.filename().string(),
                           desired.filename().string());
                std::string ffmpeg = config_.ffmpegPath.string();
                std::string cmd = ffmpeg +
                    " -y -i \"" + result.outputPath +
                    "\" -c copy -movflags +faststart \"" +
                    outputPath + "\"";
                int rc = std::system(cmd.c_str());
                if (rc == 0 && fs::exists(outputPath, ec))
                {
                    fs::remove(found, ec);
                    result.outputPath = outputPath;
                    result.bytesWritten = fs::file_size(outputPath, ec);
                    log_->info("Remux complete: {} ({} bytes)",
                               desired.filename().string(), result.bytesWritten);
                }
                else
                {
                    log_->warn("Remux failed (exit {}), keeping .ts file", rc);
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
