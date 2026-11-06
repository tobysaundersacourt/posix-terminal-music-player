// Pulse: a small POSIX terminal music player. Build: g++ -std=c++17 -O2 play.cpp -o play
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <poll.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static volatile sig_atomic_t interrupted = 0;
static void onSignal(int) { interrupted = 1; }
static std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
static std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// Never let filenames inject terminal control sequences.
static std::string clean(std::string s) {
    for (char &c : s) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
    return s;
}
static double number(const std::string &s) {
    size_t n = 0; double v = std::stod(s, &n);
    if (n != s.size() || !std::isfinite(v)) throw std::runtime_error("Enter a valid number.");
    return v;
}
static std::string timeText(double seconds) {
    int n = static_cast<int>(std::clamp(seconds, 0.0, 2147483647.0));
    std::ostringstream out; out << n / 60 << ':' << std::setw(2) << std::setfill('0') << n % 60;
    return out.str();
}
static std::string decimalText(double value, int precision = 12) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(precision) << value;
    return out.str();
}
// Keep each atempo stage within [0.5, 2] for compatibility and quality.
static std::string tempoFilter(double speed) {
    std::string filter;
    auto stage = [&](double factor) {
        if (!filter.empty()) filter += ',';
        filter += "atempo=" + decimalText(factor, std::numeric_limits<double>::max_digits10);
    };
    while (speed > 2.0) { stage(2.0); speed /= 2.0; }
    while (speed < 0.5) { stage(0.5); speed *= 2.0; }
    stage(speed);
    return filter;
}
static bool executable(const std::string &name) {
    const char *env = std::getenv("PATH"); if (!env) return false;
    std::stringstream paths(env); std::string p;
    while (std::getline(paths, p, ':'))
        if (access(((p.empty() ? "." : p) + "/" + name).c_str(), X_OK) == 0) return true;
    return false;
}
// Launch without a shell; each filename is one literal argument.
static pid_t launch(const std::vector<std::string> &args, int output = -1) {
    std::vector<char *> argv;
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("Unable to start a child process.");
    if (!pid) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull < 0) _exit(126);
        dup2(devnull, STDIN_FILENO); dup2(devnull, STDERR_FILENO);
        dup2(output < 0 ? devnull : output, STDOUT_FILENO);
        if (output > 2) close(output);
        if (devnull > 2) close(devnull);
        execvp(argv[0], argv.data()); _exit(127);
    }
    return pid;
}
static double duration(const fs::path &file) {
    if (!executable("ffprobe")) return 0;
    int fds[2]; if (pipe(fds)) return 0;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC); fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    pid_t pid;
    try { pid = launch({"ffprobe", "-v", "error", "-show_entries", "format=duration",
                        "-of", "default=noprint_wrappers=1:nokey=1", file.string()}, fds[1]); }
    catch (...) { close(fds[0]); close(fds[1]); return 0; }
    close(fds[1]); fcntl(fds[0], F_SETFL, O_NONBLOCK);
    std::string result; auto deadline = Clock::now() + std::chrono::seconds(2);
    bool done = false;
    while (!done && !interrupted && Clock::now() < deadline) {
        pollfd fd{fds[0], POLLIN, 0}; poll(&fd, 1, 50);
        char buf[128]; ssize_t count;
        while ((count = read(fds[0], buf, sizeof(buf))) > 0) result.append(buf, count);
        done = waitpid(pid, nullptr, WNOHANG) == pid;
        if (result.size() > 4096) break;
    }
    if (!done) { kill(pid, SIGKILL); while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }
    close(fds[0]);
    try { return std::max(0.0, number(trim(result))); } catch (...) { return 0; }
}
class Terminal {
    termios saved{};
public:
    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    Terminal() {
        if (!interactive) return;
        if (tcgetattr(STDIN_FILENO, &saved)) throw std::runtime_error("Cannot read terminal settings.");
        termios raw = saved; raw.c_lflag &= ~(ICANON | ECHO); raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw)) throw std::runtime_error("Cannot configure terminal.");
        std::cout << "\033[?1049h\033[?25l";
    }
    ~Terminal() {
        if (interactive) { tcsetattr(STDIN_FILENO, TCSANOW, &saved); std::cout << "\033[0m\033[?25h\033[?1049l" << std::flush; }
    }
};
// Named semantic colors keep every theme consistent across the dashboard.
struct Theme {
    std::string name;
    int background, text, muted, accent, secondary, good, warning, error;
};
static const std::vector<Theme> themes{
    {"midnight", 234, 255, 247, 117, 183, 120, 222, 210},
    {"ocean",     17, 255, 153,  87, 117, 121, 229, 217},
    {"forest",    22, 255, 151, 157, 229, 120, 222, 217},
    {"amber",    234, 230, 180, 220, 215, 150, 229, 210},
    {"light",    255, 234, 241,  25,  91,  22, 130, 160},
    {"mono",      -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1}
};
class Player {
    std::vector<fs::path> tracks;
    size_t index = 0;
    fs::path directory;
    pid_t child = -1;
    bool paused = false, quitting = false, reverse = false, recursive = false;
    bool display = false, repeatOne = false, repeatOnce = false, announce = true, ended = false;
    int volume = 70, failures = 0;
    double offset = 0, length = 0, speed = 1.0;
    Clock::time_point started;
    std::mt19937 rng{std::random_device{}()};
    std::vector<std::string> messages;
    std::string input, draft, escape;
    size_t cursor = 0, historyPos = 0;
    std::vector<std::string> history;
    Terminal &terminal;
    size_t themeIndex = 0;
    bool noColor = std::getenv("NO_COLOR") != nullptr;
    enum Role { Text, Muted, Accent, Secondary, Good, Warning, Error, Title };
    struct Span { std::string text; Role role = Text; bool bold = false; };
    using Row = std::vector<Span>;
    bool colors() const { return !noColor && themes[themeIndex].name != "mono"; }
    std::string baseStyle() const {
        const auto &t = themes[themeIndex];
        return colors() ? "\033[0;48;5;" + std::to_string(t.background) + ";38;5;" + std::to_string(t.text) + "m" : "\033[0m";
    }
    std::string style(Role role, bool bold = false) const {
        if (!colors()) return (bold || role == Title) ? "\033[0;1m" : "\033[0m";
        const auto &t = themes[themeIndex];
        int color = t.text;
        switch (role) {
            case Muted: color = t.muted; break;
            case Accent: case Title: color = t.accent; break;
            case Secondary: color = t.secondary; break;
            case Good: color = t.good; break;
            case Warning: color = t.warning; break;
            case Error: color = t.error; break;
            default: break;
        }
        return "\033[0;48;5;" + std::to_string(t.background) + ";38;5;" + std::to_string(color) + ((bold || role == Title) ? ";1m" : "m");
    }
    void setTheme(std::string name) {
        name = lower(trim(name));
        if (name == "next") themeIndex = (themeIndex + 1) % themes.size();
        else {
            auto it = std::find_if(themes.begin(), themes.end(), [&](const Theme &t) { return t.name == name; });
            if (it == themes.end()) throw std::runtime_error("Unknown theme. Choose midnight, ocean, forest, amber, light or mono.");
            themeIndex = static_cast<size_t>(it - themes.begin());
        }
    }
    void note(const std::string &s) {
        messages.push_back(clean(s)); if (messages.size() > 5) messages.erase(messages.begin());
        if (!terminal.interactive) std::cout << clean(s) << '\n' << std::flush;
    }
    double position() const {
        double p = offset;
        if (!paused && child > 0) p += std::chrono::duration<double>(Clock::now() - started).count() * speed;
        return length > 0 ? std::min(p, length) : p;
    }
    size_t wrapped(long long i) const {
        auto n = static_cast<long long>(tracks.size()); return static_cast<size_t>((i % n + n) % n);
    }
    void stopChild() {
        if (child <= 0) return;
        kill(child, SIGCONT); kill(child, SIGTERM);
        for (int i = 0; i < 30; ++i) {
            pid_t r = waitpid(child, nullptr, WNOHANG);
            if (r == child || (r < 0 && errno == ECHILD)) { child = -1; return; }
            poll(nullptr, 0, 10);
        }
        kill(child, SIGKILL); while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        child = -1;
    }
    void start(bool probe = true) {
        stopChild(); ended = false;
        if (probe) { offset = 0; length = duration(tracks[index]); }
        std::vector<std::string> args{"ffplay", "-hide_banner", "-loglevel", "error", "-autoexit",
            "-volume", std::to_string(volume), "-ss", std::to_string(offset)};
        if (!display) args.push_back("-nodisp");
        else { args.insert(args.end(), {"-window_title", "Pulse", "-x", "640", "-y", "240", "-showmode", "waves"}); }
        if (speed != 1.0) args.insert(args.end(), {"-af", tempoFilter(speed)});
        args.push_back(tracks[index].string());
        child = launch(args); started = Clock::now();
        if (paused) kill(child, SIGSTOP);
        if (probe && announce) note("Now playing: " + tracks[index].filename().string());
    }
    void select(size_t next) {
        index = next; failures = 0; repeatOnce = false; start();
    }
    void togglePause() {
        if (ended) { paused = false; start(); return; }
        if (paused) { started = Clock::now(); kill(child, SIGCONT); paused = false; }
        else { offset = position(); kill(child, SIGSTOP); paused = true; }
    }
    void restartAt(double p) {
        offset = std::max(0.0, length > 0 ? std::min(p, std::max(0.0, length - 0.05)) : p);
        start(false);
    }
    void load(std::string path) {
        path = trim(path);
        if (path.size() >= 2 && ((path.front() == '"' && path.back() == '"') ||
                                 (path.front() == '\'' && path.back() == '\''))) path = path.substr(1, path.size() - 2);
        if (path == "~" || path.rfind("~/", 0) == 0) {
            const char *home = std::getenv("HOME"); if (home) path.replace(0, 1, home);
        }
        if (path.empty()) throw std::runtime_error("Use mvdir PATH (spaces are supported).");
        fs::path root = fs::canonical(path);
        if (!fs::is_directory(root)) throw std::runtime_error("That path is not a directory.");
        std::vector<fs::path> found;
        const std::vector<std::string> extensions{".mp3", ".flac", ".wav", ".ogg", ".opus", ".m4a", ".aac", ".aiff", ".aif", ".wma", ".mp4"};
        auto add = [&](const fs::directory_entry &e) {
            if (e.is_regular_file() && std::find(extensions.begin(), extensions.end(), lower(e.path().extension().string())) != extensions.end()) found.push_back(e.path());
        };
        if (recursive) for (const auto &e : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) add(e);
        else for (const auto &e : fs::directory_iterator(root)) add(e);
        if (found.empty()) throw std::runtime_error("No supported audio files found; current queue kept.");
        std::sort(found.begin(), found.end(), [](const fs::path &a, const fs::path &b) {
            auto x = lower(a.string()), y = lower(b.string()); return x == y ? a.string() < b.string() : x < y;
        });
        stopChild(); tracks = std::move(found); directory = root; index = 0; failures = 0;
        repeatOnce = false; paused = false; start();
        note(std::to_string(tracks.size()) + " tracks loaded.");
    }
    void list(long long count) {
        long long n = static_cast<long long>(tracks.size());
        long long amount = count == 0 ? 1 : std::min(count < 0 ? -count : count, n);
        for (long long i = 0; i < amount; ++i) {
            size_t j = wrapped(static_cast<long long>(index) + (count < 0 ? -i : i));
            note(std::string(j == index ? "> " : "  ") + std::to_string(j + 1) + ". " + fs::relative(tracks[j], directory).string());
        }
    }
    void help() {
        note("next / previous / random | pause / resume | stop / quit");
        note("tracks [N or -N] | track | find TEXT | play NUMBER or TEXT");
        note("seek +/-SECONDS or SECONDS | speed 0.01..100 | volume 0..100 | mute");
        note("shuffle | sort | forward / reverse | once / loop | mode");
        note("mvdir PATH | theme [NAME / next] | themes | display | help");
    }
public:
    Player(Terminal &t, bool recurse, const std::string &theme = "midnight") : recursive(recurse), terminal(t) { setTheme(theme); }
    ~Player() { stopChild(); }
    void open(const std::string &path) { load(path); }
    void command(std::string line) {
        line = trim(line); if (line.empty()) return;
        if (history.empty() || history.back() != line) history.push_back(line);
        historyPos = history.size(); draft.clear();
        auto split = line.find_first_of(" \t");
        std::string cmd = lower(line.substr(0, split));
        std::string arg = split == std::string::npos ? "" : trim(line.substr(split + 1));
        try {
            if (cmd == "quit" || cmd == "exit" || cmd == "stop" || cmd == "q") quitting = true;
            else if (cmd == "help" || cmd == "?") help();
            else if (cmd == "themes") {
                note("Themes: midnight, ocean, forest, amber, light, mono. Use theme NAME.");
            }
            else if (cmd == "theme") {
                if (!arg.empty()) setTheme(arg);
                note("Theme: " + themes[themeIndex].name + (noColor ? " (NO_COLOR is set)" : ""));
            }
            else if (cmd == "mvdir" || cmd == "open") load(arg);
            else if (tracks.empty()) note("Open a folder with mvdir PATH.");
            else if (cmd == "next" || cmd == "n") select(wrapped(static_cast<long long>(index) + 1));
            else if (cmd == "previous" || cmd == "prev" || cmd == "p") select(wrapped(static_cast<long long>(index) - 1));
            else if (cmd == "pause" || cmd == "space") {
                togglePause(); note(paused ? "Playback paused. Type pause or resume to continue." : "Playback resumed.");
            }
            else if (cmd == "resume") {
                if (paused || ended) { togglePause(); note("Playback resumed."); }
                else note("Playback is already running.");
            }
            else if (cmd == "random") {
                if (tracks.size() > 1) select((index + std::uniform_int_distribution<size_t>(1, tracks.size() - 1)(rng)) % tracks.size());
                else select(0);
            }
            else if (cmd == "track") list(0);
            else if (cmd == "tracks") {
                double value = arg.empty() ? static_cast<double>(tracks.size()) : number(arg);
                if (std::floor(value) != value || std::abs(value) > 1000000) throw std::runtime_error("Track count must be an integer between -1000000 and 1000000.");
                list(static_cast<long long>(value));
            }
            else if (cmd == "find" || cmd == "search" || cmd == "play") {
                if (arg.empty()) throw std::runtime_error("Supply a track number or search text.");
                if (cmd == "play" && arg.find_first_not_of("0123456789") == std::string::npos) {
                    double v = number(arg);
                    if (v < 1 || v > tracks.size()) throw std::runtime_error("Track number is outside the queue.");
                    select(static_cast<size_t>(v) - 1);
                } else {
                    std::vector<size_t> matches;
                    for (size_t i = 0; i < tracks.size(); ++i)
                        if (lower(fs::relative(tracks[i], directory).string()).find(lower(arg)) != std::string::npos) matches.push_back(i);
                    if (cmd == "play" && matches.size() == 1) select(matches[0]);
                    else {
                        if (matches.empty()) note("No matching tracks.");
                        for (size_t i : matches) note(std::to_string(i + 1) + ". " + fs::relative(tracks[i], directory).string());
                        if (cmd == "play" && matches.size() > 1) note("Multiple matches; use play NUMBER.");
                    }
                }
            }
            else if (cmd == "seek") {
                double p = number(arg); if (arg.front() == '+' || arg.front() == '-') p += position(); restartAt(p);
                note("Seek: " + timeText(offset) + " into the track.");
            }
            else if (cmd == "speed" || cmd == "rate") {
                if (arg.empty()) note("Speed: " + decimalText(speed) + "x");
                else {
                    double value = number(arg);
                    if (value < 0.01 || value > 100.0)
                        throw std::runtime_error("Speed must be between 0.01 and 100 (1 = normal).");
                    if (value != speed) {
                        // Snapshot source position with the OLD rate before changing it.
                        double p = position(); speed = value; restartAt(p);
                    }
                    note("Speed: " + decimalText(speed) + "x (pitch preserved)");
                }
            }
            else if (cmd == "volume" || cmd == "vol") {
                if (arg.empty()) note("Volume: " + std::to_string(volume) + "%");
                else {
                    double v = number(arg); if (v < 0 || v > 100) throw std::runtime_error("Volume must be between 0 and 100.");
                    double p = position(); volume = static_cast<int>(v); restartAt(p);
                    note("Volume: " + std::to_string(volume) + "%");
                }
            }
            else if (cmd == "mute") {
                double p = position(); volume = volume == 0 ? 70 : 0; restartAt(p);
                note(volume == 0 ? "Muted. Type mute to restore volume." : "Unmuted. Volume: " + std::to_string(volume) + "%");
            }
            else if (cmd == "shuffle" || cmd == "sort") {
                fs::path current = tracks[index];
                if (cmd == "shuffle") std::shuffle(tracks.begin(), tracks.end(), rng);
                else std::sort(tracks.begin(), tracks.end());
                index = static_cast<size_t>(std::find(tracks.begin(), tracks.end(), current) - tracks.begin());
                note(cmd == "shuffle" ? "Queue shuffled; current track kept." : "Queue sorted.");
            }
            else if (cmd == "forward") {
                reverse = false; note("Direction: FORWARD. Automatic track changes go forward.");
            }
            else if (cmd == "reverse") {
                reverse = !reverse;
                note(reverse ? "Direction: REVERSE. Automatic track changes go backward."
                             : "Direction: FORWARD. Automatic track changes go forward.");
            }
            else if (cmd == "once") {
                repeatOnce = !repeatOnce; repeatOne = false;
                note(repeatOnce ? "Repeat once ON: this track will play one extra time."
                                : "Repeat once OFF: playlist playback resumes.");
            }
            else if (cmd == "loop") {
                repeatOne = !repeatOne; repeatOnce = false;
                note(repeatOne ? "Loop ON: repeating the current track until you turn loop off."
                               : "Loop OFF: playlist playback resumes.");
            }
            else if (cmd == "mode") note(std::string(reverse ? "Reverse" : "Forward") + (repeatOne ? " / repeat track" : repeatOnce ? " / repeat once" : " / repeat queue"));
            else if (cmd == "notify") { announce = !announce; note(announce ? "Track announcements on." : "Track announcements off."); }
            else if (cmd == "display") { double p = position(); display = !display; restartAt(p); note(display ? "Waveform window on." : "Waveform window off."); }
            else if (cmd != "null") note("Unknown command: " + line + ". Type help.");
        } catch (const std::exception &e) { note(std::string("Error: ") + e.what()); }
    }
    void tick() {
        if (child <= 0) return;
        int status = 0; if (waitpid(child, &status, WNOHANG) != child) return;
        child = -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            note("Could not play: " + tracks[index].filename().string());
            if (++failures >= static_cast<int>(tracks.size())) { ended = true; note("Playback stopped: all tracks failed. Try play NUMBER."); return; }
            index = wrapped(static_cast<long long>(index) + (reverse ? -1 : 1));
        } else {
            failures = 0;
            if (repeatOnce) repeatOnce = false;
            else if (!repeatOne) index = wrapped(static_cast<long long>(index) + (reverse ? -1 : 1));
        }
        start();
    }
    void key(char c) {
        if (!escape.empty()) {
            escape += c;
            if (escape == "\033[") return;
            if (escape == "\033[A") {
                if (historyPos == history.size()) draft = input;
                if (historyPos > 0) input = history[--historyPos];
                cursor = input.size();
            } else if (escape == "\033[B") {
                if (historyPos < history.size()) ++historyPos;
                input = historyPos == history.size() ? draft : history[historyPos]; cursor = input.size();
            } else if (escape == "\033[C") cursor = std::min(cursor + 1, input.size());
            else if (escape == "\033[D" && cursor) --cursor;
            escape.clear(); return;
        }
        if (c == '\033' && terminal.interactive) escape = c;
        else if (c == '\n' || c == '\r') { command(input); input.clear(); cursor = 0; }
        else if (c == 4) quitting = true;
        else if (c == 1) cursor = 0;
        else if (c == 5) cursor = input.size();
        else if (c == 21) { input.clear(); cursor = 0; }
        else if (c == 127 || c == 8) { if (cursor) input.erase(--cursor, 1); }
        else if (static_cast<unsigned char>(c) >= 32) { input.insert(cursor, 1, c); ++cursor; }
    }
    void render() {
        if (!terminal.interactive) return;
        winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
        int width = ws.ws_col ? ws.ws_col : 80, height = ws.ws_row ? ws.ws_row : 24;
        int usable = std::max(0, width - 1);
        std::vector<Row> rows;
        auto row = [&](std::string text, Role role = Text, bool bold = false) {
            rows.push_back({{clean(text), role, bold}});
        };
        auto blank = [&]() { row(""); };
        auto divider = [&]() { row(std::string(std::max(0, std::min(usable, 88)), '-'), Muted); };
        auto commandRow = [&](const std::string &command, const std::string &description) {
            rows.push_back({{"  " + command, Accent, true}, {"  " + description, Muted}});
        };
        rows.push_back({{" PULSE", Title, true}, {"    /    " + themes[themeIndex].name, Muted}});
        row(tracks.empty() ? " No music folder open" : " " + directory.string(), Muted);
        blank();
        std::vector<Row> details;
        details.push_back({{"NOW PLAYING", Secondary, true},
            {tracks.empty() ? "" : "   " + std::to_string(index + 1) + " / " + std::to_string(tracks.size()), Muted}});
        details.push_back({{tracks.empty() ? "Choose a music folder" : clean(tracks[index].stem().string()), Title, true}});
        Role statusColor = tracks.empty() || ended ? Error : paused ? Warning : Good;
        details.push_back({{tracks.empty() ? "[ IDLE ]" : ended ? "[ STOPPED ]" : paused ? "[ PAUSED ]" : "[ PLAYING ]", statusColor, true}});
        bool art = width >= 70 && height >= 23;
        int artWidth = art ? 22 : 0;
        int bars = std::clamp(width - artWidth - 25, 3, 42);
        int filled = length > 0 ? static_cast<int>(bars * std::clamp(position() / length, 0.0, 1.0)) : 0;
        details.push_back({{"[", Muted}, {std::string(filled, '='), Accent, true},
            {std::string(bars - filled, '-') + "] ", Muted},
            {timeText(position()), Text, true}, {" / " + (length > 0 ? timeText(length) : "--:--"), Muted}});
        details.push_back({{"VOL ", Muted}, {std::to_string(volume) + "%", Text, true},
            {"    SPEED ", Muted}, {decimalText(speed) + "x", Secondary, true}});
        details.push_back({{reverse ? "REVERSE" : "FORWARD", Muted}, {"  /  ", Muted},
            {repeatOne ? "REPEAT TRACK" : repeatOnce ? "REPEAT ONCE" : "REPEAT QUEUE", Secondary}});
        // Deliberately ASCII: works without an icon font or Unicode glyphs.
        const std::vector<std::string> disk{
            "     .--------.     ",
            "   .' .------. '.   ",
            "  / .'  .--.  '. \\  ",
            " | |   ( () )   | | ",
            "  \\ '.  '--'  .' /  ",
            "   '. '------' .'   ",
            "     '--------'     "
        };
        if (art) {
            for (size_t i = 0; i < disk.size(); ++i) {
                Row combined{{disk[i] + "  ", i == 3 ? Secondary : Accent, false}};
                if (i < details.size()) combined.insert(combined.end(), details[i].begin(), details[i].end());
                rows.push_back(std::move(combined));
            }
        } else {
            for (auto &d : details) rows.push_back(std::move(d));
        }
        // Prioritize playback, command instructions and the input on short screens.
        if (height >= 24 && !tracks.empty()) {
            blank(); row("UP NEXT", Secondary, true);
            size_t count = std::min<size_t>(height >= 29 ? 3 : 1, tracks.size() - 1);
            if (!count) row("  End of queue - repeats current track", Muted);
            for (size_t i = 1; i <= count; ++i) {
                size_t j = wrapped(static_cast<long long>(index) + (reverse ? -static_cast<long long>(i) : static_cast<long long>(i)));
                rows.push_back({{"  " + std::to_string(j + 1) + "  ", Accent}, {clean(tracks[j].filename().string()), Text}});
            }
        }
        blank(); divider();
        row("COMMANDS", Secondary, true);
        row("Type a command below, then press Enter.", Muted);
        if (tracks.empty()) commandRow("mvdir PATH", "open a music folder");
        else {
            commandRow("pause", "pause / resume");
            if (height >= 22) commandRow("next", "next track");
            commandRow("speed 1.25", "set playback speed");
        }
        if (height >= 20) commandRow("theme ocean", "change colors (themes lists all)");
        commandRow("help", "all commands");
        int room = std::max(0, height - 1);
        int extra = room - static_cast<int>(rows.size());
        if (extra > 0 && !messages.empty()) {
            // Feedback is labeled separately so status and example commands never mix.
            size_t count = std::min<size_t>(extra, messages.size());
            for (size_t i = messages.size() - count; i < messages.size(); ++i) {
                const auto &m = messages[i];
                bool error = m.rfind("Error:", 0) == 0 || m.rfind("Unknown command:", 0) == 0 ||
                    m.rfind("Could not play:", 0) == 0 || m.rfind("Playback stopped:", 0) == 0;
                bool current = m.rfind("> ", 0) == 0 || m.rfind("Now playing:", 0) == 0;
                rows.push_back({{std::string(error ? "ERROR" : "INFO") + "  ", error ? Error : Muted, true},
                    {m, error ? Error : current ? Good : Text, current}});
            }
        }
        // Clip plain spans BEFORE inserting ANSI sequences; styling never gets cut.
        std::cout << "\033[?25l" << baseStyle() << "\033[H";
        size_t startLine = rows.size() > static_cast<size_t>(room) ? rows.size() - room : 0;
        for (size_t i = startLine; i < rows.size(); ++i) {
            int remaining = usable;
            for (const auto &span : rows[i]) {
                if (remaining <= 0) break;
                std::string text = clean(span.text).substr(0, remaining);
                std::cout << style(span.role, span.bold) << text;
                remaining -= static_cast<int>(text.size());
            }
            std::cout << baseStyle() << "\033[K\r\n";
        }
        std::string prompt = width >= 18 ? "command > " : "> ";
        size_t available = static_cast<size_t>(std::max(1, usable - static_cast<int>(prompt.size())));
        size_t begin = cursor >= available ? cursor - available + 1 : 0;
        std::cout << style(Accent, true) << prompt << style(Text) << clean(input.substr(begin, available))
                  << baseStyle() << "\033[K\033[J\r\033[" << prompt.size() + 1 + cursor - begin << "G\033[?25h" << std::flush;
    }
    void run() {
        if (!terminal.interactive) note("Pulse ready. Type help for commands.");
        while (!quitting && !interrupted) {
            tick(); render();
            pollfd fd{STDIN_FILENO, POLLIN, 0};
            int r = poll(&fd, 1, 150);
            if (r > 0 && (fd.revents & (POLLIN | POLLHUP))) {
                char buf[256]; ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n <= 0) { if (!input.empty()) command(input); break; }
                for (ssize_t i = 0; i < n && !quitting; ++i) key(buf[i]);
            } else if (r < 0 && errno != EINTR) break;
        }
    }
};
int main(int argc, char **argv) {
    bool recursive = false; std::string directory, theme = "midnight";
    if (const char *preferred = std::getenv("PULSE_THEME")) theme = preferred;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Pulse: terminal music player\nUsage: play [--recursive] [--theme NAME] [MUSIC_DIRECTORY]\nRequires ffplay; ffprobe enables duration display. Type help inside the player.\n"; return 0;
        }
        if (arg == "--recursive" || arg == "-r") recursive = true;
        else if (arg == "--theme") {
            if (i + 1 >= argc) { std::cerr << "--theme requires a name.\n"; return 1; }
            theme = argv[++i];
        }
        else if (directory.empty()) directory = arg;
        else { std::cerr << "Unexpected argument: " << arg << '\n'; return 1; }
    }
    if (!executable("ffplay")) { std::cerr << "ffplay is missing. Install FFmpeg (Ubuntu: sudo apt install ffmpeg).\n"; return 1; }
    std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal); std::signal(SIGHUP, onSignal);
    try {
        Terminal terminal; Player player(terminal, recursive, theme);
        if (!directory.empty()) player.open(directory);
        player.run();
    } catch (const std::exception &e) { std::cerr << "Pulse: " << e.what() << '\n'; return 1; }
}
