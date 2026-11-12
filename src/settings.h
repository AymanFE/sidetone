#pragma once
#include <string>

struct Settings {
    std::string backend = "Auto";
    std::string input;   // "" = system default
    std::string output;  // "" = system default
    int sampleRate = 48000;
    int periodFrames = 64;
    bool exclusiveIn = false;
    bool exclusiveOut = false;
    bool directEngine = true;
    std::string shareMicTo;
    int safetyFrames = 16;

    float gainDb = 0.0f;
    bool muted = false;
    float pan = 0.0f;
    int chanMode = 0;
    bool hpf = true;
    float hpfHz = 80.0f;
    bool gate = false;
    float gateDb = -50.0f;

    bool startOnLaunch = false;
    bool alwaysOnTop = false;
    bool autoReconnect = true;

    std::string serialize() const;
    void parse(const std::string& text);
    bool load();
    bool save() const;
    static std::string path();
};
