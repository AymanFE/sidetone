#include "settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static fs::path configDir() {
#if defined(_WIN32)
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    fs::path base = appdata ? fs::path(appdata) : fs::current_path();
    return base / "Sidetone";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Library" / "Application Support" / "Sidetone";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "sidetone";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".config" / "sidetone";
#endif
}

std::string Settings::path() { return (configDir() / "settings.ini").u8string(); }

std::string Settings::serialize() const {
    std::ostringstream o;
    o << "backend=" << backend << "\n"
      << "input=" << input << "\n"
      << "output=" << output << "\n"
      << "sample_rate=" << sampleRate << "\n"
      << "period_frames=" << periodFrames << "\n"
      << "exclusive_in=" << exclusiveIn << "\n"
      << "exclusive_out=" << exclusiveOut << "\n"
      << "direct_engine=" << directEngine << "\n"
      << "share_mic_to=" << shareMicTo << "\n"
      << "apps_from=" << appsFrom << "\n"
      << "apps_gain_db=" << appsGainDb << "\n"
      << "apps_muted=" << appsMuted << "\n"
      << "safety_frames=" << safetyFrames << "\n"
      << "gain_db=" << gainDb << "\n"
      << "muted=" << muted << "\n"
      << "pan=" << pan << "\n"
      << "channel=" << chanMode << "\n"
      << "hpf=" << hpf << "\n"
      << "hpf_hz=" << hpfHz << "\n"
      << "gate=" << gate << "\n"
      << "gate_db=" << gateDb << "\n"
      << "start_on_launch=" << startOnLaunch << "\n"
      << "always_on_top=" << alwaysOnTop << "\n"
      << "auto_reconnect=" << autoReconnect << "\n";
    return o.str();
}

void Settings::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto I = [&](int& x) { try { x = std::stoi(v); } catch (...) {} };
        auto F = [&](float& x) { try { x = std::stof(v); } catch (...) {} };
        auto B = [&](bool& x) { x = (v == "1" || v == "true"); };
        if (k == "backend") backend = v;
        else if (k == "input") input = v;
        else if (k == "output") output = v;
        else if (k == "sample_rate") I(sampleRate);
        else if (k == "period_frames") I(periodFrames);
        else if (k == "exclusive_in") B(exclusiveIn);
        else if (k == "exclusive_out") B(exclusiveOut);
        else if (k == "direct_engine") B(directEngine);
        else if (k == "share_mic_to") shareMicTo = v;
        else if (k == "apps_from") appsFrom = v;
        else if (k == "apps_gain_db") F(appsGainDb);
        else if (k == "apps_muted") B(appsMuted);
        else if (k == "safety_frames") I(safetyFrames);
        else if (k == "gain_db") F(gainDb);
        else if (k == "muted") B(muted);
        else if (k == "pan") F(pan);
        else if (k == "channel") I(chanMode);
        else if (k == "hpf") B(hpf);
        else if (k == "hpf_hz") F(hpfHz);
        else if (k == "gate") B(gate);
        else if (k == "gate_db") F(gateDb);
        else if (k == "start_on_launch") B(startOnLaunch);
        else if (k == "always_on_top") B(alwaysOnTop);
        else if (k == "auto_reconnect") B(autoReconnect);
    }
}

bool Settings::load() {
    std::ifstream f(configDir() / "settings.ini", std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    parse(ss.str());
    return true;
}

bool Settings::save() const {
    std::error_code ec;
    fs::create_directories(configDir(), ec);
    std::ofstream f(configDir() / "settings.ini", std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << serialize();
    return (bool)f;
}
