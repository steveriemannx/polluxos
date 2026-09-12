#include "WifiData.h"

#include "PolluxSettings.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wifi {
namespace {

const char* kWpaCli = "/usr/sbin/wpa_cli";

std::string Trim(const std::string& text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

/** Trim, and drop the "OK" wpa_cli answers down to something comparable. */
std::string CleanReply(const std::string& raw)
{
    std::string out;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos) {
            end = raw.size();
        }
        const std::string line = Trim(raw.substr(start, end - start));
        // wpa_cli echoes a "> " prompt per command in interactive mode.
        if (!line.empty() && line.compare(0, 2, "> ") != 0) {
            if (!out.empty()) {
                out += '\n';
            }
            out += line;
        }
        start = end + 1;
    }
    return out;
}

// The scan listing is column-aligned under a header, and an SSID may contain
// spaces, so the columns are located from the header once and sliced by
// position rather than split on whitespace.
struct ScanColumns
{
    size_t ssid = 0, bssid = 0, chan = 0, rate = 0, signal = 0, caps = 0;
    bool valid = false;
};

ScanColumns FindScanColumns(const std::string& header)
{
    ScanColumns columns;
    const char* names[] = { "SSID", "BSSID", "CHAN", "RATE", "S:N", "CAPS" };
    size_t* targets[] = { &columns.ssid, &columns.bssid, &columns.chan,
                          &columns.rate, &columns.signal, &columns.caps };
    for (int i = 0; i < 6; ++i) {
        const size_t at = header.find(names[i]);
        if (at == std::string::npos) {
            return columns;
        }
        *targets[i] = at;
    }
    columns.valid = columns.ssid < columns.bssid && columns.bssid < columns.chan &&
                    columns.chan < columns.rate && columns.rate < columns.signal &&
                    columns.signal < columns.caps;
    return columns;
}

std::string Slice(const std::string& line, size_t from, size_t to)
{
    if (from >= line.size()) {
        return std::string();
    }
    if (to == std::string::npos || to > line.size()) {
        to = line.size();
    }
    return Trim(line.substr(from, to - from));
}

/** RSN is WPA2, and WPA3 announces itself by name in the capabilities. */
std::string SecurityName(const std::string& caps)
{
    if (caps.find("SAE") != std::string::npos) {
        return "WPA3";
    }
    if (caps.find("RSN") != std::string::npos) {
        return "WPA2";
    }
    if (caps.find("WPA") != std::string::npos) {
        return "WPA";
    }
    if (caps.find("EP") != std::string::npos ||
        caps.find("PRIVACY") != std::string::npos) {
        return "WEP";
    }
    return "开放";
}

bool IsSecured(const std::string& security)
{
    return security != "开放";
}

void SplitWhitespace(const std::string& text, std::vector<std::string>& out)
{
    size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
            ++at;
        }
        size_t end = at;
        while (end < text.size() && text[end] != ' ' && text[end] != '\t') {
            ++end;
        }
        if (end > at) {
            out.push_back(text.substr(at, end - at));
        }
        at = end;
    }
}

}  // namespace

std::string QuoteWpaValue(const std::string& value)
{
    // wpa_cli's own parser reads these, not a shell, but it understands the
    // same backslash escapes inside double quotes.
    std::string quoted = "\"";
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            quoted += '\\';
        }
        quoted += ch;
    }
    quoted += '"';
    return quoted;
}

Wifi::Wifi()
{
    // The first wireless interface.  A machine can have more than one; the
    // window is about the one that is up, which is the first the system
    // listed.
    const std::string interfaces = pollux::Run("ifconfig -l");
    size_t start = 0;
    while (start < interfaces.size()) {
        size_t end = interfaces.find_first_of(" \t\n", start);
        if (end == std::string::npos) {
            end = interfaces.size();
        }
        const std::string name = interfaces.substr(start, end - start);
        if (name.compare(0, 4, "wlan") == 0) {
            m_interface = name;
            break;
        }
        start = end + 1;
    }
}

bool Wifi::HaveControlInterface() const
{
    if (m_interface.empty()) {
        return false;
    }
    const std::string path = "/var/run/wpa_supplicant/" + m_interface;
    struct stat info;
    if (::stat(path.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode)) {
        return false;
    }
    // The socket exists; whether this user may use it is a permission
    // question, and that is what a failed command reports.
    return ::access(path.c_str(), R_OK | W_OK) == 0;
}

bool Wifi::WpaCli(const std::string& command, std::string& response, std::string& error)
{
    response.clear();
    if (m_interface.empty()) {
        error = "没有无线网卡";
        return false;
    }

    int toChild[2] = { -1, -1 };
    int fromChild[2] = { -1, -1 };
    if (::pipe(toChild) != 0 || ::pipe(fromChild) != 0) {
        error = "pipe() 失败";
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(toChild[0]); ::close(toChild[1]);
        ::close(fromChild[0]); ::close(fromChild[1]);
        error = "fork() 失败";
        return false;
    }
    if (pid == 0) {
        ::dup2(toChild[0], STDIN_FILENO);
        ::dup2(fromChild[1], STDOUT_FILENO);
        ::dup2(fromChild[1], STDERR_FILENO);
        ::close(toChild[0]); ::close(toChild[1]);
        ::close(fromChild[0]); ::close(fromChild[1]);
        ::execl(kWpaCli, "wpa_cli", "-i", m_interface.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(toChild[0]);
    ::close(fromChild[1]);

    // The command goes in on standard input, so a password never shows up in
    // the process table the way it would as an argument.
    const std::string script = command + "\nquit\n";
    ssize_t written = ::write(toChild[1], script.data(), script.size());
    (void)written;
    ::close(toChild[1]);

    char buffer[512];
    ssize_t count = 0;
    while ((count = ::read(fromChild[0], buffer, sizeof(buffer))) > 0) {
        response.append(buffer, static_cast<size_t>(count));
    }
    ::close(fromChild[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);

    response = CleanReply(response);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        error = "找不到 wpa_cli";
        return false;
    }
    if (response.empty()) {
        error = "wpa_supplicant 没有回应";
        return false;
    }
    return true;
}

int Wifi::FindNetwork(const std::string& ssid)
{
    std::string response, error;
    if (!WpaCli("list_networks", response, error)) {
        return -1;
    }
    size_t start = 0;
    while (start < response.size()) {
        size_t end = response.find('\n', start);
        if (end == std::string::npos) {
            end = response.size();
        }
        const std::string line = response.substr(start, end - start);
        // "id<TAB>ssid<TAB>bssid<TAB>flags"; the first line is a heading.
        const size_t firstTab = line.find('\t');
        const size_t secondTab = firstTab == std::string::npos
                                     ? std::string::npos
                                     : line.find('\t', firstTab + 1);
        if (firstTab != std::string::npos && secondTab != std::string::npos) {
            const std::string idText = line.substr(0, firstTab);
            const std::string name = line.substr(firstTab + 1, secondTab - firstTab - 1);
            if (name == ssid && !idText.empty() &&
                idText.find_first_not_of("0123456789") == std::string::npos) {
                return std::atoi(idText.c_str());
            }
        }
        start = end + 1;
    }
    return -1;
}

bool Wifi::Scan(std::vector<Network>& out, std::string& error)
{
    out.clear();
    if (m_interface.empty()) {
        error = "没有无线网卡";
        return false;
    }

    const std::string listing =
        pollux::Run(("ifconfig " + m_interface + " list scan").c_str());
    if (listing.empty()) {
        error = "扫描没有返回结果";
        return false;
    }

    size_t start = 0;
    ScanColumns columns;
    while (start < listing.size()) {
        size_t end = listing.find('\n', start);
        if (end == std::string::npos) {
            end = listing.size();
        }
        const std::string line = listing.substr(start, end - start);
        start = end + 1;

        if (line.find("SSID/MESH ID") != std::string::npos) {
            columns = FindScanColumns(line);
            continue;
        }
        if (!columns.valid || line.empty()) {
            continue;
        }

        Network network;
        network.ssid = Slice(line, columns.ssid, columns.bssid);
        if (network.ssid.empty()) {
            continue;   // a hidden network has no name to show or join
        }

        // Everything after the SSID column is whitespace-separated and none of
        // it contains a space: bssid, channel, rate, signal:noise, the beacon
        // interval, then the capability flags.  These are read as fields
        // rather than by the header's columns because the columns do not
        // actually line up -- the signal value sits two characters left of its
        // heading, and slicing there produced "-96" for a signal of -50.
        std::vector<std::string> fields;
        SplitWhitespace(line.substr(columns.bssid), fields);
        if (fields.size() < 5) {
            continue;
        }
        network.bssid = fields[0];
        network.channel = std::atoi(fields[1].c_str());
        const size_t colon = fields[3].find(':');
        network.signalDbm = std::atoi(fields[3].substr(0, colon).c_str());

        std::string caps;
        for (size_t i = 5; i < fields.size(); ++i) {
            caps += fields[i];
            caps += ' ';
        }
        network.security = SecurityName(caps);
        network.secured = IsSecured(network.security);

        // The same name can be advertised by several access points; keep the
        // one with the strongest signal.
        bool merged = false;
        for (Network& existing : out) {
            if (existing.ssid == network.ssid) {
                if (network.signalDbm > existing.signalDbm) {
                    existing = network;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            out.push_back(network);
        }
    }

    if (out.empty()) {
        error = "没有扫描到网络";
        return false;
    }

    // Mark the saved ones so the window can tell "join" from "update".
    std::string response, listError;
    if (HaveControlInterface() && WpaCli("list_networks", response, listError)) {
        size_t lineStart = 0;
        while (lineStart < response.size()) {
            size_t lineEnd = response.find('\n', lineStart);
            if (lineEnd == std::string::npos) {
                lineEnd = response.size();
            }
            const std::string line = response.substr(lineStart, lineEnd - lineStart);
            const size_t firstTab = line.find('\t');
            const size_t secondTab = firstTab == std::string::npos
                                         ? std::string::npos
                                         : line.find('\t', firstTab + 1);
            if (firstTab != std::string::npos && secondTab != std::string::npos) {
                const std::string name = line.substr(firstTab + 1, secondTab - firstTab - 1);
                for (Network& network : out) {
                    if (network.ssid == name) {
                        network.saved = true;
                        network.networkId = std::atoi(line.substr(0, firstTab).c_str());
                    }
                }
            }
            lineStart = lineEnd + 1;
        }
    }

    // Strongest first, which is the order a person looks for their own network
    // in.
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        for (size_t j = i + 1; j < out.size(); ++j) {
            if (out[j].signalDbm > out[i].signalDbm) {
                std::swap(out[i], out[j]);
            }
        }
    }
    return true;
}

void Wifi::ReadStatus(Status& out)
{
    out = Status();
    out.haveControl = HaveControlInterface();
    if (m_interface.empty()) {
        return;
    }

    const std::string config = pollux::Run(("ifconfig " + m_interface).c_str());

    // The association line reads:
    //     ssid Redmi channel 11 (2462 MHz 11g) bssid ...
    const size_t ssidAt = config.find("ssid ");
    if (ssidAt != std::string::npos) {
        const size_t nameAt = ssidAt + 5;
        const size_t nameEnd = config.find(" channel ", nameAt);
        if (nameEnd != std::string::npos) {
            out.ssid = config.substr(nameAt, nameEnd - nameAt);
        }
        const size_t bssidAt = config.find("bssid ", nameAt);
        if (bssidAt != std::string::npos) {
            out.bssid = config.substr(bssidAt + 6, 17);
        }
    }

    const size_t inetAt = config.find("inet ");
    if (inetAt != std::string::npos) {
        const size_t ipAt = inetAt + 5;
        const size_t ipEnd = config.find_first_of(" \t\n", ipAt);
        out.ipAddress = config.substr(ipAt, ipEnd - ipAt);
    }
    out.connected = !out.ssid.empty() && !out.ipAddress.empty();

    if (!out.haveControl) {
        return;
    }

    std::string response, error;
    if (!WpaCli("status", response, error)) {
        return;
    }
    size_t start = 0;
    while (start < response.size()) {
        size_t end = response.find('\n', start);
        if (end == std::string::npos) {
            end = response.size();
        }
        const std::string line = response.substr(start, end - start);
        const size_t equals = line.find('=');
        if (equals != std::string::npos) {
            const std::string key = line.substr(0, equals);
            const std::string value = line.substr(equals + 1);
            if (key == "wpa_state") {
                out.state = value;
            } else if (key == "ssid" && out.ssid.empty()) {
                out.ssid = value;
            } else if (key == "bssid" && out.bssid.empty()) {
                out.bssid = value;
            } else if (key == "ip_address" && out.ipAddress.empty()) {
                out.ipAddress = value;
            }
        }
        start = end + 1;
    }
    if (out.state == "COMPLETED" && !out.ssid.empty() && !out.ipAddress.empty()) {
        out.connected = true;
    }

    std::string signalResponse;
    if (WpaCli("signal_poll", signalResponse, error)) {
        const size_t rssiAt = signalResponse.find("RSSI=");
        if (rssiAt != std::string::npos) {
            out.signalDbm = std::atoi(signalResponse.substr(rssiAt + 5).c_str());
        }
    }
}

bool Wifi::Connect(const std::string& ssid, const std::string& psk, bool secured,
                   std::string& error)
{
    if (!HaveControlInterface()) {
        error = "wpa_supplicant 控制接口不可用";
        return false;
    }
    if (ssid.empty()) {
        error = "网络名为空";
        return false;
    }

    std::string response;
    int id = FindNetwork(ssid);
    if (id < 0) {
        if (secured && psk.empty()) {
            error = "这个网络需要密码";
            return false;
        }
        if (!WpaCli("add_network", response, error)) {
            return false;
        }
        id = std::atoi(response.c_str());
        if (id < 0) {
            error = "add_network 没有返回编号：" + response;
            return false;
        }
        if (!WpaCli("set_network " + std::to_string(id) + " ssid " +
                        QuoteWpaValue(ssid), response, error)) {
            return false;
        }
        if (response != "OK") {
            error = response;
            return false;
        }
    }

    if (!psk.empty()) {
        // Setting the key is what makes this the "the password changed" path
        // as well as the "first time" one: wpa_supplicant replaces the stored
        // one.
        if (!WpaCli("set_network " + std::to_string(id) + " psk " +
                        QuoteWpaValue(psk), response, error)) {
            return false;
        }
        if (response != "OK") {
            error = response;
            return false;
        }
    } else if (!secured) {
        // An open network: no key at all, and the key management says so.
        WpaCli("set_network " + std::to_string(id) + " key_mgmt NONE", response, error);
    }

    // Enable first, then select: selecting a network disables the others, so
    // without the enable a later switch back to a network used before would
    // find it switched off.
    WpaCli("enable_network " + std::to_string(id), response, error);
    if (!WpaCli("select_network " + std::to_string(id), response, error)) {
        return false;
    }
    if (response != "OK") {
        error = response;
        return false;
    }
    WpaCli("save_config", response, error);
    return true;
}

bool Wifi::Disconnect(std::string& error)
{
    if (!HaveControlInterface()) {
        error = "wpa_supplicant 控制接口不可用";
        return false;
    }
    std::string response;
    if (!WpaCli("disconnect", response, error)) {
        return false;
    }
    if (response != "OK") {
        error = response;
        return false;
    }
    return true;
}

bool Wifi::Forget(const std::string& ssid, std::string& error)
{
    if (!HaveControlInterface()) {
        error = "wpa_supplicant 控制接口不可用";
        return false;
    }
    const int id = FindNetwork(ssid);
    if (id < 0) {
        error = "没有保存过这个网络";
        return false;
    }
    std::string response;
    if (!WpaCli("remove_network " + std::to_string(id), response, error)) {
        return false;
    }
    if (response != "OK") {
        error = response;
        return false;
    }
    WpaCli("save_config", response, error);
    return true;
}

}  // namespace wifi
