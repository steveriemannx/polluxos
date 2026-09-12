#ifndef EXAMPLES_POLLUXDESK_WIFI_WIFI_DATA_H_
#define EXAMPLES_POLLUXDESK_WIFI_WIFI_DATA_H_

// Wi-Fi readings and commands.
//
// Two programs do the work.  Scanning is `ifconfig <iface> list scan`, which a
// plain user may run.  Everything that changes the configuration goes through
// `wpa_cli`, the control program for the running wpa_supplicant -- which is
// also why none of this needs root:
//
//     wpa_supplicant runs as root and owns the radio.  wpa_cli talks to it over
//     a unix socket in /var/run/wpa_supplicant, and access to that socket is a
//     matter of file permissions, not privilege.  So the app asks
//     wpa_supplicant to join a network, and wpa_supplicant does the privileged
//     part.  This is the same split NetworkManager and friends use.
//
// The socket only exists if wpa_supplicant was started with a ctrl_interface
// configured; the FreeBSD way of starting it from rc.conf does not do that by
// default.  HaveControlInterface() reports whether it is there, and the window
// says so plainly when it is not.
//
// No dui types here, so the whole layer can be exercised by a small program on
// its own.

#include <string>
#include <vector>

namespace wifi {

/** One access point as the scan reported it. */
struct Network
{
    std::string ssid;
    std::string bssid;
    int  channel = 0;
    int  signalDbm = 0;         // -50 is a good signal, -85 is a bad one
    bool secured = false;       // WPA/WPA2/WPA3, or WEP
    std::string security;       // "WPA2", "WPA3", "WPA", "WEP", "开放"
    bool saved = false;         // there is a network block for it already
    int  networkId = -1;        // its wpa_supplicant id when saved
    bool connected = false;     // this is the one we are on
};

/** What the interface is doing right now. */
struct Status
{
    bool connected = false;
    bool haveControl = false;   // the wpa_supplicant control socket is usable
    std::string ssid;
    std::string ipAddress;
    std::string bssid;
    std::string state;          // COMPLETED, 4WAY_HANDSHAKE, DISCONNECTED, ...
    int  signalDbm = 0;
};

class Wifi
{
public:
    Wifi();

    /** The wireless interface to work on: the first wlan* device, or empty
     *  when the machine has none. */
    const std::string& Interface() const { return m_interface; }

    /** True when wpa_cli can talk to wpa_supplicant, which is what decides
     *  whether the window can change anything or only report. */
    bool HaveControlInterface() const;

    /** Access points in range, strongest first.  Reading a scan is allowed
     *  without privilege; it takes a moment, so callers run it off the
     *  refresh path that paints the window. */
    bool Scan(std::vector<Network>& out, std::string& error);

    /** Current connection, and the saved networks the scan turned up. */
    void ReadStatus(Status& out);

    /** Join a network, creating or updating its entry.
     *
     *  An empty @p psk means "use the key already stored", which is what makes
     *  reconnecting to a saved network work without asking for the password
     *  again; for a network that is not saved yet, a secured one needs one.
     *  A false return means the request could not be made at all; a true one
     *  means wpa_supplicant accepted it, and the caller has to watch
     *  Status::state to see whether the handshake then succeeded. */
    bool Connect(const std::string& ssid, const std::string& psk, bool secured,
                 std::string& error);

    /** Drop the current connection.  The interface stays up but loses its
     *  address, so on a machine reached over Wi-Fi this is the same as pulling
     *  the cable. */
    bool Disconnect(std::string& error);

    /** Forget a saved network. */
    bool Forget(const std::string& ssid, std::string& error);

private:
    /** Run one wpa_cli command and return its reply.  The command goes in on
     *  the child's standard input rather than on its command line, so a
     *  password never appears in the process table. */
    bool WpaCli(const std::string& command, std::string& response, std::string& error);

    /** The wpa_supplicant network id for an SSID, or -1. */
    int FindNetwork(const std::string& ssid);

    std::string m_interface;
};

/** The SSID and key of a saved network, quoted the way wpa_cli expects. */
std::string QuoteWpaValue(const std::string& value);

}  // namespace wifi

#endif  // EXAMPLES_POLLUXDESK_WIFI_WIFI_DATA_H_
