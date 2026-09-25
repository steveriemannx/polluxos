#pragma once

// PolluxOS desktop settings.
//
// One small key=value file, $HOME/.config/polluxdesk/settings.conf, is the
// only channel between the settings app and everything that has to react to
// it.  The shell polls the file's mtime on the once-a-second timer it already
// runs, so a change made in the settings app -- or typed into the file by
// hand -- shows up within a second with no signals, no sockets and no private
// Wayland protocol.  The compositor reads the same file and prefix-matches the
// keys it knows, so keys added here are ignored by it rather than rejected.
//
// Header-only and deliberately free of dui types: the shell, the settings app,
// the file browser and the launchpad all include it, and the compositor
// (plain C) writes the same format from the other side.

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace pollux {

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------
inline constexpr const char* kKeyWallpaper    = "wallpaper";      // compositor
inline constexpr const char* kKeyResolution   = "resolution";     // compositor
inline constexpr const char* kKeyAppearance   = "appearance";     // light|dark|auto
inline constexpr const char* kKeyAccent       = "accent";
inline constexpr const char* kKeyDockIconPx   = "dock_icon_px";
inline constexpr const char* kKeyDockPosition = "dock_position";  // bottom|left|right

// Wallpapers the compositor knows how to paint.
inline constexpr const char* kWallpapers[] = { "blue", "purple", "dark", "green" };

// Icon sizes the dock is laid out for.  Anything else in the file is clamped
// to the nearest of these, so a hand-edited value cannot produce a dock whose
// hit targets and drawing disagree.
inline constexpr int kDockIconSizes[] = { 36, 44, 56 };

struct Settings {
    std::string wallpaper    = "blue";
    std::string resolution;                    // "WxH"; empty leaves it alone
    std::string appearance   = "light";
    std::string accent       = "blue";
    int         dockIconPx   = 36;
    std::string dockPosition = "bottom";       // bottom | left | right
};

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
inline std::string HomeDir() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) : std::string("/tmp");
}

inline std::string ConfigDir()  { return HomeDir() + "/.config/polluxdesk"; }
inline std::string ConfigPath() { return ConfigDir() + "/settings.conf"; }
// Written by the compositor: the window list the dock draws indicators from.
inline std::string StatePath()  { return ConfigDir() + "/state.conf"; }
// Written by the compositor: the output modes the display panel lists.
inline std::string OutputsPath(){ return ConfigDir() + "/outputs.conf"; }
// Window thumbnails for the dock's minimized shelf, one PNG per window id.
inline std::string ThumbDir()   { return ConfigDir() + "/thumbs"; }
inline std::string ThumbPath(unsigned long id) {
    return ThumbDir() + "/" + std::to_string(id) + ".png";
}

// The compositor writes its process id here at startup.
inline std::string PidPath() { return ConfigDir() + "/compositor.pid"; }

inline std::string ReadFileText(const std::string& path) {
    std::string out;
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) return out;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), file)) > 0) {
        out.append(buf, n);
    }
    std::fclose(file);
    return out;
}

inline bool FileExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Run a command and capture its standard output.  Every hardware reading in
// the settings app (mixer, ifconfig, sysctl, df) goes through here.
inline std::string Run(const char* cmd) {
    std::string out;
    FILE* pipe = ::popen(cmd, "r");
    if (pipe == nullptr) return out;
    char buf[512];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), pipe)) > 0) {
        out.append(buf, n);
    }
    ::pclose(pipe);
    return out;
}

// Modification time in nanoseconds, or 0 when the file does not exist.  Used
// as a change token: cheaper and more reliable than watching the contents.
inline unsigned long long MtimeNs(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<unsigned long long>(st.st_mtim.tv_sec) * 1000000000ULL +
           static_cast<unsigned long long>(st.st_mtim.tv_nsec);
}

// Ask the compositor to re-read its settings.
//
// By pid, never by name: the process is called polluxdesk-compositor, but BSD
// keeps only the first 19 characters of that in the process table, so
// `pkill -x polluxdesk-compositor` matches nothing at all. That is what the
// settings app used to do, which is why its wallpaper and resolution buttons
// had never once reached the compositor.
inline bool NotifyCompositor() {
    const std::string pidText = Trim(ReadFileText(PidPath()));
    if (pidText.empty()) {
        return false;
    }
    const long pid = std::atol(pidText.c_str());
    if (pid <= 0) {
        return false;
    }
    return ::kill(static_cast<pid_t>(pid), SIGUSR1) == 0;
}

// Write a whole file so that a reader either sees the previous contents or the
// new ones, never a half-written line.  The temporary lives in the target
// directory because rename() is only atomic within one filesystem.
inline bool WriteFileAtomic(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        ssize_t written = ::write(fd, p, left);
        if (written <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        p += written;
        left -= static_cast<size_t>(written);
    }
    // Flush to disk before the rename publishes it: otherwise a crash can
    // leave the new name pointing at blocks that were never written.
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// mkdir -p.  Creating only the leaf is not enough: a home directory that does
// not exist yet, or has no .config, would make the mkdir fail and every save
// silently do nothing.
inline void MkdirP(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        const std::string part = path.substr(0, i);
        ::mkdir(part.c_str(), 0755);   // EEXIST included, deliberately ignored
    }
}

inline void EnsureConfigDir() { MkdirP(ConfigDir()); }

// ---------------------------------------------------------------------------
// Derived values
// ---------------------------------------------------------------------------
// Clamp to the sizes the dock is laid out for.  Ties go to the larger size so
// the result rounds up towards the default rather than away from it.
inline int DockIconPx(const Settings& s) {
    int best = kDockIconSizes[0];
    int bestDelta = std::abs(s.dockIconPx - best);
    for (int size : kDockIconSizes) {
        int delta = std::abs(s.dockIconPx - size);
        if (delta <= bestDelta) {
            bestDelta = delta;
            best = size;
        }
    }
    return best;
}

// "auto" follows the local clock rather than the real sunrise: there is no
// geolocation here, and an evening switch is what people actually notice.
inline bool IsDark(const Settings& s) {
    if (s.appearance == "dark")  return true;
    if (s.appearance == "light") return false;
    std::time_t now = std::time(nullptr);
    struct tm local;
    if (::localtime_r(&now, &local) == nullptr) return false;
    return local.tm_hour < 7 || local.tm_hour >= 19;
}

// Accent colours, in dui's #AARRGGBB form.  Dark mode uses the lighter tints,
// as macOS does: the light-mode blues and reds disappear against a dark
// surface.
inline const char* AccentHex(const Settings& s) {
    const bool dark = IsDark(s);
    const std::string& a = s.accent;
    if (a == "purple")   return dark ? "#FFBF5AF2" : "#FFAF52DE";
    if (a == "pink")     return dark ? "#FFFF375F" : "#FFFF2D55";
    if (a == "red")      return dark ? "#FFFF453A" : "#FFFF3B30";
    if (a == "orange")   return dark ? "#FFFF9F0A" : "#FFFF9500";
    if (a == "yellow")   return dark ? "#FFFFD60A" : "#FFFFCC00";
    if (a == "green")    return dark ? "#FF32D74B" : "#FF34C759";
    if (a == "graphite") return dark ? "#FF98989D" : "#FF8E8E93";
    return dark ? "#FF0A84FF" : "#FF007AFF";   // blue, the default
}

// ---------------------------------------------------------------------------
// Reading and writing
// ---------------------------------------------------------------------------
// Every line of the file, in order, so unknown keys and comments survive a
// write-back.  Keys are compared before the '=' only.
inline std::vector<std::string> ReadLines(const std::string& path) {
    std::vector<std::string> lines;
    FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) return lines;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), file) != nullptr) {
        lines.push_back(Trim(buf));
    }
    std::fclose(file);
    return lines;
}

inline std::string ValueOf(const std::vector<std::string>& lines, const char* key) {
    const size_t keyLen = std::strlen(key);
    for (const std::string& line : lines) {
        if (line.size() > keyLen && line.compare(0, keyLen, key) == 0 &&
            line[keyLen] == '=') {
            return line.substr(keyLen + 1);
        }
    }
    return std::string();
}

inline Settings Load() {
    const std::vector<std::string> lines = ReadLines(ConfigPath());
    Settings s;
    std::string v;

    if (!(v = ValueOf(lines, kKeyWallpaper)).empty())    s.wallpaper = v;
    if (!(v = ValueOf(lines, kKeyResolution)).empty())   s.resolution = v;
    if (!(v = ValueOf(lines, kKeyAppearance)).empty())   s.appearance = v;
    if (!(v = ValueOf(lines, kKeyAccent)).empty())       s.accent = v;
    if (!(v = ValueOf(lines, kKeyDockPosition)).empty()) s.dockPosition = v;
    if (!(v = ValueOf(lines, kKeyDockIconPx)).empty()) {
        int px = std::atoi(v.c_str());
        if (px > 0) s.dockIconPx = px;
    }
    s.dockIconPx = DockIconPx(s);
    return s;
}

// Change exactly one key: read the whole file, replace the matching line or
// append one, then swap the new file in atomically.  Appending a fresh line
// each time -- which is what this replaces -- left the file holding every
// value ever chosen, and the last one won only by luck of read order.
inline bool SaveKey(const char* key, const std::string& value) {
    EnsureConfigDir();
    const std::string path = ConfigPath();
    std::vector<std::string> lines = ReadLines(path);

    const size_t keyLen = std::strlen(key);
    bool replaced = false;
    for (std::string& line : lines) {
        if (line.size() > keyLen && line.compare(0, keyLen, key) == 0 &&
            line[keyLen] == '=') {
            line = std::string(key) + "=" + value;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        lines.push_back(std::string(key) + "=" + value);
    }

    std::string out;
    for (const std::string& line : lines) {
        if (line.empty()) continue;
        out += line;
        out += '\n';
    }
    return WriteFileAtomic(path, out);
}

}  // namespace pollux
