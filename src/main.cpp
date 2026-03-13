#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define STB_EASY_FONT_IMPLEMENTATION

#include "stb_easy_font.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"AccretionWindowClass";
constexpr wchar_t kWindowTitle[] = L"Project Accretion - Halley Range";
constexpr int kServerPort = 39015;
constexpr double kSimHz = 256.0;
constexpr double kFixedDt = 1.0 / kSimHz;
constexpr uint32_t kPacketMagic = 0x41434352u;
constexpr int kMaxDummies = 16;
constexpr int kHistorySize = 512;
constexpr int kCurrentSettingsVersion = 4;
constexpr float kPi = 3.14159265359f;
constexpr float kMaxShotDistance = 120.0f;

enum ButtonBits : uint8_t {
    Button_Jump = 1 << 0,
    Button_Crouch = 1 << 1,
    Button_Fire = 1 << 2,
    Button_Reload = 1 << 3,
};

enum class PacketType : uint16_t {
    Hello = 1,
    Input = 2,
    Snapshot = 3,
    HitConfirm = 4,
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Mat4 {
    float m[4][4] = {};
};

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct Aabb {
    Vec3 min;
    Vec3 max;
};

struct ResolutionOption {
    int width;
    int height;
    const char* label;
};

constexpr std::array<ResolutionOption, 4> kResolutions = {{
    {1280, 720, "1280 x 720"},
    {1600, 900, "1600 x 900"},
    {1920, 1080, "1920 x 1080"},
    {2560, 1440, "2560 x 1440"},
}};

constexpr std::array<int, 4> kFrameCaps = {{0, 120, 144, 240}};
constexpr std::array<const char*, 4> kFrameCapLabels = {{"Uncapped", "120", "144", "240"}};

float Clamp(float v, float min_v, float max_v) {
    return std::max(min_v, std::min(max_v, v));
}

float Saturate(float v) {
    return Clamp(v, 0.0f, 1.0f);
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

Vec2 operator+(const Vec2& a, const Vec2& b) {
    return {a.x + b.x, a.y + b.y};
}

Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(const Vec3& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}

Vec3 operator/(const Vec3& a, float s) {
    return {a.x / s, a.y / s, a.z / s};
}

float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float LengthSq(const Vec3& v) {
    return Dot(v, v);
}

float LengthSq(const Vec2& v) {
    return v.x * v.x + v.y * v.y;
}

float Length(const Vec3& v) {
    return std::sqrt(LengthSq(v));
}

float Length(const Vec2& v) {
    return std::sqrt(LengthSq(v));
}

Vec3 Normalize(const Vec3& v) {
    const float len = Length(v);
    if (len <= 0.00001f) {
        return {};
    }
    return v / len;
}

Vec2 Rotate(const Vec2& point, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {
        point.x * c - point.y * s,
        point.x * s + point.y * c,
    };
}

Mat4 Identity() {
    Mat4 m = {};
    m.m[0][0] = 1.0f;
    m.m[1][1] = 1.0f;
    m.m[2][2] = 1.0f;
    m.m[3][3] = 1.0f;
    return m;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 out = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] =
                a.m[r][0] * b.m[0][c] +
                a.m[r][1] * b.m[1][c] +
                a.m[r][2] * b.m[2][c] +
                a.m[r][3] * b.m[3][c];
        }
    }
    return out;
}

Mat4 Transpose(const Mat4& in) {
    Mat4 out = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] = in.m[c][r];
        }
    }
    return out;
}

Mat4 PerspectiveFovLh(float vertical_fov_radians, float aspect_ratio, float near_plane, float far_plane) {
    const float y_scale = 1.0f / std::tan(vertical_fov_radians * 0.5f);
    const float x_scale = y_scale / aspect_ratio;
    Mat4 m = {};
    m.m[0][0] = x_scale;
    m.m[1][1] = y_scale;
    m.m[2][2] = far_plane / (far_plane - near_plane);
    m.m[2][3] = 1.0f;
    m.m[3][2] = (-near_plane * far_plane) / (far_plane - near_plane);
    return m;
}

Mat4 LookAtLh(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 z_axis = Normalize(target - eye);
    const Vec3 x_axis = Normalize(Cross(up, z_axis));
    const Vec3 y_axis = Cross(z_axis, x_axis);

    Mat4 m = Identity();
    m.m[0][0] = x_axis.x;
    m.m[1][0] = x_axis.y;
    m.m[2][0] = x_axis.z;
    m.m[0][1] = y_axis.x;
    m.m[1][1] = y_axis.y;
    m.m[2][1] = y_axis.z;
    m.m[0][2] = z_axis.x;
    m.m[1][2] = z_axis.y;
    m.m[2][2] = z_axis.z;
    m.m[3][0] = -Dot(x_axis, eye);
    m.m[3][1] = -Dot(y_axis, eye);
    m.m[3][2] = -Dot(z_axis, eye);
    return m;
}

Mat4 OrthoOffCenter(float left, float right, float bottom, float top) {
    Mat4 m = Identity();
    m.m[0][0] = 2.0f / (right - left);
    m.m[1][1] = 2.0f / (top - bottom);
    m.m[2][2] = 1.0f;
    m.m[3][0] = (left + right) / (left - right);
    m.m[3][1] = (top + bottom) / (bottom - top);
    return m;
}

float Radians(float degrees) {
    return degrees * (kPi / 180.0f);
}

Vec3 ForwardFromAngles(float yaw, float pitch) {
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    return Normalize({sy * cp, sp, cy * cp});
}

Vec3 RightFromYaw(float yaw) {
    return Normalize({std::cos(yaw), 0.0f, -std::sin(yaw)});
}

bool AabbOverlap(const Aabb& a, const Aabb& b) {
    return !(a.max.x <= b.min.x || a.min.x >= b.max.x ||
             a.max.y <= b.min.y || a.min.y >= b.max.y ||
             a.max.z <= b.min.z || a.min.z >= b.max.z);
}

bool RayIntersectAabb(const Vec3& origin, const Vec3& dir, const Aabb& box, float* out_t) {
    float tmin = 0.0f;
    float tmax = 10000.0f;

    const float epsilon = 0.00001f;
    auto axis = [&](float origin_axis, float dir_axis, float min_axis, float max_axis) -> bool {
        if (std::fabs(dir_axis) < epsilon) {
            return origin_axis >= min_axis && origin_axis <= max_axis;
        }
        float inv_d = 1.0f / dir_axis;
        float t1 = (min_axis - origin_axis) * inv_d;
        float t2 = (max_axis - origin_axis) * inv_d;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        return tmin <= tmax;
    };

    if (!axis(origin.x, dir.x, box.min.x, box.max.x)) {
        return false;
    }
    if (!axis(origin.y, dir.y, box.min.y, box.max.y)) {
        return false;
    }
    if (!axis(origin.z, dir.z, box.min.z, box.max.z)) {
        return false;
    }

    if (out_t) {
        *out_t = tmin;
    }
    return true;
}

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

double NowSeconds() {
    static LARGE_INTEGER frequency = {};
    static bool initialized = false;
    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        initialized = true;
    }
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

std::filesystem::path FindProjectRoot() {
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path current = std::filesystem::path(exe_path).parent_path();
    for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(current / "AGENTS.md") || std::filesystem::exists(current / ".git")) {
            return current;
        }
        if (!current.has_parent_path()) {
            break;
        }
        current = current.parent_path();
    }
    return std::filesystem::current_path();
}

struct AppPaths {
    std::filesystem::path project_root;
    std::filesystem::path config_dir;
    std::filesystem::path telemetry_dir;
};

AppPaths BuildAppPaths() {
    AppPaths paths = {};
    paths.project_root = FindProjectRoot();
    paths.config_dir = paths.project_root / "config";
    paths.telemetry_dir = paths.project_root / "telemetry";
    std::filesystem::create_directories(paths.config_dir);
    std::filesystem::create_directories(paths.telemetry_dir);
    return paths;
}

struct Settings {
    int version = 0;
    float mouse_sensitivity = 0.08f;
    bool raw_input = true;
    bool invert_y = false;
    float fov_horizontal = 103.0f;
    int resolution_index = 2;
    bool fullscreen = true;
    int frame_cap_index = 0;
    bool show_hitmarkers = true;
    float audio_master_volume = 0.85f;
    float audio_weapon_volume = 0.92f;
    float audio_movement_volume = 0.78f;
};

Settings LoadSettings(const AppPaths& paths) {
    Settings settings = {};
    const std::filesystem::path file_path = paths.config_dir / "settings.ini";
    std::ifstream file(file_path);
    if (!file.is_open()) {
        settings.version = kCurrentSettingsVersion;
        return settings;
    }

    std::string line;
    bool has_version = false;
    while (std::getline(file, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "settings_version") {
            settings.version = std::stoi(value);
            has_version = true;
        } else if (key == "mouse_sensitivity") {
            settings.mouse_sensitivity = std::stof(value);
        } else if (key == "raw_input") {
            settings.raw_input = value == "1";
        } else if (key == "invert_y") {
            settings.invert_y = value == "1";
        } else if (key == "fov_horizontal") {
            settings.fov_horizontal = Clamp(std::stof(value), 80.0f, 103.0f);
        } else if (key == "resolution_index") {
            settings.resolution_index = std::clamp(std::stoi(value), 0, static_cast<int>(kResolutions.size()) - 1);
        } else if (key == "fullscreen") {
            settings.fullscreen = value == "1";
        } else if (key == "frame_cap_index") {
            settings.frame_cap_index = std::clamp(std::stoi(value), 0, static_cast<int>(kFrameCaps.size()) - 1);
        } else if (key == "show_hitmarkers") {
            settings.show_hitmarkers = value == "1";
        } else if (key == "audio_master_volume") {
            settings.audio_master_volume = Saturate(std::stof(value));
        } else if (key == "audio_weapon_volume") {
            settings.audio_weapon_volume = Saturate(std::stof(value));
        } else if (key == "audio_movement_volume") {
            settings.audio_movement_volume = Saturate(std::stof(value));
        }
    }

    const int loaded_version = has_version ? settings.version : 0;
    if (!has_version || loaded_version < 3) {
        settings.fullscreen = true;
    }
    settings.version = kCurrentSettingsVersion;

    return settings;
}

void SaveSettings(const AppPaths& paths, const Settings& settings) {
    const std::filesystem::path file_path = paths.config_dir / "settings.ini";
    std::ofstream file(file_path, std::ios::trunc);
    if (!file.is_open()) {
        return;
    }
    file << "settings_version=" << settings.version << "\n";
    file << "mouse_sensitivity=" << settings.mouse_sensitivity << "\n";
    file << "raw_input=" << (settings.raw_input ? 1 : 0) << "\n";
    file << "invert_y=" << (settings.invert_y ? 1 : 0) << "\n";
    file << "fov_horizontal=" << settings.fov_horizontal << "\n";
    file << "resolution_index=" << settings.resolution_index << "\n";
    file << "fullscreen=" << (settings.fullscreen ? 1 : 0) << "\n";
    file << "frame_cap_index=" << settings.frame_cap_index << "\n";
    file << "show_hitmarkers=" << (settings.show_hitmarkers ? 1 : 0) << "\n";
    file << "audio_master_volume=" << settings.audio_master_volume << "\n";
    file << "audio_weapon_volume=" << settings.audio_weapon_volume << "\n";
    file << "audio_movement_volume=" << settings.audio_movement_volume << "\n";
}

enum class AudioClipId : size_t {
    Gunshot = 0,
    FootstepLeft,
    FootstepRight,
    Jump,
    Crouch,
    Count,
};

enum class AudioBus : uint8_t {
    Weapon,
    Movement,
};

struct AudioClip {
    WAVEFORMATEX format = {};
    std::vector<int16_t> samples;
    XAUDIO2_BUFFER buffer = {};
};

struct ActiveVoice {
    IXAudio2SourceVoice* voice = nullptr;
    AudioBus bus = AudioBus::Weapon;
    float gain = 1.0f;
};

float RandomSigned(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>((state >> 8) & 0xffffu) / 32767.5f - 1.0f;
}

AudioClip BuildMonoClip(const std::vector<float>& source, int sample_rate = 44100) {
    AudioClip clip = {};
    clip.format.wFormatTag = WAVE_FORMAT_PCM;
    clip.format.nChannels = 1;
    clip.format.nSamplesPerSec = sample_rate;
    clip.format.wBitsPerSample = 16;
    clip.format.nBlockAlign = static_cast<WORD>(clip.format.nChannels * clip.format.wBitsPerSample / 8);
    clip.format.nAvgBytesPerSec = clip.format.nSamplesPerSec * clip.format.nBlockAlign;
    clip.samples.resize(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        clip.samples[i] = static_cast<int16_t>(Clamp(source[i], -1.0f, 1.0f) * 32767.0f);
    }
    clip.buffer.AudioBytes = static_cast<UINT32>(clip.samples.size() * sizeof(int16_t));
    clip.buffer.pAudioData = reinterpret_cast<const BYTE*>(clip.samples.data());
    clip.buffer.Flags = XAUDIO2_END_OF_STREAM;
    return clip;
}

AudioClip BuildGunshotClip() {
    constexpr int sample_rate = 44100;
    const int sample_count = static_cast<int>(sample_rate * 0.18f);
    std::vector<float> samples(static_cast<size_t>(sample_count), 0.0f);
    uint32_t seed = 0x41434352u;
    float low_noise = 0.0f;
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        const float noise = RandomSigned(seed);
        low_noise = Lerp(low_noise, noise, 0.08f);
        const float crack = noise * std::exp(-70.0f * t) * 0.95f;
        const float body = low_noise * std::exp(-18.0f * t) * 0.38f;
        const float tone = std::sin(2.0f * kPi * 130.0f * t) * std::exp(-12.0f * t) * 0.30f;
        const float snap = std::sin(2.0f * kPi * 900.0f * t) * std::exp(-90.0f * t) * 0.18f;
        samples[static_cast<size_t>(i)] = crack + body + tone + snap;
    }
    return BuildMonoClip(samples, sample_rate);
}

AudioClip BuildFootstepClip(uint32_t seed, float tone_hz) {
    constexpr int sample_rate = 44100;
    const int sample_count = static_cast<int>(sample_rate * 0.11f);
    std::vector<float> samples(static_cast<size_t>(sample_count), 0.0f);
    float low_noise = 0.0f;
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        const float noise = RandomSigned(seed);
        low_noise = Lerp(low_noise, noise, 0.05f);
        const float heel = std::sin(2.0f * kPi * tone_hz * t) * std::exp(-26.0f * t) * 0.40f;
        const float thump = low_noise * std::exp(-17.0f * t) * 0.65f;
        const float grit = noise * std::exp(-38.0f * t) * 0.12f;
        samples[static_cast<size_t>(i)] = thump + heel + grit;
    }
    return BuildMonoClip(samples, sample_rate);
}

AudioClip BuildJumpClip() {
    constexpr int sample_rate = 44100;
    const int sample_count = static_cast<int>(sample_rate * 0.18f);
    std::vector<float> samples(static_cast<size_t>(sample_count), 0.0f);
    uint32_t seed = 0x9a41cc07u;
    float air = 0.0f;
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        const float noise = RandomSigned(seed);
        air = Lerp(air, noise, 0.22f);
        const float whoosh = air * std::exp(-12.0f * t) * 0.25f;
        const float chirp = std::sin(2.0f * kPi * (220.0f + 520.0f * t) * t) * std::exp(-10.0f * t) * 0.22f;
        const float thump = std::sin(2.0f * kPi * 96.0f * t) * std::exp(-18.0f * t) * 0.22f;
        samples[static_cast<size_t>(i)] = whoosh + chirp + thump;
    }
    return BuildMonoClip(samples, sample_rate);
}

AudioClip BuildCrouchClip() {
    constexpr int sample_rate = 44100;
    const int sample_count = static_cast<int>(sample_rate * 0.10f);
    std::vector<float> samples(static_cast<size_t>(sample_count), 0.0f);
    uint32_t seed = 0x71c0ff33u;
    float rustle = 0.0f;
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
        const float noise = RandomSigned(seed);
        rustle = Lerp(rustle, noise, 0.33f);
        const float cloth = rustle * std::exp(-24.0f * t) * 0.20f;
        const float tap = std::sin(2.0f * kPi * 78.0f * t) * std::exp(-28.0f * t) * 0.16f;
        samples[static_cast<size_t>(i)] = cloth + tap;
    }
    return BuildMonoClip(samples, sample_rate);
}

float BusVolume(const Settings& settings, AudioBus bus) {
    return bus == AudioBus::Weapon ? settings.audio_weapon_volume : settings.audio_movement_volume;
}

struct AudioEngine {
    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* mastering_voice = nullptr;
    std::array<AudioClip, static_cast<size_t>(AudioClipId::Count)> clips = {};
    std::vector<ActiveVoice> active_voices;

    bool Initialize();
    void Shutdown();
    void Update(const Settings& settings);
    void Play(AudioClipId clip_id, AudioBus bus, const Settings& settings, float gain = 1.0f, float pitch = 1.0f);
    bool IsAvailable() const {
        return xaudio != nullptr && mastering_voice != nullptr;
    }
};

bool AudioEngine::Initialize() {
    HRESULT hr = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        xaudio = nullptr;
        return false;
    }

    hr = xaudio->CreateMasteringVoice(&mastering_voice);
    if (FAILED(hr)) {
        SafeRelease(xaudio);
        mastering_voice = nullptr;
        return false;
    }

    clips[static_cast<size_t>(AudioClipId::Gunshot)] = BuildGunshotClip();
    clips[static_cast<size_t>(AudioClipId::FootstepLeft)] = BuildFootstepClip(0x0f00ba11u, 118.0f);
    clips[static_cast<size_t>(AudioClipId::FootstepRight)] = BuildFootstepClip(0x13572468u, 132.0f);
    clips[static_cast<size_t>(AudioClipId::Jump)] = BuildJumpClip();
    clips[static_cast<size_t>(AudioClipId::Crouch)] = BuildCrouchClip();
    return true;
}

void AudioEngine::Shutdown() {
    for (ActiveVoice& active : active_voices) {
        if (active.voice) {
            active.voice->Stop(0);
            active.voice->DestroyVoice();
            active.voice = nullptr;
        }
    }
    active_voices.clear();
    if (mastering_voice) {
        mastering_voice->DestroyVoice();
        mastering_voice = nullptr;
    }
    SafeRelease(xaudio);
}

void AudioEngine::Update(const Settings& settings) {
    if (!IsAvailable()) {
        return;
    }

    mastering_voice->SetVolume(Saturate(settings.audio_master_volume));
    for (size_t i = 0; i < active_voices.size();) {
        ActiveVoice& active = active_voices[i];
        XAUDIO2_VOICE_STATE state = {};
        active.voice->GetState(&state);
        if (state.BuffersQueued == 0) {
            active.voice->DestroyVoice();
            active_voices.erase(active_voices.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        active.voice->SetVolume(active.gain * BusVolume(settings, active.bus));
        ++i;
    }
}

void AudioEngine::Play(AudioClipId clip_id, AudioBus bus, const Settings& settings, float gain, float pitch) {
    if (!IsAvailable()) {
        return;
    }

    const AudioClip& clip = clips[static_cast<size_t>(clip_id)];
    if (clip.samples.empty()) {
        return;
    }

    IXAudio2SourceVoice* voice = nullptr;
    HRESULT hr = xaudio->CreateSourceVoice(&voice, &clip.format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, nullptr, nullptr);
    if (FAILED(hr) || !voice) {
        return;
    }

    XAUDIO2_BUFFER buffer = clip.buffer;
    buffer.pAudioData = reinterpret_cast<const BYTE*>(clip.samples.data());
    voice->SetVolume(Saturate(gain) * BusVolume(settings, bus));
    voice->SetFrequencyRatio(Clamp(pitch, 0.75f, 1.35f));
    hr = voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        voice->DestroyVoice();
        return;
    }
    hr = voice->Start(0);
    if (FAILED(hr)) {
        voice->DestroyVoice();
        return;
    }

    active_voices.push_back({voice, bus, Saturate(gain)});
}

struct TelemetryLogger {
    std::ofstream file;
    double last_flush_time = 0.0;

    bool Initialize(const AppPaths& paths) {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        char name[64] = {};
        std::snprintf(name, sizeof(name),
            "session_%04d%02d%02d_%02d%02d%02d.csv",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        file.open(paths.telemetry_dir / name, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        file << "kind,time,render_fps,frame_ms,server_tick_hz,server_jitter_ms,client_sim_hz,correction_cm,velocity,notes\n";
        return true;
    }

    void LogFrame(double time, double render_fps, double frame_ms, double server_tick_hz, double server_jitter_ms,
        double client_sim_hz, double correction_cm, double velocity, const std::string& notes) {
        if (!file.is_open()) {
            return;
        }
        if (time - last_flush_time < 0.25) {
            return;
        }
        last_flush_time = time;
        file << "frame," << time << "," << render_fps << "," << frame_ms << ","
             << server_tick_hz << "," << server_jitter_ms << "," << client_sim_hz << ","
             << correction_cm << "," << velocity << "," << notes << "\n";
    }

    void LogEvent(double time, const std::string& notes) {
        if (!file.is_open()) {
            return;
        }
        file << "event," << time << ",0,0,0,0,0,0,0," << notes << "\n";
    }
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint16_t type;
    uint16_t size;
};

struct HelloPacket {
    PacketHeader header;
    uint32_t version;
};

struct InputPacket {
    PacketHeader header;
    uint32_t sequence;
    double client_time;
    double estimated_server_time;
    float move_x;
    float move_y;
    float yaw;
    float pitch;
    uint8_t buttons;
};

struct NetPlayerState {
    float x;
    float y;
    float z;
    float vx;
    float vy;
    float vz;
    float yaw;
    float pitch;
    uint8_t crouched;
    uint8_t on_ground;
    int32_t health;
    int32_t ammo;
    uint8_t reloading;
    float reload_time_remaining;
    float shot_cooldown;
};

struct NetDummyState {
    uint32_t id;
    float x;
    float y;
    float z;
    uint8_t alive;
    int32_t health;
};

struct SnapshotPacket {
    PacketHeader header;
    uint32_t server_tick;
    uint32_t last_processed_input;
    double server_time;
    NetPlayerState player;
    uint32_t dummy_count;
    NetDummyState dummies[kMaxDummies];
};

struct HitConfirmPacket {
    PacketHeader header;
    uint32_t input_sequence;
    uint32_t dummy_id;
    int32_t damage;
    uint8_t headshot;
    uint8_t eliminated;
};
#pragma pack(pop)

template <typename T>
T MakePacket(PacketType type) {
    T packet = {};
    packet.header.magic = kPacketMagic;
    packet.header.type = static_cast<uint16_t>(type);
    packet.header.size = static_cast<uint16_t>(sizeof(T));
    return packet;
}

struct WeaponState {
    int ammo = 30;
    bool reloading = false;
    float reload_time_remaining = 0.0f;
    float shot_cooldown = 0.0f;
};

struct PlayerState {
    Vec3 position = {0.0f, 0.0f, 2.0f};
    Vec3 velocity = {};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool crouched = false;
    bool on_ground = true;
    int health = 200;
    WeaponState weapon = {};
};

struct DummyState {
    uint32_t id = 0;
    Vec3 spawn_position = {};
    Vec3 position = {};
    bool moving = false;
    bool loop_path = false;
    float movement_phase = 0.0f;
    int health = 200;
    bool alive = true;
    float respawn_time = 0.0f;
};

struct RenderDummy {
    uint32_t id = 0;
    Vec3 position = {};
    bool alive = false;
    int health = 200;
};

struct TraceDummy {
    uint32_t id = 0;
    Vec3 position = {};
    bool alive = false;
};

struct TraceHit {
    bool hit = false;
    bool hit_world = false;
    bool headshot = false;
    uint32_t dummy_id = 0;
    float distance = kMaxShotDistance;
    Vec3 point = {};
};

struct InputCommand {
    uint32_t sequence = 0;
    double client_time = 0.0;
    double estimated_server_time = 0.0;
    float move_x = 0.0f;
    float move_y = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    uint8_t buttons = 0;
};

struct PredictionEntry {
    InputCommand input = {};
    PlayerState state = {};
};

struct ServerDebugStats {
    std::atomic<int> tick_rate_x100 {0};
    std::atomic<int> jitter_us {0};
};

struct DummyHistoryFrame {
    double server_time = 0.0;
    std::array<NetDummyState, kMaxDummies> dummies = {};
    uint32_t dummy_count = 0;
};

struct WorldGeometry {
    std::vector<Aabb> solids;
};

WorldGeometry BuildWorldGeometry() {
    WorldGeometry world = {};
    const float room_half_width = 10.0f;
    const float room_length = 58.0f;
    const float wall_height = 6.0f;
    const float wall_thickness = 0.5f;

    world.solids.push_back({{-room_half_width, -1.0f, -2.0f}, {room_half_width, 0.0f, room_length}});
    world.solids.push_back({{-room_half_width - wall_thickness, 0.0f, -2.0f}, {-room_half_width, wall_height, room_length}});
    world.solids.push_back({{room_half_width, 0.0f, -2.0f}, {room_half_width + wall_thickness, wall_height, room_length}});
    world.solids.push_back({{-room_half_width - wall_thickness, 0.0f, -2.5f}, {room_half_width + wall_thickness, wall_height, -2.0f}});
    world.solids.push_back({{-room_half_width - wall_thickness, 0.0f, room_length}, {room_half_width + wall_thickness, wall_height, room_length + wall_thickness}});
    world.solids.push_back({{-6.5f, 0.0f, 14.0f}, {-2.5f, 1.5f, 18.0f}});
    world.solids.push_back({{-1.5f, 0.0f, 24.0f}, {1.5f, 1.8f, 25.5f}});
    world.solids.push_back({{4.5f, 0.0f, 38.0f}, {7.0f, 1.6f, 39.5f}});
    world.solids.push_back({{-8.0f, 0.0f, 46.0f}, {-5.5f, 1.2f, 49.0f}});
    return world;
}

std::vector<DummyState> BuildInitialDummies() {
    std::vector<DummyState> dummies;
    const std::array<float, 4> distances = {{10.0f, 20.0f, 35.0f, 50.0f}};
    uint32_t next_id = 1;
    for (size_t i = 0; i < distances.size(); ++i) {
        DummyState static_dummy = {};
        static_dummy.id = next_id++;
        static_dummy.spawn_position = {-2.0f, 0.0f, distances[i]};
        static_dummy.position = static_dummy.spawn_position;
        dummies.push_back(static_dummy);

        DummyState moving_dummy = {};
        moving_dummy.id = next_id++;
        moving_dummy.spawn_position = {2.5f, 0.0f, distances[i]};
        moving_dummy.position = moving_dummy.spawn_position;
        moving_dummy.moving = true;
        moving_dummy.loop_path = (i % 2) == 1;
        moving_dummy.movement_phase = static_cast<float>(i) * 1.3f;
        dummies.push_back(moving_dummy);
    }
    return dummies;
}

float PlayerHeight(bool crouched) {
    return crouched ? 1.25f : 1.8f;
}

float EyeHeight(bool crouched) {
    return crouched ? 1.05f : 1.56f;
}

float PlayerRadius() {
    return 0.35f;
}

Aabb MakePlayerAabb(bool crouched, const Vec3& position) {
    const float radius = PlayerRadius();
    const float height = PlayerHeight(crouched);
    return {
        {position.x - radius, position.y, position.z - radius},
        {position.x + radius, position.y + height, position.z + radius},
    };
}

Aabb MakeDummyBodyAabb(const Vec3& position) {
    return {
        {position.x - 0.35f, position.y + 0.0f, position.z - 0.35f},
        {position.x + 0.35f, position.y + 1.45f, position.z + 0.35f},
    };
}

Aabb MakeDummyHeadAabb(const Vec3& position) {
    return {
        {position.x - 0.20f, position.y + 1.45f, position.z - 0.20f},
        {position.x + 0.20f, position.y + 1.85f, position.z + 0.20f},
    };
}

TraceHit ResolveHitscanTrace(const Vec3& origin, const Vec3& dir, const WorldGeometry& world, const TraceDummy* dummies, uint32_t dummy_count) {
    TraceHit result = {};
    result.point = origin + dir * kMaxShotDistance;

    float best_t = kMaxShotDistance;
    for (const Aabb& solid : world.solids) {
        float solid_t = 0.0f;
        if (RayIntersectAabb(origin, dir, solid, &solid_t) && solid_t >= 0.0f && solid_t < best_t) {
            best_t = solid_t;
            result.hit = true;
            result.hit_world = true;
            result.headshot = false;
            result.dummy_id = 0;
        }
    }

    for (uint32_t i = 0; i < dummy_count; ++i) {
        const TraceDummy& dummy = dummies[i];
        if (!dummy.alive) {
            continue;
        }

        const Aabb head = MakeDummyHeadAabb(dummy.position);
        const Aabb body = MakeDummyBodyAabb(dummy.position);
        float head_t = 0.0f;
        float body_t = 0.0f;
        const bool hit_head = RayIntersectAabb(origin, dir, head, &head_t);
        const bool hit_body = RayIntersectAabb(origin, dir, body, &body_t);
        if (!hit_head && !hit_body) {
            continue;
        }

        const float candidate_t = hit_head && (!hit_body || head_t <= body_t) ? head_t : body_t;
        if (candidate_t < 0.0f || candidate_t >= best_t) {
            continue;
        }

        best_t = candidate_t;
        result.hit = true;
        result.hit_world = false;
        result.headshot = hit_head && (!hit_body || head_t <= body_t);
        result.dummy_id = dummy.id;
    }

    result.distance = best_t;
    result.point = origin + dir * best_t;
    return result;
}

bool CanStandAt(const Vec3& position, const WorldGeometry& world) {
    const Aabb standing_box = MakePlayerAabb(false, position);
    for (const Aabb& solid : world.solids) {
        if (AabbOverlap(standing_box, solid)) {
            return false;
        }
    }
    return true;
}

bool ResolvePlayerAxis(PlayerState& player, const WorldGeometry& world, int axis, float delta) {
    if (delta == 0.0f) {
        return false;
    }

    Vec3 new_position = player.position;
    if (axis == 0) {
        new_position.x += delta;
    } else if (axis == 1) {
        new_position.y += delta;
    } else {
        new_position.z += delta;
    }

    Aabb player_box = MakePlayerAabb(player.crouched, new_position);
    bool collided = false;
    for (const Aabb& solid : world.solids) {
        if (!AabbOverlap(player_box, solid)) {
            continue;
        }

        collided = true;
        if (axis == 0) {
            new_position.x = delta > 0.0f ? solid.min.x - PlayerRadius() : solid.max.x + PlayerRadius();
        } else if (axis == 1) {
            if (delta > 0.0f) {
                new_position.y = solid.min.y - PlayerHeight(player.crouched);
            } else {
                new_position.y = solid.max.y;
                player.on_ground = true;
            }
            player.velocity.y = 0.0f;
        } else {
            new_position.z = delta > 0.0f ? solid.min.z - PlayerRadius() : solid.max.z + PlayerRadius();
        }
        player_box = MakePlayerAabb(player.crouched, new_position);
    }

    player.position = new_position;
    return collided;
}

void SimulateDummies(std::vector<DummyState>& dummies, double server_time, double dt) {
    (void)dt;
    for (DummyState& dummy : dummies) {
        if (!dummy.alive) {
            dummy.respawn_time -= static_cast<float>(dt);
            if (dummy.respawn_time <= 0.0f) {
                dummy.alive = true;
                dummy.health = 200;
                dummy.position = dummy.spawn_position;
            }
            continue;
        }

        dummy.position = dummy.spawn_position;
        if (!dummy.moving) {
            continue;
        }

        const float t = static_cast<float>(server_time) + dummy.movement_phase;
        if (dummy.loop_path) {
            dummy.position.x += std::sin(t * 0.8f) * 1.8f;
            dummy.position.z += std::cos(t * 0.8f) * 1.1f;
        } else {
            dummy.position.x += std::sin(t * 1.6f) * 2.0f;
        }
    }
}

void FillNetPlayerState(const PlayerState& src, NetPlayerState& dst) {
    dst.x = src.position.x;
    dst.y = src.position.y;
    dst.z = src.position.z;
    dst.vx = src.velocity.x;
    dst.vy = src.velocity.y;
    dst.vz = src.velocity.z;
    dst.yaw = src.yaw;
    dst.pitch = src.pitch;
    dst.crouched = src.crouched ? 1 : 0;
    dst.on_ground = src.on_ground ? 1 : 0;
    dst.health = src.health;
    dst.ammo = src.weapon.ammo;
    dst.reloading = src.weapon.reloading ? 1 : 0;
    dst.reload_time_remaining = src.weapon.reload_time_remaining;
    dst.shot_cooldown = src.weapon.shot_cooldown;
}

PlayerState PlayerStateFromNet(const NetPlayerState& net) {
    PlayerState state = {};
    state.position = {net.x, net.y, net.z};
    state.velocity = {net.vx, net.vy, net.vz};
    state.yaw = net.yaw;
    state.pitch = net.pitch;
    state.crouched = net.crouched != 0;
    state.on_ground = net.on_ground != 0;
    state.health = net.health;
    state.weapon.ammo = net.ammo;
    state.weapon.reloading = net.reloading != 0;
    state.weapon.reload_time_remaining = net.reload_time_remaining;
    state.weapon.shot_cooldown = net.shot_cooldown;
    return state;
}

void FillNetDummyState(const DummyState& src, NetDummyState& dst) {
    dst.id = src.id;
    dst.x = src.position.x;
    dst.y = src.position.y;
    dst.z = src.position.z;
    dst.alive = src.alive ? 1 : 0;
    dst.health = src.health;
}

void ApplyMovement(PlayerState& state, const InputCommand& input, const WorldGeometry& world, double dt) {
    state.yaw = input.yaw;
    state.pitch = Clamp(input.pitch, Radians(-89.0f), Radians(89.0f));

    const bool wants_crouch = (input.buttons & Button_Crouch) != 0;
    if (wants_crouch) {
        state.crouched = true;
    } else if (state.crouched && CanStandAt(state.position, world)) {
        state.crouched = false;
    }

    const Vec3 forward = Normalize(Vec3{ForwardFromAngles(state.yaw, 0.0f).x, 0.0f, ForwardFromAngles(state.yaw, 0.0f).z});
    const Vec3 right = RightFromYaw(state.yaw);

    Vec3 wish_dir = forward * input.move_y + right * input.move_x;
    if (LengthSq(wish_dir) > 0.00001f) {
        wish_dir = Normalize(wish_dir);
    }

    const float max_speed = state.crouched ? 3.95f : 7.2f;
    const float ground_accel = 55.0f;
    const float air_accel = 18.0f;
    const float friction = 10.0f;
    const float gravity = 28.0f;
    const float jump_speed = 8.35f;

    Vec3 horizontal = {state.velocity.x, 0.0f, state.velocity.z};
    const Vec3 desired = wish_dir * max_speed;

    if (state.on_ground) {
        if (LengthSq(wish_dir) < 0.0001f) {
            const float speed = Length(horizontal);
            if (speed > 0.0f) {
                const float drop = speed * friction * static_cast<float>(dt);
                const float new_speed = std::max(0.0f, speed - drop);
                horizontal = speed > 0.0f ? horizontal * (new_speed / speed) : horizontal;
            }
        } else {
            horizontal.x = Lerp(horizontal.x, desired.x, Clamp(ground_accel * static_cast<float>(dt), 0.0f, 1.0f));
            horizontal.z = Lerp(horizontal.z, desired.z, Clamp(ground_accel * static_cast<float>(dt), 0.0f, 1.0f));
        }
        if ((input.buttons & Button_Jump) != 0) {
            state.velocity.y = jump_speed;
            state.on_ground = false;
        }
    } else {
        horizontal.x = Lerp(horizontal.x, desired.x, Clamp(air_accel * static_cast<float>(dt), 0.0f, 1.0f));
        horizontal.z = Lerp(horizontal.z, desired.z, Clamp(air_accel * static_cast<float>(dt), 0.0f, 1.0f));
        state.velocity.y -= gravity * static_cast<float>(dt);
    }

    state.velocity.x = horizontal.x;
    state.velocity.z = horizontal.z;

    state.on_ground = false;
    ResolvePlayerAxis(state, world, 0, state.velocity.x * static_cast<float>(dt));
    ResolvePlayerAxis(state, world, 2, state.velocity.z * static_cast<float>(dt));
    ResolvePlayerAxis(state, world, 1, state.velocity.y * static_cast<float>(dt));
    if (state.position.y <= 0.0f) {
        state.position.y = 0.0f;
        state.velocity.y = 0.0f;
        state.on_ground = true;
    }
    if (!state.crouched && !CanStandAt(state.position, world)) {
        state.crouched = true;
    }
}

bool ApplyWeaponPrediction(PlayerState& state, const InputCommand& input, double dt) {
    state.weapon.shot_cooldown = std::max(0.0f, state.weapon.shot_cooldown - static_cast<float>(dt));
    if (state.weapon.reloading) {
        state.weapon.reload_time_remaining -= static_cast<float>(dt);
        if (state.weapon.reload_time_remaining <= 0.0f) {
            state.weapon.reloading = false;
            state.weapon.ammo = 30;
        }
    }

    if ((input.buttons & Button_Reload) != 0 && !state.weapon.reloading && state.weapon.ammo < 30) {
        state.weapon.reloading = true;
        state.weapon.reload_time_remaining = 1.5f;
    }

    if ((input.buttons & Button_Fire) != 0 && !state.weapon.reloading && state.weapon.shot_cooldown <= 0.0f && state.weapon.ammo > 0) {
        state.weapon.ammo -= 1;
        state.weapon.shot_cooldown = 0.1f;
        return true;
    }

    return false;
}

struct ShotResolution {
    bool fired = false;
    bool hit = false;
    uint32_t dummy_id = 0;
    int damage = 0;
    bool headshot = false;
    bool eliminated = false;
};

ShotResolution ResolveServerShot(PlayerState& state, const InputCommand& input, const DummyHistoryFrame& history_frame,
    const WorldGeometry& world, std::vector<DummyState>& dummies) {
    ShotResolution resolution = {};
    resolution.fired = true;

    const Vec3 origin = state.position + Vec3{0.0f, EyeHeight(state.crouched), 0.0f};
    const Vec3 ray_dir = ForwardFromAngles(input.yaw, input.pitch);

    TraceDummy trace_dummies[kMaxDummies] = {};
    for (uint32_t i = 0; i < history_frame.dummy_count; ++i) {
        const NetDummyState& snapshot_dummy = history_frame.dummies[i];
        trace_dummies[i].id = snapshot_dummy.id;
        trace_dummies[i].position = {snapshot_dummy.x, snapshot_dummy.y, snapshot_dummy.z};
        trace_dummies[i].alive = snapshot_dummy.alive != 0;
    }

    const TraceHit trace = ResolveHitscanTrace(origin, ray_dir, world, trace_dummies, history_frame.dummy_count);
    if (!trace.hit || trace.hit_world || trace.dummy_id == 0) {
        return resolution;
    }

    DummyState* best_dummy = nullptr;
    for (DummyState& dummy : dummies) {
        if (dummy.id == trace.dummy_id) {
            best_dummy = &dummy;
            break;
        }
    }

    if (!best_dummy || !best_dummy->alive) {
        return resolution;
    }

    resolution.hit = true;
    resolution.dummy_id = best_dummy->id;
    resolution.headshot = trace.headshot;
    resolution.damage = trace.headshot ? 40 : 20;
    best_dummy->health -= resolution.damage;
    if (best_dummy->health <= 0) {
        best_dummy->alive = false;
        best_dummy->respawn_time = 1.25f;
        resolution.eliminated = true;
    }
    return resolution;
}

DummyHistoryFrame FindHistoryFrame(const std::array<DummyHistoryFrame, kHistorySize>& history, uint32_t history_count, double shot_server_time) {
    const uint32_t count = std::min<uint32_t>(history_count, kHistorySize);
    DummyHistoryFrame best = history[0];
    double best_delta = std::fabs(best.server_time - shot_server_time);
    for (uint32_t i = 0; i < count; ++i) {
        const DummyHistoryFrame& candidate = history[i];
        const double delta = std::fabs(candidate.server_time - shot_server_time);
        if (delta < best_delta) {
            best = candidate;
            best_delta = delta;
        }
    }
    return best;
}

struct ServerRuntime {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in client_address = {};
    bool has_client = false;
    std::thread thread;
    std::atomic<bool> running {false};
    std::mutex input_mutex;
    std::deque<InputCommand> input_queue;
    PlayerState player = {};
    std::vector<DummyState> dummies = BuildInitialDummies();
    std::array<DummyHistoryFrame, kHistorySize> history = {};
    uint32_t history_count = 0;
    uint32_t server_tick = 0;
    uint32_t last_processed_input = 0;
    InputCommand last_input = {};
    WorldGeometry world = BuildWorldGeometry();
    ServerDebugStats* debug_stats = nullptr;

    bool Initialize(ServerDebugStats* stats);
    void Shutdown();
    void ThreadMain();
    void ProcessIncoming();
    void Tick(double now, double dt);
    void RecordHistory(double server_time);
};

bool ServerRuntime::Initialize(ServerDebugStats* stats) {
    debug_stats = stats;
    socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        return false;
    }
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(kServerPort);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket);
        socket = INVALID_SOCKET;
        return false;
    }

    RecordHistory(NowSeconds());
    running = true;
    thread = std::thread([this]() { ThreadMain(); });
    return true;
}

void ServerRuntime::Shutdown() {
    running = false;
    if (thread.joinable()) {
        thread.join();
    }
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
        socket = INVALID_SOCKET;
    }
}

void ServerRuntime::ThreadMain() {
    const double dt = kFixedDt;
    double next_tick_time = NowSeconds();
    double stats_time = next_tick_time;
    int ticks_this_second = 0;
    double lateness_accumulator = 0.0;

    while (running) {
        ProcessIncoming();

        double now = NowSeconds();
        while (now >= next_tick_time) {
            const double tick_lateness = std::max(0.0, now - next_tick_time);
            Tick(next_tick_time, dt);
            next_tick_time += dt;
            ++ticks_this_second;
            lateness_accumulator += tick_lateness;
            now = NowSeconds();
        }

        if (now - stats_time >= 1.0) {
            if (debug_stats) {
                debug_stats->tick_rate_x100.store(static_cast<int>((ticks_this_second / (now - stats_time)) * 100.0));
                const double jitter_ms = ticks_this_second > 0 ? (lateness_accumulator / ticks_this_second) * 1000.0 : 0.0;
                debug_stats->jitter_us.store(static_cast<int>(jitter_ms * 1000.0));
            }
            stats_time = now;
            ticks_this_second = 0;
            lateness_accumulator = 0.0;
        }

        const double sleep_seconds = next_tick_time - now;
        if (sleep_seconds > 0.002) {
            Sleep(static_cast<DWORD>((sleep_seconds - 0.001) * 1000.0));
        } else {
            SwitchToThread();
        }
    }
}

void ServerRuntime::ProcessIncoming() {
    std::array<uint8_t, 1024> buffer = {};
    sockaddr_in from = {};
    int from_len = sizeof(from);
    while (true) {
        const int received = recvfrom(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received <= 0) {
            break;
        }
        if (received < static_cast<int>(sizeof(PacketHeader))) {
            continue;
        }
        const PacketHeader* header = reinterpret_cast<const PacketHeader*>(buffer.data());
        if (header->magic != kPacketMagic || header->size != received) {
            continue;
        }
        has_client = true;
        client_address = from;

        if (static_cast<PacketType>(header->type) == PacketType::Input) {
            const InputPacket* packet = reinterpret_cast<const InputPacket*>(buffer.data());
            InputCommand input = {};
            input.sequence = packet->sequence;
            input.client_time = packet->client_time;
            input.estimated_server_time = packet->estimated_server_time;
            input.move_x = packet->move_x;
            input.move_y = packet->move_y;
            input.yaw = packet->yaw;
            input.pitch = packet->pitch;
            input.buttons = packet->buttons;
            std::lock_guard<std::mutex> lock(input_mutex);
            input_queue.push_back(input);
            while (input_queue.size() > 256) {
                input_queue.pop_front();
            }
        }
    }
}

void ServerRuntime::Tick(double now, double dt) {
    InputCommand input_to_use = last_input;
    {
        std::lock_guard<std::mutex> lock(input_mutex);
        if (!input_queue.empty()) {
            input_to_use = input_queue.front();
            input_queue.pop_front();
        }
    }

    last_input = input_to_use;
    last_processed_input = input_to_use.sequence;

    ApplyMovement(player, input_to_use, world, dt);
    const bool fired = ApplyWeaponPrediction(player, input_to_use, dt);
    SimulateDummies(dummies, now, dt);
    RecordHistory(now);

    if (fired && has_client) {
        const DummyHistoryFrame frame = FindHistoryFrame(history, history_count,
            input_to_use.estimated_server_time > 0.0 ? input_to_use.estimated_server_time : now);
        const ShotResolution shot = ResolveServerShot(player, input_to_use, frame, world, dummies);
        if (shot.hit) {
            HitConfirmPacket packet = MakePacket<HitConfirmPacket>(PacketType::HitConfirm);
            packet.input_sequence = input_to_use.sequence;
            packet.dummy_id = shot.dummy_id;
            packet.damage = shot.damage;
            packet.headshot = shot.headshot ? 1 : 0;
            packet.eliminated = shot.eliminated ? 1 : 0;
            sendto(socket, reinterpret_cast<const char*>(&packet), sizeof(packet), 0,
                reinterpret_cast<sockaddr*>(&client_address), sizeof(client_address));
        }
    }

    if (has_client) {
        SnapshotPacket snapshot = MakePacket<SnapshotPacket>(PacketType::Snapshot);
        snapshot.server_tick = server_tick++;
        snapshot.last_processed_input = last_processed_input;
        snapshot.server_time = now;
        FillNetPlayerState(player, snapshot.player);
        snapshot.dummy_count = static_cast<uint32_t>(std::min<size_t>(dummies.size(), kMaxDummies));
        for (uint32_t i = 0; i < snapshot.dummy_count; ++i) {
            FillNetDummyState(dummies[i], snapshot.dummies[i]);
        }
        sendto(socket, reinterpret_cast<const char*>(&snapshot), sizeof(snapshot), 0,
            reinterpret_cast<sockaddr*>(&client_address), sizeof(client_address));
    }
}

void ServerRuntime::RecordHistory(double server_time) {
    DummyHistoryFrame frame = {};
    frame.server_time = server_time;
    frame.dummy_count = static_cast<uint32_t>(std::min<size_t>(dummies.size(), kMaxDummies));
    for (uint32_t i = 0; i < frame.dummy_count; ++i) {
        FillNetDummyState(dummies[i], frame.dummies[i]);
    }
    history[history_count % kHistorySize] = frame;
    ++history_count;
}

struct RenderVertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
    float a;
};

struct DynamicBuffer {
    ID3D11Buffer* buffer = nullptr;
    size_t capacity = 0;
};

struct EasyFontVertex {
    float x;
    float y;
    float z;
    unsigned char c[4];
};

using D3DCompileProc = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

struct Renderer {
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
    ID3D11DepthStencilView* depth_stencil_view = nullptr;
    ID3D11Texture2D* depth_texture = nullptr;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11InputLayout* input_layout = nullptr;
    ID3D11Buffer* constant_buffer = nullptr;
    ID3D11RasterizerState* rasterizer_state = nullptr;
    ID3D11DepthStencilState* depth_state_enabled = nullptr;
    ID3D11DepthStencilState* depth_state_disabled = nullptr;
    ID3D11BlendState* blend_state = nullptr;
    DynamicBuffer triangle_buffer_3d = {};
    DynamicBuffer line_buffer_3d = {};
    DynamicBuffer triangle_buffer_2d = {};
    DynamicBuffer line_buffer_2d = {};

    std::vector<RenderVertex> triangles_3d;
    std::vector<RenderVertex> lines_3d;
    std::vector<RenderVertex> triangles_2d;
    std::vector<RenderVertex> lines_2d;

    bool Initialize(HWND window_handle, int client_width, int client_height);
    bool CreateTargets();
    bool CreateShaders();
    bool CreateStates();
    void Shutdown();
    void Resize(int client_width, int client_height);
    void Clear();
    void BeginFrame(const Color& clear);
    void Present();
    void PushLine3D(const Vec3& a, const Vec3& b, const Color& color);
    void PushLine2D(float x0, float y0, float x1, float y1, const Color& color);
    void PushTriangle2D(const Vec2& a, const Vec2& b, const Vec2& c, const Color& color);
    void PushQuad2D(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d, const Color& color);
    void PushRect2D(float x, float y, float w, float h, const Color& color);
    void PushBoxSolid(const Aabb& box, const Color& color);
    void PushBoxWire(const Aabb& box, const Color& color);
    void PushText(float x, float y, const char* text, const Color& color);
    void DrawBuffer(DynamicBuffer& dynamic_buffer, const std::vector<RenderVertex>& vertices, D3D11_PRIMITIVE_TOPOLOGY topology, const Mat4& matrix, bool use_depth);
    void Flush(const Mat4& view_projection);
    bool CaptureBackBufferBmp(const std::filesystem::path& file_path);
};

bool Renderer::Initialize(HWND window_handle, int client_width, int client_height) {
    hwnd = window_handle;
    width = client_width;
    height = client_height;

    DXGI_SWAP_CHAIN_DESC swap_desc = {};
    swap_desc.BufferCount = 2;
    swap_desc.BufferDesc.Width = static_cast<UINT>(width);
    swap_desc.BufferDesc.Height = static_cast<UINT>(height);
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_desc.OutputWindow = hwnd;
        swap_desc.SampleDesc.Count = 1;
        swap_desc.Windowed = TRUE;
        swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        feature_levels, static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION,
        &swap_desc, &swap_chain, &device, &feature_level, &context);
    if (FAILED(hr)) {
        return false;
    }
    return CreateTargets() && CreateShaders() && CreateStates();
}

bool Renderer::CreateTargets() {
    SafeRelease(render_target);
    SafeRelease(depth_stencil_view);
    SafeRelease(depth_texture);

    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
    if (FAILED(hr)) {
        return false;
    }
    hr = device->CreateRenderTargetView(back_buffer, nullptr, &render_target);
    SafeRelease(back_buffer);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC depth_desc = {};
    depth_desc.Width = static_cast<UINT>(width);
    depth_desc.Height = static_cast<UINT>(height);
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = device->CreateTexture2D(&depth_desc, nullptr, &depth_texture);
    if (FAILED(hr)) {
        return false;
    }
    hr = device->CreateDepthStencilView(depth_texture, nullptr, &depth_stencil_view);
    return SUCCEEDED(hr);
}

bool Renderer::CreateShaders() {
    HMODULE compiler_module = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler_module) {
        compiler_module = LoadLibraryW(L"d3dcompiler_43.dll");
    }
    if (!compiler_module) {
        return false;
    }

    auto* compile = reinterpret_cast<D3DCompileProc>(GetProcAddress(compiler_module, "D3DCompile"));
    if (!compile) {
        return false;
    }

    const char* shader_source =
        "cbuffer Camera : register(b0) { float4x4 u_matrix; };"
        "struct VSIn { float3 pos : POSITION; float4 color : COLOR; };"
        "struct PSIn { float4 pos : SV_Position; float4 color : COLOR; };"
        "PSIn vs_main(VSIn input) { PSIn output; output.pos = mul(float4(input.pos, 1.0f), u_matrix); output.color = input.color; return output; }"
        "float4 ps_main(PSIn input) : SV_Target { return input.color; }";

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = compile(shader_source, std::strlen(shader_source), nullptr, nullptr, nullptr,
        "vs_main", "vs_4_0", 0, 0, &vs_blob, &error_blob);
    if (FAILED(hr)) {
        if (error_blob) {
            MessageBoxA(hwnd, static_cast<const char*>(error_blob->GetBufferPointer()), "Vertex Shader Error", MB_OK);
        }
        SafeRelease(error_blob);
        return false;
    }
    hr = compile(shader_source, std::strlen(shader_source), nullptr, nullptr, nullptr,
        "ps_main", "ps_4_0", 0, 0, &ps_blob, &error_blob);
    if (FAILED(hr)) {
        if (error_blob) {
            MessageBoxA(hwnd, static_cast<const char*>(error_blob->GetBufferPointer()), "Pixel Shader Error", MB_OK);
        }
        SafeRelease(vs_blob);
        SafeRelease(error_blob);
        return false;
    }

    hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader);
    if (FAILED(hr)) {
        SafeRelease(vs_blob);
        SafeRelease(ps_blob);
        return false;
    }
    hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &pixel_shader);
    if (FAILED(hr)) {
        SafeRelease(vs_blob);
        SafeRelease(ps_blob);
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)),
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &input_layout);
    SafeRelease(vs_blob);
    SafeRelease(ps_blob);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.ByteWidth = sizeof(Mat4);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&cb_desc, nullptr, &constant_buffer));
}

bool Renderer::CreateStates() {
    D3D11_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&raster_desc, &rasterizer_state))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth_enabled = {};
    depth_enabled.DepthEnable = TRUE;
    depth_enabled.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_enabled.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&depth_enabled, &depth_state_enabled))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth_disabled = {};
    depth_disabled.DepthEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depth_disabled, &depth_state_disabled))) {
        return false;
    }

    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device->CreateBlendState(&blend_desc, &blend_state));
}

void Renderer::Shutdown() {
    SafeRelease(line_buffer_2d.buffer);
    SafeRelease(triangle_buffer_2d.buffer);
    SafeRelease(line_buffer_3d.buffer);
    SafeRelease(triangle_buffer_3d.buffer);
    SafeRelease(blend_state);
    SafeRelease(depth_state_disabled);
    SafeRelease(depth_state_enabled);
    SafeRelease(rasterizer_state);
    SafeRelease(constant_buffer);
    SafeRelease(input_layout);
    SafeRelease(pixel_shader);
    SafeRelease(vertex_shader);
    SafeRelease(depth_stencil_view);
    SafeRelease(depth_texture);
    SafeRelease(render_target);
    SafeRelease(context);
    SafeRelease(device);
    SafeRelease(swap_chain);
}

void Renderer::Resize(int client_width, int client_height) {
    width = std::max(1, client_width);
    height = std::max(1, client_height);
    if (!swap_chain) {
        return;
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(render_target);
    SafeRelease(depth_stencil_view);
    SafeRelease(depth_texture);
    swap_chain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    CreateTargets();
}

void Renderer::Clear() {
    triangles_3d.clear();
    lines_3d.clear();
    triangles_2d.clear();
    lines_2d.clear();
}

void Renderer::BeginFrame(const Color& clear) {
    const float color[4] = {clear.r, clear.g, clear.b, clear.a};
    context->OMSetRenderTargets(1, &render_target, depth_stencil_view);
    context->ClearRenderTargetView(render_target, color);
    context->ClearDepthStencilView(depth_stencil_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->RSSetState(rasterizer_state);
    context->IASetInputLayout(input_layout);
    context->VSSetShader(vertex_shader, nullptr, 0);
    context->PSSetShader(pixel_shader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &constant_buffer);
}

void Renderer::Present() {
    swap_chain->Present(0, 0);
}

void Renderer::PushLine3D(const Vec3& a, const Vec3& b, const Color& color) {
    lines_3d.push_back({a.x, a.y, a.z, color.r, color.g, color.b, color.a});
    lines_3d.push_back({b.x, b.y, b.z, color.r, color.g, color.b, color.a});
}

void Renderer::PushLine2D(float x0, float y0, float x1, float y1, const Color& color) {
    lines_2d.push_back({x0, y0, 0.0f, color.r, color.g, color.b, color.a});
    lines_2d.push_back({x1, y1, 0.0f, color.r, color.g, color.b, color.a});
}

void Renderer::PushTriangle2D(const Vec2& a, const Vec2& b, const Vec2& c, const Color& color) {
    triangles_2d.push_back({a.x, a.y, 0.0f, color.r, color.g, color.b, color.a});
    triangles_2d.push_back({b.x, b.y, 0.0f, color.r, color.g, color.b, color.a});
    triangles_2d.push_back({c.x, c.y, 0.0f, color.r, color.g, color.b, color.a});
}

void Renderer::PushQuad2D(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d, const Color& color) {
    PushTriangle2D(a, b, c, color);
    PushTriangle2D(a, c, d, color);
}

void Renderer::PushRect2D(float x, float y, float w, float h, const Color& color) {
    const RenderVertex v0 = {x, y, 0.0f, color.r, color.g, color.b, color.a};
    const RenderVertex v1 = {x + w, y, 0.0f, color.r, color.g, color.b, color.a};
    const RenderVertex v2 = {x + w, y + h, 0.0f, color.r, color.g, color.b, color.a};
    const RenderVertex v3 = {x, y + h, 0.0f, color.r, color.g, color.b, color.a};
    triangles_2d.push_back(v0); triangles_2d.push_back(v1); triangles_2d.push_back(v2);
    triangles_2d.push_back(v0); triangles_2d.push_back(v2); triangles_2d.push_back(v3);
}

void Renderer::PushBoxSolid(const Aabb& box, const Color& color) {
    const Vec3 p000 = {box.min.x, box.min.y, box.min.z};
    const Vec3 p001 = {box.min.x, box.min.y, box.max.z};
    const Vec3 p010 = {box.min.x, box.max.y, box.min.z};
    const Vec3 p011 = {box.min.x, box.max.y, box.max.z};
    const Vec3 p100 = {box.max.x, box.min.y, box.min.z};
    const Vec3 p101 = {box.max.x, box.min.y, box.max.z};
    const Vec3 p110 = {box.max.x, box.max.y, box.min.z};
    const Vec3 p111 = {box.max.x, box.max.y, box.max.z};
    auto push_tri = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
        triangles_3d.push_back({a.x, a.y, a.z, color.r, color.g, color.b, color.a});
        triangles_3d.push_back({b.x, b.y, b.z, color.r, color.g, color.b, color.a});
        triangles_3d.push_back({c.x, c.y, c.z, color.r, color.g, color.b, color.a});
    };
    push_tri(p001, p101, p111); push_tri(p001, p111, p011);
    push_tri(p100, p000, p010); push_tri(p100, p010, p110);
    push_tri(p000, p001, p011); push_tri(p000, p011, p010);
    push_tri(p101, p100, p110); push_tri(p101, p110, p111);
    push_tri(p010, p011, p111); push_tri(p010, p111, p110);
    push_tri(p000, p100, p101); push_tri(p000, p101, p001);
}

void Renderer::PushBoxWire(const Aabb& box, const Color& color) {
    const Vec3 p000 = {box.min.x, box.min.y, box.min.z};
    const Vec3 p001 = {box.min.x, box.min.y, box.max.z};
    const Vec3 p010 = {box.min.x, box.max.y, box.min.z};
    const Vec3 p011 = {box.min.x, box.max.y, box.max.z};
    const Vec3 p100 = {box.max.x, box.min.y, box.min.z};
    const Vec3 p101 = {box.max.x, box.min.y, box.max.z};
    const Vec3 p110 = {box.max.x, box.max.y, box.min.z};
    const Vec3 p111 = {box.max.x, box.max.y, box.max.z};
    PushLine3D(p000, p001, color); PushLine3D(p001, p011, color); PushLine3D(p011, p010, color); PushLine3D(p010, p000, color);
    PushLine3D(p100, p101, color); PushLine3D(p101, p111, color); PushLine3D(p111, p110, color); PushLine3D(p110, p100, color);
    PushLine3D(p000, p100, color); PushLine3D(p001, p101, color); PushLine3D(p010, p110, color); PushLine3D(p011, p111, color);
}

void Renderer::PushText(float x, float y, const char* text, const Color& color) {
    unsigned char rgba[4] = {
        static_cast<unsigned char>(Clamp(color.r, 0.0f, 1.0f) * 255.0f),
        static_cast<unsigned char>(Clamp(color.g, 0.0f, 1.0f) * 255.0f),
        static_cast<unsigned char>(Clamp(color.b, 0.0f, 1.0f) * 255.0f),
        static_cast<unsigned char>(Clamp(color.a, 0.0f, 1.0f) * 255.0f),
    };
    EasyFontVertex vertices[4096];
    const int quads = stb_easy_font_print(x, y, const_cast<char*>(text), rgba, vertices, sizeof(vertices));
    for (int i = 0; i < quads; ++i) {
        const EasyFontVertex* v = &vertices[i * 4];
        const RenderVertex r0 = {v[0].x, v[0].y, 0.0f, v[0].c[0] / 255.0f, v[0].c[1] / 255.0f, v[0].c[2] / 255.0f, v[0].c[3] / 255.0f};
        const RenderVertex r1 = {v[1].x, v[1].y, 0.0f, v[1].c[0] / 255.0f, v[1].c[1] / 255.0f, v[1].c[2] / 255.0f, v[1].c[3] / 255.0f};
        const RenderVertex r2 = {v[2].x, v[2].y, 0.0f, v[2].c[0] / 255.0f, v[2].c[1] / 255.0f, v[2].c[2] / 255.0f, v[2].c[3] / 255.0f};
        const RenderVertex r3 = {v[3].x, v[3].y, 0.0f, v[3].c[0] / 255.0f, v[3].c[1] / 255.0f, v[3].c[2] / 255.0f, v[3].c[3] / 255.0f};
        triangles_2d.push_back(r0); triangles_2d.push_back(r1); triangles_2d.push_back(r2);
        triangles_2d.push_back(r0); triangles_2d.push_back(r2); triangles_2d.push_back(r3);
    }
}

void Renderer::DrawBuffer(DynamicBuffer& dynamic_buffer, const std::vector<RenderVertex>& vertices, D3D11_PRIMITIVE_TOPOLOGY topology, const Mat4& matrix, bool use_depth) {
    if (vertices.empty()) {
        return;
    }
    const size_t required_size = vertices.size() * sizeof(RenderVertex);
    if (!dynamic_buffer.buffer || dynamic_buffer.capacity < required_size) {
        SafeRelease(dynamic_buffer.buffer);
        D3D11_BUFFER_DESC desc = {};
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.ByteWidth = static_cast<UINT>(std::max<size_t>(required_size, 65536));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &dynamic_buffer.buffer))) {
            return;
        }
        dynamic_buffer.capacity = desc.ByteWidth;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(dynamic_buffer.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, vertices.data(), required_size);
    context->Unmap(dynamic_buffer.buffer, 0);

    D3D11_MAPPED_SUBRESOURCE cb_map = {};
    if (SUCCEEDED(context->Map(constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb_map))) {
        const Mat4 gpu_matrix = Transpose(matrix);
        std::memcpy(cb_map.pData, &gpu_matrix, sizeof(gpu_matrix));
        context->Unmap(constant_buffer, 0);
    }

    const UINT stride = sizeof(RenderVertex);
    const UINT offset = 0;
    context->IASetPrimitiveTopology(topology);
    context->IASetVertexBuffers(0, 1, &dynamic_buffer.buffer, &stride, &offset);
    context->OMSetDepthStencilState(use_depth ? depth_state_enabled : depth_state_disabled, 0);
    const float blend_factor[4] = {0, 0, 0, 0};
    context->OMSetBlendState(blend_state, blend_factor, 0xffffffffu);
    context->Draw(static_cast<UINT>(vertices.size()), 0);
}

void Renderer::Flush(const Mat4& view_projection) {
    DrawBuffer(triangle_buffer_3d, triangles_3d, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, view_projection, true);
    DrawBuffer(line_buffer_3d, lines_3d, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, view_projection, true);
    const Mat4 ortho = OrthoOffCenter(0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f);
    DrawBuffer(triangle_buffer_2d, triangles_2d, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, ortho, false);
    DrawBuffer(line_buffer_2d, lines_2d, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, ortho, false);
}

bool Renderer::CaptureBackBufferBmp(const std::filesystem::path& file_path) {
    if (!swap_chain || !device || !context) {
        return false;
    }

    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
    if (FAILED(hr)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    back_buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    ID3D11Texture2D* staging = nullptr;
    hr = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) {
        SafeRelease(back_buffer);
        return false;
    }

    context->CopyResource(staging, back_buffer);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        SafeRelease(staging);
        SafeRelease(back_buffer);
        return false;
    }

    const uint32_t output_width = desc.Width;
    const uint32_t output_height = desc.Height;
    const uint32_t row_stride = output_width * 3u;
    const uint32_t padded_row_stride = (row_stride + 3u) & ~3u;
    const uint32_t pixel_data_size = padded_row_stride * output_height;
    const uint32_t file_size = 54u + pixel_data_size;

    std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        context->Unmap(staging, 0);
        SafeRelease(staging);
        SafeRelease(back_buffer);
        return false;
    }

    auto write_u16 = [&](uint16_t value) {
        file.put(static_cast<char>(value & 0xff));
        file.put(static_cast<char>((value >> 8) & 0xff));
    };
    auto write_u32 = [&](uint32_t value) {
        file.put(static_cast<char>(value & 0xff));
        file.put(static_cast<char>((value >> 8) & 0xff));
        file.put(static_cast<char>((value >> 16) & 0xff));
        file.put(static_cast<char>((value >> 24) & 0xff));
    };

    write_u16(0x4d42);
    write_u32(file_size);
    write_u16(0);
    write_u16(0);
    write_u32(54);
    write_u32(40);
    write_u32(output_width);
    write_u32(output_height);
    write_u16(1);
    write_u16(24);
    write_u32(0);
    write_u32(pixel_data_size);
    write_u32(2835);
    write_u32(2835);
    write_u32(0);
    write_u32(0);

    std::vector<uint8_t> row(padded_row_stride, 0);
    for (int y = static_cast<int>(output_height) - 1; y >= 0; --y) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (uint32_t x = 0; x < output_width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        file.write(reinterpret_cast<const char*>(row.data()), padded_row_stride);
    }

    context->Unmap(staging, 0);
    SafeRelease(staging);
    SafeRelease(back_buffer);
    return file.good();
}

struct PlatformState {
    HWND hwnd = nullptr;
    bool running = true;
    bool focused = true;
    bool gameplay_focus = false;
    bool pending_gameplay_focus = false;
    bool mouse_captured = false;
    bool resized = false;
    int client_width = 1600;
    int client_height = 900;
    std::array<bool, 256> keys = {};
    std::array<bool, 256> prev_keys = {};
    bool left_mouse_down = false;
    bool prev_left_mouse_down = false;
    float mouse_dx = 0.0f;
    float mouse_dy = 0.0f;
    POINT last_cursor = {};
    bool cursor_valid = false;
    double ignore_mouse_until = 0.0;
    bool fullscreen = false;
    RECT windowed_rect = {0, 0, 1600, 900};
    DWORD windowed_style = WS_OVERLAPPEDWINDOW;
};

PlatformState* g_platform = nullptr;

void UpdateCursorCapture(PlatformState& platform, bool capture) {
    if (capture == platform.mouse_captured) {
        return;
    }
    platform.mouse_captured = capture;
    if (capture) {
        platform.mouse_dx = 0.0f;
        platform.mouse_dy = 0.0f;
        platform.cursor_valid = false;
        platform.ignore_mouse_until = NowSeconds() + 0.10;
        RECT client_rect = {};
        GetClientRect(platform.hwnd, &client_rect);
        POINT tl = {client_rect.left, client_rect.top};
        POINT br = {client_rect.right, client_rect.bottom};
        ClientToScreen(platform.hwnd, &tl);
        ClientToScreen(platform.hwnd, &br);
        RECT clip = {tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
        SetCapture(platform.hwnd);
        SetCursor(nullptr);
    } else {
        ClipCursor(nullptr);
        ReleaseCapture();
        platform.ignore_mouse_until = 0.0;
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }
}

void ApplyResolution(PlatformState& platform, const Settings& settings) {
    const ResolutionOption& option = kResolutions[settings.resolution_index];
    if (!platform.fullscreen) {
        RECT rect = {0, 0, option.width, option.height};
        AdjustWindowRect(&rect, platform.windowed_style, FALSE);
        SetWindowPos(platform.hwnd, nullptr, platform.windowed_rect.left, platform.windowed_rect.top,
            rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

RECT GetPrimaryMonitorRect() {
    MONITORINFO monitor_info = {sizeof(monitor_info)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor_info);
    return monitor_info.rcMonitor;
}

RECT BuildDefaultWindowedRect(const Settings& settings) {
    RECT rect = {0, 0, kResolutions[settings.resolution_index].width, kResolutions[settings.resolution_index].height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    const RECT monitor_rect = GetPrimaryMonitorRect();
    const int monitor_width = monitor_rect.right - monitor_rect.left;
    const int monitor_height = monitor_rect.bottom - monitor_rect.top;
    const int window_width = rect.right - rect.left;
    const int window_height = rect.bottom - rect.top;
    const int x = monitor_rect.left + std::max(0, (monitor_width - window_width) / 2);
    const int y = monitor_rect.top + std::max(0, (monitor_height - window_height) / 2);
    return {x, y, x + window_width, y + window_height};
}

void ToggleFullscreen(PlatformState& platform, Settings& settings) {
    settings.fullscreen = !settings.fullscreen;
    if (settings.fullscreen) {
        platform.windowed_style = static_cast<DWORD>(GetWindowLongPtr(platform.hwnd, GWL_STYLE));
        GetWindowRect(platform.hwnd, &platform.windowed_rect);
        MONITORINFO monitor_info = {sizeof(monitor_info)};
        GetMonitorInfoW(MonitorFromWindow(platform.hwnd, MONITOR_DEFAULTTONEAREST), &monitor_info);
        SetWindowLongPtr(platform.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(platform.hwnd, HWND_TOP,
            monitor_info.rcMonitor.left, monitor_info.rcMonitor.top,
            monitor_info.rcMonitor.right - monitor_info.rcMonitor.left,
            monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top,
            SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtr(platform.hwnd, GWL_STYLE, platform.windowed_style);
        SetWindowPos(platform.hwnd, nullptr,
            platform.windowed_rect.left, platform.windowed_rect.top,
            platform.windowed_rect.right - platform.windowed_rect.left,
            platform.windowed_rect.bottom - platform.windowed_rect.top,
            SWP_FRAMECHANGED | SWP_NOZORDER);
        ApplyResolution(platform, settings);
    }
    platform.fullscreen = settings.fullscreen;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    PlatformState& platform = *g_platform;
    switch (message) {
    case WM_DESTROY:
        platform.running = false;
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        platform.running = false;
        DestroyWindow(hwnd);
        return 0;
    case WM_SIZE:
        platform.client_width = LOWORD(lparam);
        platform.client_height = HIWORD(lparam);
        platform.resized = true;
        return 0;
    case WM_SETFOCUS:
        platform.focused = true;
        return 0;
    case WM_KILLFOCUS:
        platform.focused = false;
        platform.gameplay_focus = false;
        platform.pending_gameplay_focus = false;
        UpdateCursorCapture(platform, false);
        return 0;
    case WM_INPUT: {
        RAWINPUT raw = {};
        UINT data_size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &data_size, sizeof(RAWINPUTHEADER)) == data_size) {
            if (raw.header.dwType == RIM_TYPEMOUSE) {
                platform.mouse_dx += static_cast<float>(raw.data.mouse.lLastX);
                platform.mouse_dy += static_cast<float>(raw.data.mouse.lLastY);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam < 256) {
            platform.keys[static_cast<size_t>(wparam)] = true;
        }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wparam < 256) {
            platform.keys[static_cast<size_t>(wparam)] = false;
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (platform.focused && !platform.mouse_captured) {
            platform.pending_gameplay_focus = true;
            return 0;
        }
        platform.left_mouse_down = true;
        return 0;
    case WM_LBUTTONUP:
        if (platform.pending_gameplay_focus) {
            platform.pending_gameplay_focus = false;
            platform.gameplay_focus = true;
            platform.left_mouse_down = false;
            return 0;
        }
        platform.left_mouse_down = false;
        return 0;
    case WM_SETCURSOR:
        if (platform.mouse_captured) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool KeyPressed(const PlatformState& platform, int vk) {
    return platform.keys[vk] && !platform.prev_keys[vk];
}

void PumpPlatformMessages(PlatformState& platform) {
    platform.prev_keys = platform.keys;
    platform.prev_left_mouse_down = platform.left_mouse_down;
    platform.mouse_dx = 0.0f;
    platform.mouse_dy = 0.0f;
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

struct TracerFx {
    Vec3 start = {};
    Vec3 end = {};
    double spawn_time = 0.0;
    double end_time = 0.0;
    float segment_length = 6.0f;
    Color color = {1.0f, 0.84f, 0.35f, 1.0f};
};

struct ImpactFx {
    Vec3 position = {};
    double spawn_time = 0.0;
    double end_time = 0.0;
    Color color = {1.0f, 0.80f, 0.38f, 1.0f};
};

struct ClientRuntime {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in server_address = {};
    WorldGeometry world = BuildWorldGeometry();
    PlayerState predicted_player = {};
    std::deque<PredictionEntry> history;
    std::array<RenderDummy, kMaxDummies> render_dummies = {};
    uint32_t render_dummy_count = 0;
    uint32_t local_sequence = 0;
    uint32_t last_acknowledged_sequence = 0;
    double estimated_clock_offset = 0.0;
    std::deque<std::string> damage_log;
    double hitmarker_time = 0.0;
    double elimination_time = 0.0;
    float last_correction_cm = 0.0f;
    float max_correction_cm = 0.0f;
    int correction_events = 0;
    int sim_ticks_this_second = 0;
    double sim_stats_time = 0.0;
    double sim_rate = kSimHz;
    bool debug_hud = true;
    bool show_hitboxes = false;
    bool artificial_delay = false;
    double artificial_delay_seconds = 0.05;
    double smoothed_render_rate = 0.0;
    double smoothed_frame_ms = 0.0;
    std::array<float, 120> frame_graph = {};
    size_t frame_graph_index = 0;
    std::deque<TracerFx> tracers;
    std::deque<ImpactFx> impacts;
    double muzzle_flash_time = 0.0;
    double weapon_bob_phase = 0.0;
    float weapon_recoil = 0.0f;
    float weapon_lift = 0.0f;
    float weapon_roll = 0.0f;
    float weapon_sway_x = 0.0f;
    float weapon_sway_y = 0.0f;
    float weapon_land_kick = 0.0f;
    float weapon_crouch_blend = 0.0f;
    float footstep_distance = 0.0f;
    bool alternate_footstep = false;
    bool presentation_initialized = false;
    bool presentation_last_on_ground = true;
    bool presentation_last_crouched = false;
    Vec3 presentation_last_position = {};
    float presentation_last_velocity_y = 0.0f;

    struct PendingPacket {
        double release_time = 0.0;
        std::vector<uint8_t> payload;
    };

    std::deque<PendingPacket> delayed_send_queue;
    std::deque<PendingPacket> delayed_receive_queue;

    bool Initialize();
    void Shutdown();
    void SendPacket(const void* data, size_t size, bool respect_delay);
    void FlushDelayedSends(double now);
    void PumpNetwork(double now);
    void ConsumePacket(const void* data, size_t size, double now);
    void HandleSnapshot(const SnapshotPacket& snapshot, double receipt_time);
    void HandleHit(const HitConfirmPacket& hit, double now);
    bool StepPrediction(const InputCommand& input, double dt);
    InputCommand BuildInput(const PlatformState& platform, float yaw, float pitch);
    void TickSimRate(double now);
    void UpdatePresentation(double now, double dt, float look_dx, float look_dy, AudioEngine& audio, const Settings& settings);
    void AddTracer(const Vec3& start, const Vec3& end, double now, const Color& color);
    void AddImpact(const Vec3& position, double now, const Color& color);
    void OnPredictedShot(const InputCommand& input, double now, AudioEngine& audio, const Settings& settings);
};

bool ClientRuntime::Initialize() {
    socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        return false;
    }
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);

    sockaddr_in local_address = {};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(0);
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket, reinterpret_cast<sockaddr*>(&local_address), sizeof(local_address)) == SOCKET_ERROR) {
        closesocket(socket);
        socket = INVALID_SOCKET;
        return false;
    }

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(kServerPort);
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);

    HelloPacket hello = MakePacket<HelloPacket>(PacketType::Hello);
    hello.version = 1;
    SendPacket(&hello, sizeof(hello), false);

    predicted_player.position = {0.0f, 0.0f, 2.0f};
    predicted_player.weapon.ammo = 30;
    sim_stats_time = NowSeconds();
    return true;
}

void ClientRuntime::Shutdown() {
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
        socket = INVALID_SOCKET;
    }
}

void ClientRuntime::SendPacket(const void* data, size_t size, bool respect_delay) {
    if (artificial_delay && respect_delay) {
        PendingPacket pending = {};
        pending.release_time = NowSeconds() + artificial_delay_seconds;
        pending.payload.resize(size);
        std::memcpy(pending.payload.data(), data, size);
        delayed_send_queue.push_back(std::move(pending));
        return;
    }

    sendto(socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
        reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address));
}

void ClientRuntime::FlushDelayedSends(double now) {
    while (!delayed_send_queue.empty() && delayed_send_queue.front().release_time <= now) {
        PendingPacket pending = std::move(delayed_send_queue.front());
        delayed_send_queue.pop_front();
        sendto(socket, reinterpret_cast<const char*>(pending.payload.data()), static_cast<int>(pending.payload.size()), 0,
            reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address));
    }
}

void ClientRuntime::PumpNetwork(double now) {
    FlushDelayedSends(now);

    std::array<uint8_t, 2048> buffer = {};
    while (true) {
        sockaddr_in from = {};
        int from_len = sizeof(from);
        const int received = recvfrom(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received <= 0) {
            break;
        }
        if (artificial_delay) {
            PendingPacket pending = {};
            pending.release_time = now + artificial_delay_seconds;
            pending.payload.assign(buffer.begin(), buffer.begin() + received);
            delayed_receive_queue.push_back(std::move(pending));
        } else {
            ConsumePacket(buffer.data(), static_cast<size_t>(received), now);
        }
    }

    while (!delayed_receive_queue.empty() && delayed_receive_queue.front().release_time <= now) {
        PendingPacket pending = std::move(delayed_receive_queue.front());
        delayed_receive_queue.pop_front();
        ConsumePacket(pending.payload.data(), pending.payload.size(), now);
    }
}

void ClientRuntime::ConsumePacket(const void* data, size_t size, double now) {
    if (size < sizeof(PacketHeader)) {
        return;
    }
    const PacketHeader* header = reinterpret_cast<const PacketHeader*>(data);
    if (header->magic != kPacketMagic || header->size != size) {
        return;
    }
    if (static_cast<PacketType>(header->type) == PacketType::Snapshot && size == sizeof(SnapshotPacket)) {
        HandleSnapshot(*reinterpret_cast<const SnapshotPacket*>(data), now);
    } else if (static_cast<PacketType>(header->type) == PacketType::HitConfirm && size == sizeof(HitConfirmPacket)) {
        HandleHit(*reinterpret_cast<const HitConfirmPacket*>(data), now);
    }
}

void ClientRuntime::HandleSnapshot(const SnapshotPacket& snapshot, double receipt_time) {
    estimated_clock_offset = snapshot.server_time - receipt_time;
    last_acknowledged_sequence = snapshot.last_processed_input;

    render_dummy_count = std::min<uint32_t>(snapshot.dummy_count, kMaxDummies);
    for (uint32_t i = 0; i < render_dummy_count; ++i) {
        RenderDummy& dummy = render_dummies[i];
        dummy.id = snapshot.dummies[i].id;
        dummy.position = {snapshot.dummies[i].x, snapshot.dummies[i].y, snapshot.dummies[i].z};
        dummy.alive = snapshot.dummies[i].alive != 0;
        dummy.health = snapshot.dummies[i].health;
    }

    const PlayerState authoritative = PlayerStateFromNet(snapshot.player);
    auto it = std::find_if(history.begin(), history.end(), [&](const PredictionEntry& entry) {
        return entry.input.sequence == snapshot.last_processed_input;
    });
    if (it == history.end()) {
        predicted_player = authoritative;
        history.clear();
        return;
    }

    const float position_delta = Length(it->state.position - authoritative.position);
    last_correction_cm = position_delta * 100.0f;
    max_correction_cm = std::max(max_correction_cm, last_correction_cm);

    std::deque<PredictionEntry> remaining;
    bool past_ack = false;
    for (const PredictionEntry& entry : history) {
        if (entry.input.sequence == snapshot.last_processed_input) {
            past_ack = true;
            continue;
        }
        if (past_ack) {
            remaining.push_back(entry);
        }
    }

    predicted_player = authoritative;
    history.clear();
    for (PredictionEntry& entry : remaining) {
        ApplyMovement(predicted_player, entry.input, world, kFixedDt);
        ApplyWeaponPrediction(predicted_player, entry.input, kFixedDt);
        entry.state = predicted_player;
        history.push_back(entry);
    }
}

void ClientRuntime::HandleHit(const HitConfirmPacket& hit, double now) {
    hitmarker_time = now + 0.10;
    if (hit.eliminated) {
        elimination_time = now + 0.20;
    }
    char line[128] = {};
    std::snprintf(line, sizeof(line), "Seq %u hit dummy %u for %d%s%s",
        hit.input_sequence, hit.dummy_id, hit.damage,
        hit.headshot ? " headshot" : "",
        hit.eliminated ? " elim" : "");
    damage_log.emplace_front(line);
    while (damage_log.size() > 6) {
        damage_log.pop_back();
    }
}

bool ClientRuntime::StepPrediction(const InputCommand& input, double dt) {
    ApplyMovement(predicted_player, input, world, dt);
    const bool fired = ApplyWeaponPrediction(predicted_player, input, dt);
    PredictionEntry entry = {};
    entry.input = input;
    entry.state = predicted_player;
    history.push_back(entry);
    while (history.size() > 512) {
        history.pop_front();
    }
    return fired;
}

void ClientRuntime::AddTracer(const Vec3& start, const Vec3& end, double now, const Color& color) {
    TracerFx tracer = {};
    tracer.start = start;
    tracer.end = end;
    tracer.spawn_time = now;
    tracer.color = color;
    const float distance = Length(end - start);
    tracer.end_time = now + Clamp(distance / 480.0f, 0.03f, 0.10f);
    tracer.segment_length = Clamp(distance * 0.20f, 2.4f, 7.5f);
    tracers.push_back(tracer);
    while (tracers.size() > 16) {
        tracers.pop_front();
    }
}

void ClientRuntime::AddImpact(const Vec3& position, double now, const Color& color) {
    ImpactFx impact = {};
    impact.position = position;
    impact.spawn_time = now;
    impact.end_time = now + 0.09;
    impact.color = color;
    impacts.push_back(impact);
    while (impacts.size() > 16) {
        impacts.pop_front();
    }
}

void ClientRuntime::OnPredictedShot(const InputCommand& input, double now, AudioEngine& audio, const Settings& settings) {
    weapon_recoil = std::min(1.35f, weapon_recoil + 1.0f);
    weapon_lift = std::min(1.0f, weapon_lift + 1.0f);
    weapon_roll = Clamp(weapon_roll + ((input.sequence & 1u) ? 0.75f : -0.75f), -1.0f, 1.0f);
    muzzle_flash_time = now + 0.045;

    TraceDummy trace_dummies[kMaxDummies] = {};
    for (uint32_t i = 0; i < render_dummy_count; ++i) {
        trace_dummies[i].id = render_dummies[i].id;
        trace_dummies[i].position = render_dummies[i].position;
        trace_dummies[i].alive = render_dummies[i].alive;
    }

    const Vec3 origin = predicted_player.position + Vec3{0.0f, EyeHeight(predicted_player.crouched), 0.0f};
    const Vec3 ray_dir = ForwardFromAngles(input.yaw, input.pitch);
    const TraceHit trace = ResolveHitscanTrace(origin, ray_dir, world, trace_dummies, render_dummy_count);
    const Color tracer_color = trace.hit && !trace.hit_world
        ? (trace.headshot ? Color{1.0f, 0.68f, 0.34f, 1.0f} : Color{0.36f, 0.96f, 0.86f, 1.0f})
        : Color{1.0f, 0.86f, 0.40f, 1.0f};
    AddTracer(origin, trace.point, now, tracer_color);
    if (trace.hit) {
        AddImpact(trace.point, now, trace.hit_world ? Color{1.0f, 0.80f, 0.44f, 1.0f} : tracer_color);
    }

    const float pitch = 0.98f + static_cast<float>(input.sequence % 3u) * 0.02f;
    audio.Play(AudioClipId::Gunshot, AudioBus::Weapon, settings, 0.95f, pitch);
}

void ClientRuntime::UpdatePresentation(double now, double dt, float look_dx, float look_dy, AudioEngine& audio, const Settings& settings) {
    if (!presentation_initialized) {
        presentation_initialized = true;
        presentation_last_position = predicted_player.position;
        presentation_last_velocity_y = predicted_player.velocity.y;
        presentation_last_on_ground = predicted_player.on_ground;
        presentation_last_crouched = predicted_player.crouched;
        weapon_crouch_blend = predicted_player.crouched ? 1.0f : 0.0f;
        return;
    }

    const Vec3 frame_delta = predicted_player.position - presentation_last_position;
    const float horizontal_delta = Length(Vec2{frame_delta.x, frame_delta.z});
    const float speed = Length(Vec2{predicted_player.velocity.x, predicted_player.velocity.z});
    if (horizontal_delta > 0.75f) {
        footstep_distance = 0.0f;
    }

    weapon_crouch_blend = Lerp(weapon_crouch_blend, predicted_player.crouched ? 1.0f : 0.0f, Saturate(static_cast<float>(dt) * 12.0f));
    weapon_recoil = Lerp(weapon_recoil, 0.0f, Saturate(static_cast<float>(dt) * 18.0f));
    weapon_lift = Lerp(weapon_lift, 0.0f, Saturate(static_cast<float>(dt) * 16.0f));
    weapon_roll = Lerp(weapon_roll, 0.0f, Saturate(static_cast<float>(dt) * 14.0f));
    weapon_land_kick = Lerp(weapon_land_kick, 0.0f, Saturate(static_cast<float>(dt) * 9.0f));
    weapon_sway_x = Lerp(weapon_sway_x, Clamp(-look_dx * 0.55f, -12.0f, 12.0f), Saturate(static_cast<float>(dt) * 18.0f));
    weapon_sway_y = Lerp(weapon_sway_y, Clamp(look_dy * 0.55f, -10.0f, 10.0f), Saturate(static_cast<float>(dt) * 18.0f));

    if (presentation_last_on_ground && !predicted_player.on_ground && predicted_player.velocity.y > 1.0f) {
        audio.Play(AudioClipId::Jump, AudioBus::Movement, settings, 0.70f, 1.0f);
    }
    if (presentation_last_crouched != predicted_player.crouched) {
        audio.Play(AudioClipId::Crouch, AudioBus::Movement, settings, predicted_player.crouched ? 0.60f : 0.48f,
            predicted_player.crouched ? 0.98f : 1.08f);
    }
    if (!presentation_last_on_ground && predicted_player.on_ground) {
        weapon_land_kick = std::max(weapon_land_kick, Clamp(-presentation_last_velocity_y / 12.0f, 0.0f, 1.0f));
    }

    if (predicted_player.on_ground && speed > 1.2f) {
        footstep_distance += horizontal_delta;
        const float step_spacing = predicted_player.crouched ? 1.10f : 1.55f;
        while (footstep_distance >= step_spacing) {
            footstep_distance -= step_spacing;
            alternate_footstep = !alternate_footstep;
            audio.Play(alternate_footstep ? AudioClipId::FootstepLeft : AudioClipId::FootstepRight,
                AudioBus::Movement, settings, predicted_player.crouched ? 0.44f : 0.56f,
                predicted_player.crouched ? 0.92f : 1.0f);
        }
        weapon_bob_phase += horizontal_delta * (predicted_player.crouched ? 15.0f : 11.0f);
    }

    while (!tracers.empty() && tracers.front().end_time <= now) {
        tracers.pop_front();
    }
    while (!impacts.empty() && impacts.front().end_time <= now) {
        impacts.pop_front();
    }

    presentation_last_position = predicted_player.position;
    presentation_last_velocity_y = predicted_player.velocity.y;
    presentation_last_on_ground = predicted_player.on_ground;
    presentation_last_crouched = predicted_player.crouched;
}

InputCommand ClientRuntime::BuildInput(const PlatformState& platform, float yaw, float pitch) {
    InputCommand input = {};
    input.sequence = ++local_sequence;
    input.client_time = NowSeconds();
    input.estimated_server_time = input.client_time + estimated_clock_offset;
    if (platform.keys['A']) {
        input.move_x -= 1.0f;
    }
    if (platform.keys['D']) {
        input.move_x += 1.0f;
    }
    if (platform.keys['S']) {
        input.move_y -= 1.0f;
    }
    if (platform.keys['W']) {
        input.move_y += 1.0f;
    }
    if (platform.keys[VK_SPACE]) {
        input.buttons |= Button_Jump;
    }
    if (platform.keys[VK_CONTROL]) {
        input.buttons |= Button_Crouch;
    }
    if (platform.left_mouse_down) {
        input.buttons |= Button_Fire;
    }
    if (KeyPressed(platform, 'R')) {
        input.buttons |= Button_Reload;
    }
    input.yaw = yaw;
    input.pitch = pitch;
    return input;
}

void ClientRuntime::TickSimRate(double now) {
    ++sim_ticks_this_second;
    if (now - sim_stats_time >= 1.0) {
        sim_rate = static_cast<double>(sim_ticks_this_second) / (now - sim_stats_time);
        sim_stats_time = now;
        sim_ticks_this_second = 0;
    }
}

struct MenuState {
    bool open = false;
    int tab = 0;
    std::array<int, 4> row = {{0, 0, 0, 0}};
};

std::string FormatPercentLabel(float value) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%.0f%%", Saturate(value) * 100.0f);
    return text;
}

void DrawFrameGraph(Renderer& renderer, float x, float y, float w, float h, const std::array<float, 120>& samples, size_t head) {
    renderer.PushRect2D(x, y, w, h, {0.04f, 0.08f, 0.12f, 0.75f});
    for (size_t i = 0; i < samples.size() - 1; ++i) {
        const size_t a_index = (head + i) % samples.size();
        const size_t b_index = (head + i + 1) % samples.size();
        const float ax = x + (static_cast<float>(i) / static_cast<float>(samples.size() - 1)) * w;
        const float bx = x + (static_cast<float>(i + 1) / static_cast<float>(samples.size() - 1)) * w;
        const float ay = y + h - Clamp(samples[a_index] / 16.0f, 0.0f, 1.0f) * h;
        const float by = y + h - Clamp(samples[b_index] / 16.0f, 0.0f, 1.0f) * h;
        renderer.PushLine2D(ax, ay, bx, by, {0.28f, 0.95f, 0.78f, 1.0f});
    }
}

void DrawSettingsMenu(Renderer& renderer, const MenuState& menu, const Settings& settings, int width, int height) {
    if (!menu.open) {
        return;
    }
    const float panel_x = width * 0.14f;
    const float panel_y = height * 0.12f;
    const float panel_w = width * 0.72f;
    const float panel_h = height * 0.72f;
    renderer.PushRect2D(panel_x, panel_y, panel_w, panel_h, {0.03f, 0.05f, 0.08f, 0.90f});
    renderer.PushRect2D(panel_x + 18.0f, panel_y + 18.0f, 170.0f, panel_h - 36.0f, {0.08f, 0.12f, 0.16f, 0.95f});

    const std::array<const char*, 4> tabs = {{"Mouse", "Video", "Audio", "Gameplay"}};
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        const bool active = menu.tab == i;
        renderer.PushRect2D(panel_x + 28.0f, panel_y + 36.0f + i * 46.0f, 150.0f, 34.0f, active ? Color{0.20f, 0.38f, 0.48f, 1.0f} : Color{0.10f, 0.16f, 0.20f, 1.0f});
        renderer.PushText(panel_x + 40.0f, panel_y + 47.0f + i * 46.0f, tabs[i], {0.95f, 0.97f, 1.0f, 1.0f});
    }

    renderer.PushText(panel_x + 220.0f, panel_y + 32.0f, "SETTINGS", {1.0f, 0.95f, 0.75f, 1.0f});
    renderer.PushText(panel_x + 220.0f, panel_y + panel_h - 26.0f, "Tab: switch category   Up/Down: select   Left/Right: adjust   Enter: toggle/apply", {0.70f, 0.76f, 0.84f, 1.0f});

    auto draw_row = [&](int row_index, const char* label, const std::string& value) {
        const float row_y = panel_y + 86.0f + row_index * 42.0f;
        const bool selected = menu.row[menu.tab] == row_index;
        renderer.PushRect2D(panel_x + 210.0f, row_y, panel_w - 240.0f, 32.0f,
            selected ? Color{0.16f, 0.22f, 0.28f, 1.0f} : Color{0.08f, 0.12f, 0.16f, 0.75f});
        renderer.PushText(panel_x + 224.0f, row_y + 10.0f, label, {0.92f, 0.94f, 0.98f, 1.0f});
        renderer.PushText(panel_x + panel_w - 220.0f, row_y + 10.0f, value.c_str(), {0.60f, 0.95f, 0.80f, 1.0f});
    };

    if (menu.tab == 0) {
        char sensitivity[64] = {};
        std::snprintf(sensitivity, sizeof(sensitivity), "%.3f", settings.mouse_sensitivity);
        draw_row(0, "Mouse Sensitivity", sensitivity);
        draw_row(1, "Raw Input", settings.raw_input ? "On" : "Off");
        draw_row(2, "Invert Y", settings.invert_y ? "On" : "Off");
    } else if (menu.tab == 1) {
        char fov[32] = {};
        std::snprintf(fov, sizeof(fov), "%.0f", settings.fov_horizontal);
        draw_row(0, "Horizontal FOV", fov);
        draw_row(1, "Resolution", kResolutions[settings.resolution_index].label);
        draw_row(2, "Fullscreen", settings.fullscreen ? "On" : "Off");
        draw_row(3, "Frame Cap", kFrameCapLabels[settings.frame_cap_index]);
    } else if (menu.tab == 2) {
        draw_row(0, "Master Volume", FormatPercentLabel(settings.audio_master_volume));
        draw_row(1, "Weapon SFX", FormatPercentLabel(settings.audio_weapon_volume));
        draw_row(2, "Movement SFX", FormatPercentLabel(settings.audio_movement_volume));
    } else {
        draw_row(0, "Hitmarkers", settings.show_hitmarkers ? "On" : "Off");
        draw_row(1, "HUD Focus", "Competitive Debug");
        draw_row(2, "Net Delay Toggle", "Use F3 in match");
        draw_row(3, "Exit Game", "Press Enter");
    }
}

bool HandleMenuInput(MenuState& menu, Settings& settings, PlatformState& platform, const AppPaths& paths) {
    if (KeyPressed(platform, VK_TAB)) {
        menu.tab = (menu.tab + 1) % 4;
    }

    const int max_rows = menu.tab == 0 ? 3 : (menu.tab == 1 ? 4 : (menu.tab == 2 ? 3 : 4));
    if (KeyPressed(platform, VK_UP)) {
        menu.row[menu.tab] = (menu.row[menu.tab] + max_rows - 1) % max_rows;
    }
    if (KeyPressed(platform, VK_DOWN)) {
        menu.row[menu.tab] = (menu.row[menu.tab] + 1) % max_rows;
    }

    auto save = [&]() { SaveSettings(paths, settings); };
    const bool left = KeyPressed(platform, VK_LEFT);
    const bool right = KeyPressed(platform, VK_RIGHT);
    const bool activate = KeyPressed(platform, VK_RETURN);
    const int row = menu.row[menu.tab];

    if (menu.tab == 0) {
        if (row == 0 && (left || right)) {
            settings.mouse_sensitivity = Clamp(settings.mouse_sensitivity + (right ? 0.005f : -0.005f), 0.01f, 1.0f);
            save();
        } else if (row == 1 && (left || right || activate)) {
            settings.raw_input = !settings.raw_input;
            save();
        } else if (row == 2 && (left || right || activate)) {
            settings.invert_y = !settings.invert_y;
            save();
        }
    } else if (menu.tab == 1) {
        if (row == 0 && (left || right)) {
            settings.fov_horizontal = Clamp(settings.fov_horizontal + (right ? 1.0f : -1.0f), 80.0f, 103.0f);
            save();
        } else if (row == 1 && (left || right)) {
            settings.resolution_index = std::clamp(settings.resolution_index + (right ? 1 : -1), 0, static_cast<int>(kResolutions.size()) - 1);
            ApplyResolution(platform, settings);
            save();
        } else if (row == 2 && (left || right || activate)) {
            ToggleFullscreen(platform, settings);
            save();
        } else if (row == 3 && (left || right)) {
            settings.frame_cap_index = std::clamp(settings.frame_cap_index + (right ? 1 : -1), 0, static_cast<int>(kFrameCaps.size()) - 1);
            save();
        }
    } else if (menu.tab == 2) {
        if (row == 0 && (left || right)) {
            settings.audio_master_volume = Clamp(settings.audio_master_volume + (right ? 0.05f : -0.05f), 0.0f, 1.0f);
            save();
        } else if (row == 1 && (left || right)) {
            settings.audio_weapon_volume = Clamp(settings.audio_weapon_volume + (right ? 0.05f : -0.05f), 0.0f, 1.0f);
            save();
        } else if (row == 2 && (left || right)) {
            settings.audio_movement_volume = Clamp(settings.audio_movement_volume + (right ? 0.05f : -0.05f), 0.0f, 1.0f);
            save();
        }
    } else {
        if (row == 0 && (left || right || activate)) {
            settings.show_hitmarkers = !settings.show_hitmarkers;
            save();
        } else if (row == 3 && activate) {
            return true;
        }
    }
    return false;
}

void DrawDebugHud(Renderer& renderer, const ClientRuntime& client, const ServerDebugStats& server_stats, int width, int height) {
    if (!client.debug_hud) {
        return;
    }

    renderer.PushRect2D(18.0f, 18.0f, 360.0f, 198.0f, {0.03f, 0.05f, 0.09f, 0.82f});
    DrawFrameGraph(renderer, 28.0f, 114.0f, 340.0f, 90.0f, client.frame_graph, client.frame_graph_index);

    char line[256] = {};
    std::snprintf(line, sizeof(line), "FPS %.1f   Frame %.2f ms", client.smoothed_render_rate, client.smoothed_frame_ms);
    renderer.PushText(28.0f, 32.0f, line, {0.95f, 0.97f, 1.0f, 1.0f});
    std::snprintf(line, sizeof(line), "Render %dx%d   Sim %.1f Hz   Server %.1f Hz", width, height, client.sim_rate, server_stats.tick_rate_x100.load() / 100.0f);
    renderer.PushText(28.0f, 48.0f, line, {0.80f, 0.90f, 1.0f, 1.0f});
    std::snprintf(line, sizeof(line), "Server jitter %.3f ms   Last corr %.2f cm   Max corr %.2f cm", server_stats.jitter_us.load() / 1000.0f, client.last_correction_cm, client.max_correction_cm);
    renderer.PushText(28.0f, 64.0f, line, {0.66f, 0.96f, 0.84f, 1.0f});
    std::snprintf(line, sizeof(line), "Velocity %.2f m/s   Ammo %d   Delay %s", Length({client.predicted_player.velocity.x, 0.0f, client.predicted_player.velocity.z}),
        client.predicted_player.weapon.ammo, client.artificial_delay ? "100ms RTT sim" : "Off");
    renderer.PushText(28.0f, 80.0f, line, {1.0f, 0.91f, 0.72f, 1.0f});
    renderer.PushText(28.0f, 98.0f, "F1 HUD   F2 Hitboxes   F3 Net Delay   ESC Settings", {0.75f, 0.82f, 0.90f, 1.0f});

    renderer.PushRect2D(width - 330.0f, 18.0f, 312.0f, 158.0f, {0.03f, 0.05f, 0.09f, 0.82f});
    renderer.PushText(width - 318.0f, 32.0f, "Damage Log", {0.98f, 0.95f, 0.74f, 1.0f});
    int row = 0;
    for (const std::string& log : client.damage_log) {
        renderer.PushText(width - 318.0f, 50.0f + row * 16.0f, log.c_str(), {0.84f, 0.92f, 0.99f, 1.0f});
        ++row;
    }
}

Vec2 WeaponPoint(const Vec2& anchor, float scale, float rotation, float x, float y) {
    return anchor + Rotate({-x * scale, y * scale}, rotation);
}

void DrawWeaponOutline(Renderer& renderer, const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d, const Color& color) {
    renderer.PushLine2D(a.x, a.y, b.x, b.y, color);
    renderer.PushLine2D(b.x, b.y, c.x, c.y, color);
    renderer.PushLine2D(c.x, c.y, d.x, d.y, color);
    renderer.PushLine2D(d.x, d.y, a.x, a.y, color);
}

void DrawShotEffects(Renderer& renderer, const ClientRuntime& client, double now) {
    for (const TracerFx& tracer : client.tracers) {
        const double lifetime = std::max(0.0001, tracer.end_time - tracer.spawn_time);
        const float t = Saturate(static_cast<float>((now - tracer.spawn_time) / lifetime));
        const Vec3 delta = tracer.end - tracer.start;
        const float distance = Length(delta);
        if (distance <= 0.001f) {
            continue;
        }

        const Vec3 dir = delta / distance;
        const float head_distance = Clamp(t * distance, 0.0f, distance);
        const float tail_distance = std::max(0.0f, head_distance - tracer.segment_length);
        const Vec3 a = tracer.start + dir * tail_distance;
        const Vec3 b = tracer.start + dir * head_distance;
        Color color = tracer.color;
        color.a *= 1.0f - t * 0.55f;
        renderer.PushLine3D(a, b, color);
    }

    for (const ImpactFx& impact : client.impacts) {
        const double lifetime = std::max(0.0001, impact.end_time - impact.spawn_time);
        const float t = Saturate(static_cast<float>((now - impact.spawn_time) / lifetime));
        const float size = Lerp(0.05f, 0.18f, t);
        Color color = impact.color;
        color.a *= 1.0f - t;
        renderer.PushLine3D(impact.position - Vec3{size, 0.0f, 0.0f}, impact.position + Vec3{size, 0.0f, 0.0f}, color);
        renderer.PushLine3D(impact.position - Vec3{0.0f, size, 0.0f}, impact.position + Vec3{0.0f, size, 0.0f}, color);
        renderer.PushLine3D(impact.position - Vec3{0.0f, 0.0f, size}, impact.position + Vec3{0.0f, 0.0f, size}, color);
    }
}

void DrawWeaponViewModel(Renderer& renderer, const ClientRuntime& client, int width, int height, double now) {
    const float bob_x = std::sin(static_cast<float>(client.weapon_bob_phase)) * height * 0.008f;
    const float bob_y = std::fabs(std::sin(static_cast<float>(client.weapon_bob_phase) * 0.5f)) * height * 0.010f;
    const float crouch_offset = client.weapon_crouch_blend * height * 0.015f;
    const float land_offset = client.weapon_land_kick * height * 0.020f;
    const float recoil_back = client.weapon_recoil * height * 0.018f;
    const float recoil_drop = client.weapon_lift * height * 0.012f;
    const Vec2 anchor = {
        width * 0.72f + bob_x + client.weapon_sway_x - recoil_back,
        height * 0.765f + bob_y + crouch_offset + land_offset + client.weapon_sway_y + recoil_drop
    };
    const float scale = height * 0.225f;
    const Vec2 aim_target = {width * 0.53f, height * 0.56f};
    const Vec2 aim_dir = {aim_target.x - anchor.x, aim_target.y - anchor.y};
    const float rotation = static_cast<float>(std::atan2(aim_dir.y, aim_dir.x) - kPi)
        + std::sin(static_cast<float>(client.weapon_bob_phase)) * 0.020f
        + client.weapon_roll * 0.020f
        - client.weapon_lift * 0.015f;

    const Color shadow = {0.01f, 0.02f, 0.04f, 0.24f};
    const Color outline = {0.04f, 0.07f, 0.10f, 0.95f};
    const Color receiver = {0.20f, 0.24f, 0.30f, 0.96f};
    const Color receiver_light = {0.33f, 0.40f, 0.48f, 0.95f};
    const Color graphite = {0.10f, 0.13f, 0.18f, 0.98f};
    const Color accent = {0.16f, 0.90f, 0.84f, 0.95f};
    const Color hot = {1.0f, 0.66f, 0.34f, 0.92f};
    const Color glove = {0.28f, 0.19f, 0.14f, 0.95f};

    auto wp = [&](float x, float y) { return WeaponPoint(anchor, scale, rotation, x, y); };
    auto quad = [&](float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy, const Color& color, bool draw_outline = false) {
        const Vec2 a = wp(ax, ay);
        const Vec2 b = wp(bx, by);
        const Vec2 c = wp(cx, cy);
        const Vec2 d = wp(dx, dy);
        renderer.PushQuad2D(a, b, c, d, color);
        if (draw_outline) {
            DrawWeaponOutline(renderer, a, b, c, d, outline);
        }
    };
    auto tri = [&](float ax, float ay, float bx, float by, float cx, float cy, const Color& color) {
        renderer.PushTriangle2D(wp(ax, ay), wp(bx, by), wp(cx, cy), color);
    };

    quad(-0.82f, 0.20f, -0.50f, -0.02f, -0.08f, 0.28f, -0.44f, 0.48f, shadow);
    quad(-0.30f, 0.18f, 0.68f, -0.02f, 1.10f, 0.16f, -0.02f, 0.42f, shadow);

    quad(-0.78f, 0.06f, -0.54f, -0.10f, -0.34f, 0.18f, -0.60f, 0.34f, graphite, true);
    quad(-0.46f, -0.06f, 0.28f, -0.14f, 0.62f, 0.10f, -0.10f, 0.23f, receiver, true);
    quad(-0.34f, 0.00f, 0.20f, -0.06f, 0.46f, 0.08f, -0.06f, 0.14f, receiver_light);
    quad(0.28f, -0.02f, 0.90f, -0.08f, 1.02f, 0.00f, 0.40f, 0.05f, graphite, true);
    quad(0.62f, -0.10f, 0.86f, -0.12f, 0.98f, -0.01f, 0.72f, 0.00f, receiver_light, true);
    quad(-0.06f, 0.18f, 0.16f, 0.17f, 0.24f, 0.62f, 0.02f, 0.64f, graphite, true);
    quad(-0.22f, 0.12f, -0.04f, 0.10f, 0.06f, 0.42f, -0.14f, 0.46f, glove, true);
    quad(0.00f, -0.18f, 0.24f, -0.19f, 0.38f, -0.05f, 0.08f, -0.04f, receiver_light, true);
    quad(-0.04f, -0.01f, 0.28f, -0.04f, 0.40f, 0.00f, 0.08f, 0.03f, accent);
    quad(0.12f, 0.01f, 0.24f, 0.00f, 0.30f, 0.08f, 0.18f, 0.09f, hot);
    tri(-0.66f, 0.16f, -0.46f, 0.06f, -0.50f, 0.28f, receiver_light);
    tri(0.86f, -0.02f, 1.02f, 0.00f, 0.92f, 0.10f, receiver_light);

    const float flash = Saturate(static_cast<float>((client.muzzle_flash_time - now) / 0.045));
    if (flash > 0.0f) {
        const Color flash_outer = {1.0f, 0.92f, 0.58f, flash * 0.78f};
        const Color flash_core = {1.0f, 0.68f, 0.30f, flash * 0.92f};
        tri(1.00f, 0.00f, 1.18f + flash * 0.22f, -0.14f, 1.30f + flash * 0.26f, 0.02f, flash_outer);
        tri(1.00f, 0.00f, 1.30f + flash * 0.28f, 0.02f, 1.16f + flash * 0.20f, 0.18f, flash_core);
        tri(1.00f, 0.00f, 1.16f + flash * 0.14f, -0.05f, 1.42f + flash * 0.30f, 0.04f, {0.66f, 1.0f, 0.92f, flash * 0.24f});
    }
}

void DrawCrosshair(Renderer& renderer, const ClientRuntime& client, int width, int height, double now) {
    const float cx = width * 0.5f;
    const float cy = height * 0.5f;
    Color color = {0.95f, 0.95f, 0.95f, 1.0f};
    if (client.hitmarker_time > now) {
        color = client.elimination_time > now ? Color{1.0f, 0.84f, 0.40f, 1.0f} : Color{0.48f, 1.0f, 0.72f, 1.0f};
    }
    renderer.PushLine2D(cx - 10.0f, cy, cx - 3.0f, cy, color);
    renderer.PushLine2D(cx + 3.0f, cy, cx + 10.0f, cy, color);
    renderer.PushLine2D(cx, cy - 10.0f, cx, cy - 3.0f, color);
    renderer.PushLine2D(cx, cy + 3.0f, cx, cy + 10.0f, color);
    if (client.hitmarker_time > now) {
        renderer.PushLine2D(cx - 8.0f, cy - 8.0f, cx - 2.0f, cy - 2.0f, color);
        renderer.PushLine2D(cx + 8.0f, cy - 8.0f, cx + 2.0f, cy - 2.0f, color);
        renderer.PushLine2D(cx - 8.0f, cy + 8.0f, cx - 2.0f, cy + 2.0f, color);
        renderer.PushLine2D(cx + 8.0f, cy + 8.0f, cx + 2.0f, cy + 2.0f, color);
    }
}

void DrawWorld(Renderer& renderer, const WorldGeometry& world, const ClientRuntime& client) {
    renderer.PushBoxSolid({{-10.0f, -0.05f, -2.0f}, {10.0f, 0.0f, 58.0f}}, {0.18f, 0.24f, 0.30f, 1.0f});
    renderer.PushBoxSolid({{-10.0f, 0.0f, -2.0f}, {-9.5f, 6.0f, 58.0f}}, {0.16f, 0.32f, 0.42f, 1.0f});
    renderer.PushBoxSolid({{9.5f, 0.0f, -2.0f}, {10.0f, 6.0f, 58.0f}}, {0.16f, 0.32f, 0.42f, 1.0f});
    renderer.PushBoxSolid({{-10.0f, 0.0f, -2.5f}, {10.0f, 6.0f, -2.0f}}, {0.16f, 0.32f, 0.42f, 1.0f});
    renderer.PushBoxSolid({{-10.0f, 0.0f, 58.0f}, {10.0f, 6.0f, 58.5f}}, {0.16f, 0.32f, 0.42f, 1.0f});
    renderer.PushBoxSolid({{-0.12f, 0.0f, -2.0f}, {0.12f, 0.02f, 58.0f}}, {0.14f, 0.78f, 0.88f, 1.0f});
    renderer.PushBoxSolid({{-6.5f, 0.0f, 14.0f}, {-2.5f, 1.5f, 18.0f}}, {0.40f, 0.42f, 0.26f, 1.0f});
    renderer.PushBoxSolid({{-1.5f, 0.0f, 24.0f}, {1.5f, 1.8f, 25.5f}}, {0.50f, 0.34f, 0.24f, 1.0f});
    renderer.PushBoxSolid({{4.5f, 0.0f, 38.0f}, {7.0f, 1.6f, 39.5f}}, {0.32f, 0.28f, 0.46f, 1.0f});
    renderer.PushBoxSolid({{-8.0f, 0.0f, 46.0f}, {-5.5f, 1.2f, 49.0f}}, {0.28f, 0.38f, 0.24f, 1.0f});

    const std::array<float, 4> markers = {{10.0f, 20.0f, 35.0f, 50.0f}};
    const std::array<Color, 4> colors = {{
        {0.16f, 0.74f, 0.84f, 0.75f},
        {0.20f, 0.88f, 0.72f, 0.75f},
        {1.0f, 0.84f, 0.36f, 0.75f},
        {1.0f, 0.60f, 0.42f, 0.75f},
    }};
    for (size_t i = 0; i < markers.size(); ++i) {
        const float z = markers[i];
        renderer.PushBoxSolid({{-9.5f, 0.0f, z - 0.1f}, {9.5f, 0.02f, z + 0.1f}}, colors[i]);
    }

    for (uint32_t i = 0; i < client.render_dummy_count; ++i) {
        const RenderDummy& dummy = client.render_dummies[i];
        if (!dummy.alive) {
            continue;
        }
        const Aabb body = MakeDummyBodyAabb(dummy.position);
        const Aabb head = MakeDummyHeadAabb(dummy.position);
        renderer.PushBoxSolid(body, {0.94f, 0.96f, 1.0f, 1.0f});
        renderer.PushBoxSolid(head, {1.0f, 0.62f, 0.30f, 1.0f});
        if (client.show_hitboxes) {
            renderer.PushBoxWire(body, {0.34f, 1.0f, 0.80f, 1.0f});
            renderer.PushBoxWire(head, {1.0f, 0.80f, 0.32f, 1.0f});
        }
    }

    if (client.show_hitboxes) {
        for (const Aabb& solid : world.solids) {
            renderer.PushBoxWire(solid, {0.26f, 0.58f, 1.0f, 0.6f});
        }
    }
}

void DrawRangeGuide(Renderer& renderer, const ClientRuntime& client, int width) {
    renderer.PushRect2D(width * 0.33f, 16.0f, width * 0.34f, 32.0f, {0.03f, 0.05f, 0.09f, 0.70f});
    float x = width * 0.345f;
    for (uint32_t i = 0; i < client.render_dummy_count && i < 8; ++i) {
        char label[64] = {};
        std::snprintf(label, sizeof(label), "D%u %.0fm", client.render_dummies[i].id, client.render_dummies[i].position.z);
        renderer.PushText(x, 28.0f, label, client.render_dummies[i].alive ? Color{0.88f, 0.94f, 1.0f, 1.0f} : Color{0.55f, 0.60f, 0.68f, 1.0f});
        x += 72.0f;
    }
}

bool RegisterRawInput(HWND hwnd) {
    RAWINPUTDEVICE device = {};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = 0;
    device.hwndTarget = hwnd;
    return RegisterRawInputDevices(&device, 1, sizeof(device)) == TRUE;
}

HWND CreateMainWindow(HINSTANCE instance, PlatformState& platform, const Settings& settings) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) {
        return nullptr;
    }

    const RECT monitor_rect = GetPrimaryMonitorRect();
    platform.windowed_style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    platform.windowed_rect = BuildDefaultWindowedRect(settings);

    DWORD style = platform.windowed_style;
    int x = platform.windowed_rect.left;
    int y = platform.windowed_rect.top;
    int width = platform.windowed_rect.right - platform.windowed_rect.left;
    int height = platform.windowed_rect.bottom - platform.windowed_rect.top;
    if (settings.fullscreen) {
        style = WS_POPUP | WS_VISIBLE;
        x = monitor_rect.left;
        y = monitor_rect.top;
        width = monitor_rect.right - monitor_rect.left;
        height = monitor_rect.bottom - monitor_rect.top;
    }

    HWND hwnd = CreateWindowExW(0, kWindowClassName, kWindowTitle, style,
        x, y, width, height,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        return nullptr;
    }
    platform.hwnd = hwnd;
    platform.client_width = settings.fullscreen ? (monitor_rect.right - monitor_rect.left) : kResolutions[settings.resolution_index].width;
    platform.client_height = settings.fullscreen ? (monitor_rect.bottom - monitor_rect.top) : kResolutions[settings.resolution_index].height;
    platform.fullscreen = settings.fullscreen;
    return hwnd;
}

void SleepForFrameCap(double target_frame_seconds, double frame_start) {
    if (target_frame_seconds <= 0.0) {
        return;
    }
    while (true) {
        const double elapsed = NowSeconds() - frame_start;
        const double remaining = target_frame_seconds - elapsed;
        if (remaining <= 0.0) {
            break;
        }
        if (remaining > 0.003) {
            Sleep(1);
        }
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    SetProcessDPIAware();
    timeBeginPeriod(1);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(com_result);

    WSADATA wsa_data = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        MessageBoxW(nullptr, L"Failed to initialize WinSock.", L"Accretion", MB_OK | MB_ICONERROR);
        if (com_initialized) {
            CoUninitialize();
        }
        return 1;
    }

    AppPaths paths = BuildAppPaths();
    Settings settings = LoadSettings(paths);
    SaveSettings(paths, settings);
    TelemetryLogger telemetry = {};
    telemetry.Initialize(paths);

    PlatformState platform = {};
    g_platform = &platform;
    HWND hwnd = CreateMainWindow(instance, platform, settings);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create the main window.", L"Accretion", MB_OK | MB_ICONERROR);
        WSACleanup();
        timeEndPeriod(1);
        if (com_initialized) {
            CoUninitialize();
        }
        return 1;
    }
    RegisterRawInput(hwnd);

    Renderer renderer = {};
    if (!renderer.Initialize(hwnd, platform.client_width, platform.client_height)) {
        MessageBoxW(hwnd, L"Failed to initialize the Direct3D 11 renderer.", L"Accretion", MB_OK | MB_ICONERROR);
        DestroyWindow(hwnd);
        WSACleanup();
        timeEndPeriod(1);
        if (com_initialized) {
            CoUninitialize();
        }
        return 1;
    }

    AudioEngine audio = {};
    audio.Initialize();

    ServerDebugStats server_stats = {};
    ServerRuntime server = {};
    if (!server.Initialize(&server_stats)) {
        MessageBoxW(hwnd, L"Failed to start the local server.", L"Accretion", MB_OK | MB_ICONERROR);
        renderer.Shutdown();
        DestroyWindow(hwnd);
        WSACleanup();
        timeEndPeriod(1);
        audio.Shutdown();
        if (com_initialized) {
            CoUninitialize();
        }
        return 1;
    }

    ClientRuntime client = {};
    if (!client.Initialize()) {
        MessageBoxW(hwnd, L"Failed to initialize the local client networking.", L"Accretion", MB_OK | MB_ICONERROR);
        server.Shutdown();
        audio.Shutdown();
        renderer.Shutdown();
        DestroyWindow(hwnd);
        WSACleanup();
        timeEndPeriod(1);
        if (com_initialized) {
            CoUninitialize();
        }
        return 1;
    }

    telemetry.LogEvent(NowSeconds(), "session_started");
    MenuState menu = {};
    float view_yaw = 0.0f;
    float view_pitch = 0.0f;
    double previous_time = NowSeconds();
    const bool capture_startup_frame = command_line && std::wcsstr(command_line, L"--capture-startup-frame") != nullptr;
    const double capture_ready_time = previous_time + 0.25;
    bool startup_frame_captured = false;
    double sim_accumulator = 0.0;
    double render_stats_time = previous_time;
    int render_frames_this_second = 0;

    while (platform.running) {
        const double frame_start = NowSeconds();
        const double frame_dt = std::min(0.1, frame_start - previous_time);
        previous_time = frame_start;
        sim_accumulator += frame_dt;

        PumpPlatformMessages(platform);
        client.PumpNetwork(frame_start);

        if (!platform.focused) {
            menu.open = true;
        }
        if (KeyPressed(platform, VK_ESCAPE)) {
            menu.open = !menu.open;
            SaveSettings(paths, settings);
        }
        if (KeyPressed(platform, VK_F1)) {
            client.debug_hud = !client.debug_hud;
        }
        if (KeyPressed(platform, VK_F2)) {
            client.show_hitboxes = !client.show_hitboxes;
        }
        if (KeyPressed(platform, VK_F3)) {
            client.artificial_delay = !client.artificial_delay;
            telemetry.LogEvent(frame_start, client.artificial_delay ? "delay_on" : "delay_off");
        }
        if ((platform.keys[VK_MENU] && KeyPressed(platform, VK_RETURN)) || (platform.keys[VK_RETURN] && KeyPressed(platform, VK_MENU))) {
            ToggleFullscreen(platform, settings);
            SaveSettings(paths, settings);
        }

        if (menu.open) {
            UpdateCursorCapture(platform, false);
            if (HandleMenuInput(menu, settings, platform, paths)) {
                platform.running = false;
                continue;
            }
        } else {
            UpdateCursorCapture(platform, platform.focused && platform.gameplay_focus);
        }
        audio.Update(settings);

        if (!menu.open && platform.focused && platform.mouse_captured) {
            if (settings.raw_input) {
                if (frame_start >= platform.ignore_mouse_until) {
                    const float invert = settings.invert_y ? 1.0f : -1.0f;
                    view_yaw += platform.mouse_dx * settings.mouse_sensitivity * 0.01f;
                    view_pitch += platform.mouse_dy * settings.mouse_sensitivity * 0.01f * invert;
                }
            } else {
                POINT cursor = {};
                GetCursorPos(&cursor);
                if (!platform.cursor_valid) {
                    platform.last_cursor = cursor;
                    platform.cursor_valid = true;
                }
                const float dx = static_cast<float>(cursor.x - platform.last_cursor.x);
                const float dy = static_cast<float>(cursor.y - platform.last_cursor.y);
                if (frame_start >= platform.ignore_mouse_until) {
                    const float invert = settings.invert_y ? 1.0f : -1.0f;
                    view_yaw += dx * settings.mouse_sensitivity * 0.01f;
                    view_pitch += dy * settings.mouse_sensitivity * 0.01f * invert;
                }
                platform.last_cursor = cursor;
            }
        } else {
            platform.cursor_valid = false;
        }

        view_pitch = Clamp(view_pitch, Radians(-89.0f), Radians(89.0f));

        while (sim_accumulator >= kFixedDt) {
            InputCommand input = {};
            if (!menu.open && platform.focused && platform.mouse_captured) {
                input = client.BuildInput(platform, view_yaw, view_pitch);
            } else {
                input.sequence = ++client.local_sequence;
                input.client_time = NowSeconds();
                input.estimated_server_time = input.client_time + client.estimated_clock_offset;
                input.yaw = view_yaw;
                input.pitch = view_pitch;
            }
            const bool fired = client.StepPrediction(input, kFixedDt);
            if (fired) {
                client.OnPredictedShot(input, frame_start, audio, settings);
            }
            client.TickSimRate(frame_start);

            InputPacket packet = MakePacket<InputPacket>(PacketType::Input);
            packet.sequence = input.sequence;
            packet.client_time = input.client_time;
            packet.estimated_server_time = input.estimated_server_time;
            packet.move_x = input.move_x;
            packet.move_y = input.move_y;
            packet.yaw = input.yaw;
            packet.pitch = input.pitch;
            packet.buttons = input.buttons;
            client.SendPacket(&packet, sizeof(packet), true);
            sim_accumulator -= kFixedDt;
        }

        if (platform.resized) {
            platform.resized = false;
            renderer.Resize(platform.client_width, platform.client_height);
        }

        client.frame_graph[client.frame_graph_index] = static_cast<float>(frame_dt * 1000.0);
        client.frame_graph_index = (client.frame_graph_index + 1) % client.frame_graph.size();
        client.smoothed_frame_ms = client.smoothed_frame_ms * 0.9 + (frame_dt * 1000.0) * 0.1;
        ++render_frames_this_second;
        if (frame_start - render_stats_time >= 1.0) {
            client.smoothed_render_rate = static_cast<double>(render_frames_this_second) / (frame_start - render_stats_time);
            render_stats_time = frame_start;
            render_frames_this_second = 0;
        }
        client.UpdatePresentation(frame_start, frame_dt,
            platform.mouse_captured ? platform.mouse_dx : 0.0f,
            platform.mouse_captured ? platform.mouse_dy : 0.0f,
            audio, settings);

        renderer.Clear();
        renderer.BeginFrame({0.04f, 0.06f, 0.09f, 1.0f});
        DrawWorld(renderer, server.world, client);
        DrawShotEffects(renderer, client, frame_start);
        DrawRangeGuide(renderer, client, renderer.width);
        DrawWeaponViewModel(renderer, client, renderer.width, renderer.height, frame_start);
        DrawDebugHud(renderer, client, server_stats, renderer.width, renderer.height);
        DrawCrosshair(renderer, client, renderer.width, renderer.height, frame_start);
        DrawSettingsMenu(renderer, menu, settings, renderer.width, renderer.height);
        if (!platform.mouse_captured && !menu.open) {
            renderer.PushText(renderer.width * 0.5f - 110.0f, renderer.height * 0.5f + 42.0f,
                "LEFT CLICK TO ENTER RANGE", {0.98f, 0.95f, 0.76f, 1.0f});
        }
        if (settings.show_hitmarkers && client.elimination_time > frame_start) {
            renderer.PushText(renderer.width * 0.5f - 18.0f, renderer.height * 0.5f + 28.0f, "ELIM", {1.0f, 0.84f, 0.34f, 1.0f});
        }
        renderer.PushText(renderer.width - 214.0f, renderer.height - 52.0f, "MERCURY RIFLE", {0.64f, 0.96f, 0.90f, 1.0f});
        char ammo_text[64] = {};
        std::snprintf(ammo_text, sizeof(ammo_text), "%02d / 30", client.predicted_player.weapon.ammo);
        renderer.PushText(renderer.width - 96.0f, renderer.height - 34.0f, ammo_text, {1.0f, 0.96f, 0.78f, 1.0f});

        const float aspect = static_cast<float>(renderer.width) / static_cast<float>(renderer.height);
        const float vertical_fov = 2.0f * std::atan(std::tan(Radians(settings.fov_horizontal) * 0.5f) / aspect);
        const Mat4 projection = PerspectiveFovLh(vertical_fov, aspect, 0.05f, 200.0f);
        const Vec3 eye = client.predicted_player.position + Vec3{0.0f, EyeHeight(client.predicted_player.crouched), 0.0f};
        const Vec3 target = eye + ForwardFromAngles(view_yaw, view_pitch);
        const Mat4 view = LookAtLh(eye, target, {0.0f, 1.0f, 0.0f});
        renderer.Flush(Multiply(view, projection));
        if (capture_startup_frame && !startup_frame_captured && frame_start >= capture_ready_time) {
            startup_frame_captured = renderer.CaptureBackBufferBmp(paths.telemetry_dir / "startup_frame.bmp");
        }
        renderer.Present();

        const double velocity = Length({client.predicted_player.velocity.x, 0.0f, client.predicted_player.velocity.z});
        telemetry.LogFrame(frame_start, client.smoothed_render_rate, client.smoothed_frame_ms,
            server_stats.tick_rate_x100.load() / 100.0, server_stats.jitter_us.load() / 1000.0,
            client.sim_rate, client.last_correction_cm, velocity,
            client.artificial_delay ? "delay_on" : "delay_off");

        const int frame_cap = kFrameCaps[settings.frame_cap_index];
        if (frame_cap > 0) {
            SleepForFrameCap(1.0 / static_cast<double>(frame_cap), frame_start);
        }
    }

    telemetry.LogEvent(NowSeconds(), "session_ended");
    SaveSettings(paths, settings);
    audio.Shutdown();
    client.Shutdown();
    server.Shutdown();
    renderer.Shutdown();
    UpdateCursorCapture(platform, false);
    DestroyWindow(hwnd);
    WSACleanup();
    timeEndPeriod(1);
    if (com_initialized) {
        CoUninitialize();
    }
    return 0;
}
