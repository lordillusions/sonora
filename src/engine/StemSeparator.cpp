#include "engine/StemSeparator.h"

#include <algorithm>
#include <mutex>
#include <thread>

#if ! defined(_WIN32)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sonora::engine
{

namespace
{
juce::File findExecutable(const juce::String& name, const std::vector<juce::File>& extra)
{
    std::vector<juce::File> directories = extra;
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    for (const char* d : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"})
        directories.emplace_back(d);
    directories.push_back(home.getChildFile(".local/bin"));
    directories.push_back(home.getChildFile(".local/pipx/venvs/demucs/bin"));
    directories.push_back(home.getChildFile(".local/pipx/venvs/audio-separator/bin"));
    directories.push_back(home.getChildFile(".local/share/uv/tools/audio-separator/bin"));
    directories.push_back(home.getChildFile(".local/share/uv/tools/demucs/bin"));
    directories.push_back(home.getChildFile("miniconda3/bin"));
    directories.push_back(home.getChildFile("anaconda3/bin"));
    const auto path = juce::SystemStats::getEnvironmentVariable("PATH", {});
#if defined(_WIN32)
    const char* pathSeparator = ";";
#else
    const char* pathSeparator = ":";
#endif
    for (const auto& part : juce::StringArray::fromTokens(path, pathSeparator, ""))
        if (part.isNotEmpty())
            directories.emplace_back(part);
    for (const auto& d : directories)
    {
#if defined(_WIN32)
        for (const auto& candidate : {d.getChildFile(name + ".exe"), d.getChildFile(name + ".cmd"), d.getChildFile(name)})
            if (candidate.existsAsFile())
                return candidate;
#else
        const auto candidate = d.getChildFile(name);
        if (candidate.existsAsFile() && ::access(candidate.getFullPathName().toRawUTF8(), X_OK) == 0)
            return candidate;
#endif
    }
    return {};
}
} // namespace

juce::File StemSeparator::uvrModelFolder()
{
    const juce::File folder("/Applications/Ultimate Vocal Remover.app/Contents/Resources/models/MDX_Net_Models");
    return folder.getChildFile("UVR-MDX-NET-Inst_HQ_3.onnx").existsAsFile() ? folder : juce::File();
}

std::vector<StemTool> StemSeparator::detectTools(const std::vector<juce::File>& extraDirectories)
{
    std::vector<StemTool> tools;
    if (const auto exe = findExecutable("audio-separator", extraDirectories); exe != juce::File())
    {
        StemTool t;
        t.kind = StemTool::Kind::AudioSeparator;
        t.executable = exe;
        t.modelFolder = uvrModelFolder();
        if (t.modelFolder != juce::File())
        {
            t.model = "UVR-MDX-NET-Inst_HQ_3.onnx"; // vocals + instrumental, from the model UVR already has on disk
            t.label = "audio-separator (UVR MDX-Net Inst HQ 3: vocals + instrumental)";
        }
        else
        {
            t.model = "htdemucs_ft.yaml"; // downloaded on first use
            t.label = "audio-separator (htdemucs_ft: drums, bass, other, vocals)";
        }
        tools.push_back(t);
    }
    if (const auto exe = findExecutable("demucs", extraDirectories); exe != juce::File())
    {
        StemTool t;
        t.kind = StemTool::Kind::Demucs;
        t.executable = exe;
        t.model = "htdemucs";
        t.label = "demucs (htdemucs: drums, bass, other, vocals)";
        tools.push_back(t);
    }
    return tools;
}

juce::StringArray StemSeparator::commandLine(const StemTool& tool, const juce::File& input, const juce::File& outputDir)
{
    juce::StringArray args;
    args.add(tool.executable.getFullPathName());
    if (tool.kind == StemTool::Kind::AudioSeparator)
    {
        args.add(input.getFullPathName());
        args.add("--model_filename");
        args.add(juce::String(tool.model));
        if (tool.modelFolder != juce::File())
        {
            args.add("--model_file_dir");
            args.add(tool.modelFolder.getFullPathName());
        }
        args.add("--output_dir");
        args.add(outputDir.getFullPathName());
        args.add("--output_format");
        args.add("WAV");
    }
    else
    {
        args.add("-n");
        args.add(juce::String(tool.model));
        args.add("--int24");
        args.add("-o");
        args.add(outputDir.getFullPathName());
        args.add(input.getFullPathName());
    }
    return args;
}

std::string StemSeparator::stemNameFromFile(const juce::File& file)
{
    const auto base = file.getFileNameWithoutExtension();
    const int open = base.indexOf("_(");
    if (open >= 0)
    {
        const int close = base.indexOfChar(open + 2, ')');
        if (close > open)
            return base.substring(open + 2, close).toStdString(); // audio-separator: "song_(Vocals)_model"
    }
    auto name = base.toStdString();
    if (! name.empty())
        name[0] = (char) std::toupper((unsigned char) name[0]); // demucs: "vocals" -> "Vocals"
    return name;
}

StemResult StemSeparator::separate(const StemTool& tool, const juce::File& input, const juce::File& outputDir,
                                   const std::atomic<bool>& cancel, const std::function<void(const std::string&)>& onLine)
{
    StemResult result;
    if (! tool.executable.existsAsFile())
    {
        result.error = "The separation tool was not found: " + tool.executable.getFullPathName().toStdString();
        return result;
    }
    if (! input.existsAsFile())
    {
        result.error = "The audio file was not found";
        return result;
    }
    if (! outputDir.createDirectory().wasOk())
    {
        result.error = "Cannot create " + outputDir.getFullPathName().toStdString();
        return result;
    }
    const auto before = outputDir.findChildFiles(juce::File::findFiles, true, "*.wav");
#if defined(_WIN32)
    // COM-7: Windows has no fork/process groups; juce::ChildProcess reads stdout+stderr and kill() ends the tool.
    // TODO(COM-7): a Job object so cancel also ends the tool's helper processes.
    juce::ChildProcess child;
    if (! child.start(commandLine(tool, input, outputDir), juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        result.error = "Could not start " + tool.executable.getFileName().toStdString();
        return result;
    }
    // readProcessOutput blocks until its buffer fills or the tool exits, so a reader thread feeds `shared`
    // and this loop stays responsive to cancel.
    std::mutex mutex;
    std::string shared, pending, tail;
    std::thread reader([&] {
        char buffer[2048];
        for (int n; (n = child.readProcessOutput(buffer, (int) sizeof buffer)) > 0;)
        {
            const std::lock_guard lock(mutex);
            shared.append(buffer, (size_t) n);
        }
    });
    auto deliver = [&](const std::string& line) {
        if (line.empty()) return;
        tail = line;
        if (onLine) onLine(line);
    };
    for (bool running = true; running;)
    {
        if (cancel.load())
        {
            child.kill();
            reader.join();
            result.cancelled = true;
            result.error = "Cancelled";
            return result;
        }
        running = child.isRunning();
        if (! running) reader.join();
        {
            const std::lock_guard lock(mutex);
            pending += shared;
            shared.clear();
        }
        size_t cut;
        while ((cut = pending.find_first_of("\n\r")) != std::string::npos)
        {
            deliver(pending.substr(0, cut));
            pending.erase(0, cut + 1);
        }
        if (running) juce::Thread::sleep(50);
    }
    deliver(pending);
    const int code = (int) child.getExitCode();
#else
    // Own fork/exec so the tool runs in its own process group (cancel kills its helpers too) and reads can be polled.
    const auto args = commandLine(tool, input, outputDir);
    std::vector<std::string> storage;
    for (const auto& a : args) storage.push_back(a.toStdString());
    std::vector<char*> argv;
    for (auto& a : storage) argv.push_back(a.data());
    argv.push_back(nullptr);
    int fds[2];
    if (::pipe(fds) != 0)
    {
        result.error = "Could not create a pipe for " + tool.executable.getFileName().toStdString();
        return result;
    }
    const pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        result.error = "Could not start " + tool.executable.getFileName().toStdString();
        return result;
    }
    if (pid == 0)
    {
        ::setpgid(0, 0);
        ::dup2(fds[1], 1);
        ::dup2(fds[1], 2);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    ::setpgid(pid, pid);
    std::string pending, tail;
    char buffer[2048];
    int status = 0;
    bool exited = false;
    auto deliver = [&](const std::string& line) {
        if (line.empty()) return;
        tail = line;
        if (onLine) onLine(line);
    };
    for (;;)
    {
        if (cancel.load())
        {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            ::close(fds[0]);
            ::waitpid(pid, &status, 0);
            result.cancelled = true;
            result.error = "Cancelled";
            return result;
        }
        struct pollfd pfd{fds[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 50);
        if (ready > 0)
        {
            const ssize_t n = ::read(fds[0], buffer, sizeof buffer);
            if (n > 0)
            {
                pending.append(buffer, (size_t) n);
                size_t cut;
                while ((cut = pending.find_first_of("\n\r")) != std::string::npos)
                {
                    deliver(pending.substr(0, cut));
                    pending.erase(0, cut + 1);
                }
                continue;
            }
            if (n == 0) // every writer closed the pipe
            {
                ::waitpid(pid, &status, 0);
                exited = true;
                break;
            }
        }
        if (::waitpid(pid, &status, WNOHANG) == pid)
        {
            exited = true;
            // drain what is left in the pipe
            for (;;)
            {
                struct pollfd last{fds[0], POLLIN, 0};
                if (::poll(&last, 1, 0) <= 0) break;
                const ssize_t n = ::read(fds[0], buffer, sizeof buffer);
                if (n <= 0) break;
                pending.append(buffer, (size_t) n);
            }
            size_t cut;
            while ((cut = pending.find_first_of("\n\r")) != std::string::npos)
            {
                deliver(pending.substr(0, cut));
                pending.erase(0, cut + 1);
            }
            break;
        }
    }
    ::close(fds[0]);
    (void) exited;
    deliver(pending);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
    if (code != 0)
    {
        result.error = "The separation tool failed (exit " + std::to_string(code) + ")" + (tail.empty() ? "" : ": " + tail);
        return result;
    }
    auto after = outputDir.findChildFiles(juce::File::findFiles, true, "*.wav");
    for (const auto& f : after)
    {
        if (std::find(before.begin(), before.end(), f) != before.end())
            continue;
        result.stems.push_back({stemNameFromFile(f), f});
    }
    std::sort(result.stems.begin(), result.stems.end(), [](const StemFile& a, const StemFile& b) {
        static const char* order[] = {"Vocals", "Drums", "Bass", "Other", "Instrumental"};
        auto rank = [](const std::string& n) { for (int i = 0; i < 5; ++i) if (n == order[i]) return i; return 9; };
        return rank(a.name) != rank(b.name) ? rank(a.name) < rank(b.name) : a.name < b.name;
    });
    if (result.stems.empty())
    {
        result.error = "The tool finished but wrote no audio files";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace sonora::engine
