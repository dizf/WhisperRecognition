#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <fstream>
#include <deque>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <direct.h> // for _mkdir
#else
#include <sys/stat.h> // for mkdir
#endif

#include "dsp.hpp"
#include "vad.hpp"
#include "recorder.hpp"
#include "whisper.h"

// Constants
constexpr size_t VAD_FRAME_SIZE = 512; // 32ms at 16000Hz
constexpr float WHISPER_LOW_VAD_THRESHOLD = 0.10f; // Lower trigger for very soft whispered phrases
constexpr bool ENABLE_DIAGNOSTIC_LOGS = false;
constexpr bool ENABLE_FILE_DIAGNOSTIC_LOGS = true;
constexpr const char* DIAGNOSTIC_LOG_PATH = "diagnostic_logs/whisper_diagnostic.log";
constexpr bool ENABLE_USER_EVENT_LOGS = false;
constexpr bool ENABLE_PARTIAL_TRANSCRIPTION = false;

struct TranscriptionJob {
    std::vector<float> samples;
    bool is_partial = false;
};

struct AudioStats {
    float peak = 0.0f;
    float rms = 0.0f;
};

std::string utf8_to_ansi(const std::string& utf8_str);

void whisper_log_silent(enum ggml_log_level, const char*, void*) {
}

std::mutex diagnostic_log_mutex;
const auto diagnostic_log_start_time = std::chrono::steady_clock::now();

void write_diagnostic_log(const std::string& message) {
    if (!ENABLE_FILE_DIAGNOSTIC_LOGS) {
        return;
    }

    std::lock_guard<std::mutex> lock(diagnostic_log_mutex);

    static bool initialized = false;
    static std::ofstream log_file;
    if (!initialized) {
#ifdef _WIN32
        _mkdir("diagnostic_logs");
#else
        mkdir("diagnostic_logs", 0777);
#endif
        log_file.open(DIAGNOSTIC_LOG_PATH, std::ios::out | std::ios::trunc);
        initialized = true;
    }

    if (!log_file.is_open()) {
        return;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - diagnostic_log_start_time
    ).count();
    log_file << elapsed_ms << "ms " << message << std::endl;
}

#define FILE_DIAG_LOG(expr) \
    do { \
        if (ENABLE_FILE_DIAGNOSTIC_LOGS) { \
            std::ostringstream diag_oss; \
            diag_oss << expr; \
            write_diagnostic_log(diag_oss.str()); \
        } \
    } while (0)

std::string trim_copy(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string remove_printed_overlap(const std::string& text, const std::string& printed_text) {
    size_t max_overlap = text.size() < printed_text.size() ? text.size() : printed_text.size();
    for (size_t len = max_overlap; len > 0; --len) {
        if (printed_text.compare(printed_text.size() - len, len, text, 0, len) == 0) {
            return trim_copy(text.substr(len));
        }
    }
    return text;
}

static std::vector<std::string> split_alpha_words_lower(const std::string& text) {
    std::vector<std::string> words;
    std::string cur;
    for (char ch : text) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalpha(uch)) {
            cur.push_back(static_cast<char>(std::tolower(uch)));
        } else if (!cur.empty()) {
            words.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) {
        words.push_back(cur);
    }
    return words;
}

static bool is_repetitive_hallucination(const std::string& text) {
    const auto words = split_alpha_words_lower(text);
    if (words.size() < 6) {
        return false;
    }

    int max_run = 1;
    int run = 1;
    for (size_t i = 1; i < words.size(); ++i) {
        if (words[i] == words[i - 1]) {
            run++;
            if (run > max_run) max_run = run;
        } else {
            run = 1;
        }
    }

    if (max_run >= 4) {
        return true;
    }

    int unique_count = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        bool seen = false;
        for (size_t j = 0; j < i; ++j) {
            if (words[j] == words[i]) {
                seen = true;
                break;
            }
        }
        if (!seen) unique_count++;
    }

    return words.size() >= 10 && unique_count <= 2;
}

static bool is_punctuation_only_text(const std::string& text) {
    bool has_punctuation = false;
    for (char ch : text) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isspace(uch)) {
            continue;
        }
        if (!std::ispunct(uch)) {
            return false;
        }
        has_punctuation = true;
    }
    return has_punctuation;
}

AudioStats calculate_audio_stats(const std::vector<float>& samples) {
    AudioStats stats;
    if (samples.empty()) {
        return stats;
    }

    double sum_sq = 0.0;
    for (float s : samples) {
        float abs_s = std::abs(s);
        if (abs_s > stats.peak) stats.peak = abs_s;
        sum_sq += (double)s * (double)s;
    }
    stats.rms = (float)std::sqrt(sum_sq / samples.size());
    return stats;
}

double samples_to_seconds(size_t samples) {
    return (double)samples / 16000.0;
}

void print_subtitle_line(const std::string& label, const std::string& text, long long duration_ms, float peak, bool finalize) {
    std::string line = "[" + label + "] " + utf8_to_ansi(text);
    if (ENABLE_DIAGNOSTIC_LOGS) {
        line += "  (Inference: " + std::to_string(duration_ms) + "ms, Peak: " + std::to_string(peak) + ")";
    }
    
    // 回车并清除从光标到行尾的所有内容，防止长句变短句时残留旧字符
    std::cout << "\r\033[K";
    
    if (!finalize) {
        // 识别中：红色 (Bright Red) 并且不换行
        std::cout << "\033[91m" << line << "\033[0m" << std::flush;
    } else {
        // 识别完成：白色 (Bright White) 并且换行
        std::cout << "\033[97m" << line << "\033[0m" << std::endl;
    }
}

void clear_subtitle_line() {
    std::cout << "\r\033[K" << std::flush;
}

bool should_skip_final_text(const std::string& text, float peak, float rms) {
    if (peak < 0.035f && rms < 0.003f) {
        return true;
    }
    if (text.size() <= 4 && peak < 0.12f) {
        return true;
    }
    return false;
}

// Language Configuration Macro
// Define WHISPER_LANG_ZH to transcribe Chinese, or comment/undefine it to transcribe English.
// #define WHISPER_LANG_ZH

#ifdef _WIN32
// Helper to convert UTF-8 string to Windows local ANSI/GBK string for correct console output
std::string utf8_to_ansi(const std::string& utf8_str) {
    if (utf8_str.empty()) return "";
    int w_len = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, NULL, 0);
    if (w_len <= 0) return utf8_str;
    std::vector<wchar_t> w_buf(w_len);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, w_buf.data(), w_len);

    int a_len = WideCharToMultiByte(CP_ACP, 0, w_buf.data(), -1, NULL, 0, NULL, NULL);
    if (a_len <= 0) return utf8_str;
    std::vector<char> a_buf(a_len);
    WideCharToMultiByte(CP_ACP, 0, w_buf.data(), -1, a_buf.data(), a_len, NULL, NULL);
    return std::string(a_buf.data());
}
#else
std::string utf8_to_ansi(const std::string& utf8_str) {
    return utf8_str;
}
#endif

// Helper function to check if a file exists
bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

// Helper to find model file in common directories
std::string find_model_file(const std::string& filename) {
    // 1. Check current working directory
    if (file_exists(filename)) {
        return filename;
    }
    // 2. Check bin/Release/
    std::string release_path = "bin/Release/" + filename;
    if (file_exists(release_path)) {
        return release_path;
    }
    // 3. Check bin/Debug/
    std::string debug_path = "bin/Debug/" + filename;
    if (file_exists(debug_path)) {
        return debug_path;
    }
    // Fallback to original filename
    return filename;
}

// Helper to save float PCM samples as standard 16-bit mono 16000Hz WAV file
bool save_wav(const std::string& filename, const std::vector<float>& samples, int sample_rate) {
    // Automatically create the "audio_records" directory if saving to it
    if (filename.rfind("audio_records/", 0) == 0) {
#ifdef _WIN32
        _mkdir("audio_records");
#else
        mkdir("audio_records", 0777);
#endif
    }

    std::ofstream out(filename, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    struct WavHeader {
        char chunkId[4] = {'R', 'I', 'F', 'F'};
        uint32_t chunkSize;
        char format[4] = {'W', 'A', 'V', 'E'};
        char subchunk1Id[4] = {'f', 'm', 't', ' '};
        uint32_t subchunk1Size = 16;
        uint16_t audioFormat = 1; // PCM
        uint16_t numChannels = 1; // Mono
        uint32_t sampleRate = 16000;
        uint32_t byteRate = 16000 * 1 * 2;
        uint16_t blockAlign = 1 * 2;
        uint16_t bitsPerSample = 16;
        char subchunk2Id[4] = {'d', 'a', 't', 'a'};
        uint32_t subchunk2Size;
    } header;

    uint32_t num_samples = static_cast<uint32_t>(samples.size());
    header.sampleRate = sample_rate;
    header.byteRate = sample_rate * 1 * 2;
    header.subchunk2Size = num_samples * 2;
    header.chunkSize = 36 + header.subchunk2Size;

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    std::vector<int16_t> int_samples(num_samples);
    for (size_t i = 0; i < num_samples; ++i) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        int_samples[i] = static_cast<int16_t>(s * 32767.0f);
    }

    out.write(reinterpret_cast<const char*>(int_samples.data()), int_samples.size() * sizeof(int16_t));
    return true;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Enable ANSI escape codes in Windows Console
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
#endif

    // 1. Configuration
    std::string vad_model_path = find_model_file("silero_vad.onnx");
#ifdef WHISPER_LANG_ZH
    std::string whisper_model_path = find_model_file("ggml-small.bin");
    std::string language = "zh";
#else
    std::string whisper_model_path = find_model_file("ggml-medium.en.bin");
    if (!file_exists(whisper_model_path)) {
        whisper_model_path = find_model_file("ggml-small.en.bin");
    }
    if (!file_exists(whisper_model_path)) {
        whisper_model_path = find_model_file("ggml-tiny.en.bin");
    }
    std::string language = "en";
#endif

    std::cout << "=======================================================" << std::endl;
    std::cout << "   C++ Real-time Whisper & Silero VAD POC Demo         " << std::endl;
    std::cout << "   Optimized for Physical Whispering                  " << std::endl;
    std::cout << "=======================================================" << std::endl;
    std::cout << "  Whisper Model: " << whisper_model_path << std::endl;
    std::cout << "  VAD Model:     " << vad_model_path << std::endl;
    std::cout << "  Language:      " << (language.empty() ? "auto" : language) << std::endl;
    std::cout << "=======================================================\n" << std::endl;
    FILE_DIAG_LOG("[APP] start"
        << " whisper_model=\"" << whisper_model_path << "\""
        << " vad_model=\"" << vad_model_path << "\""
        << " language=\"" << (language.empty() ? "auto" : language) << "\""
        << " vad_threshold=" << WHISPER_LOW_VAD_THRESHOLD
        << " partial=" << (ENABLE_PARTIAL_TRANSCRIPTION ? "on" : "off"));

    // 2. Initialize Whisper C++
    std::cout << "[Initialization] Loading Whisper model..." << std::endl;
    whisper_log_set(whisper_log_silent, nullptr);
    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;
    cparams.flash_attn = true;
    cparams.gpu_device = 0;

    struct whisper_context* ctx = whisper_init_from_file_with_params(whisper_model_path.c_str(), cparams);
    if (ctx == nullptr) {
        std::cerr << "[Error] Failed to initialize Whisper context." << std::endl;
        return 1;
    }
    std::cout << "[Initialization] Whisper model loaded successfully." << std::endl;
    std::cout << "[Initialization] Whisper backend: " << whisper_print_system_info() << std::endl;

    // 3. Re-enable Silero VAD
    std::cout << "[Initialization] Loading Silero VAD ONNX model..." << std::endl;
    std::unique_ptr<vad::VADStateTracker> vad_tracker;
        try {
            // 针对耳语调低 VAD 阈值，并延长停顿截断，避免一句话被切碎。
            vad_tracker = std::make_unique<vad::VADStateTracker>(
                vad_model_path,
                WHISPER_LOW_VAD_THRESHOLD,
                3,  // min_speech_frames: reduce accidental short trigger for fast whisper
                16  // min_silence_frames: avoid splitting one fast sentence into fragments (16 frames * 32ms = 512ms)
            );
        std::cout << "[Initialization] Silero VAD loaded successfully (Threshold = " << WHISPER_LOW_VAD_THRESHOLD << ")." << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "[Error] Failed to initialize Silero VAD: " << e.what() << std::endl;
        whisper_free(ctx);
        return 1;
    }

    // 4. Initialize WASAPI Recorder
    std::cout << "[Initialization] Initializing Windows WASAPI Recorder..." << std::endl;
    audio::WasapiRecorder recorder;
    if (!recorder.start()) {
        std::cerr << "[Error] Failed to start WASAPI Recorder." << std::endl;
        FILE_DIAG_LOG("[RECORDER] start_failed");
        whisper_free(ctx);
        return 1;
    }
    FILE_DIAG_LOG("[RECORDER] start_ok");

    // 5. Initialize DSP modules
    dsp::HighPassFilter vad_hpf(150.0f, 16000.0f); // 过滤直流偏置和低频地噪

    std::atomic<bool> keep_running{ true };

    // 6. Start processing and transcribing threads
    audio::SafeQueue<TranscriptionJob> transcription_queue;

    std::thread transcription_thread([&]() {
        std::string finalized_text;
        std::string last_partial_text;
        int wav_counter = 0;

        while (keep_running || transcription_queue.size() > 0) {
            TranscriptionJob job;
            if (!transcription_queue.wait_for_pop(job, 100)) {
                continue;
            }
            FILE_DIAG_LOG("[TRANSCRIBE] dequeued"
                << " type=" << (job.is_partial ? "partial" : "final")
                << " samples=" << job.samples.size()
                << " duration=" << samples_to_seconds(job.samples.size()) << "s"
                << " queue_remaining=" << transcription_queue.size());
            if (job.samples.empty()) {
                FILE_DIAG_LOG("[TRANSCRIBE] skip_empty_job");
                continue;
            }

            std::vector<float> raw_samples;
            if (!job.is_partial) {
                raw_samples = job.samples;
            }

            // 1. 高通滤波：在计算峰值和放大前，先过滤低频噪声
            dsp::HighPassFilter local_hpf(150.0f, 16000.0f);
            local_hpf.process_slice(job.samples);

            // 2. 计算滤波后的音频指标
            AudioStats job_stats = calculate_audio_stats(job.samples);

            if (ENABLE_DIAGNOSTIC_LOGS) {
                std::cout << std::endl
                    << "[DIAG][AUDIO] job=" << (job.is_partial ? "partial" : "final")
                    << " duration=" << samples_to_seconds(job.samples.size()) << "s"
                    << " samples=" << job.samples.size()
                    << " peak=" << job_stats.peak
                    << " rms=" << job_stats.rms
                    << " agc=off"
                    << std::endl;
            }

            // 3. 噪声门：如果滤波后的峰值依然极低，说明是纯噪声或呼吸声，跳过识别
            if (job_stats.peak < 0.005f) {
                FILE_DIAG_LOG("[TRANSCRIBE] skip_low_peak"
                    << " type=" << (job.is_partial ? "partial" : "final")
                    << " peak=" << job_stats.peak
                    << " rms=" << job_stats.rms
                    << " samples=" << job.samples.size());
                if (ENABLE_DIAGNOSTIC_LOGS) {
                    std::cout << "[DIAG][AUDIO] skipped: post-HPF peak below 0.005" << std::endl;
                }
                if (!job.is_partial && !ENABLE_DIAGNOSTIC_LOGS) {
                    clear_subtitle_line();
                }
                continue;
            }

            // 4. 动态峰值归一化增益：耳语放大到温和目标峰值，减少过度放大噪声
            float target_peak = 0.45f;
            float scale = target_peak / job_stats.peak;
            if (scale > 12.0f) {
                scale = 12.0f; // 限制最大增益，防止极端放大噪音
            }
            for (float& s : job.samples) {
                s *= scale;
            }
            // 安全裁剪
            for (float& s : job.samples) {
                if (s > 1.0f) s = 1.0f;
                else if (s < -1.0f) s = -1.0f;
            }
            FILE_DIAG_LOG("[TRANSCRIBE] audio_ready"
                << " type=" << (job.is_partial ? "partial" : "final")
                << " peak=" << job_stats.peak
                << " rms=" << job_stats.rms
                << " gain=" << scale
                << " samples=" << job.samples.size());

            /*std::string raw_filename;
            std::string proc_filename;
            if (!job.is_partial) {
                wav_counter++;
                raw_filename = "audio_records/whisper_job_" + std::to_string(wav_counter) + "_raw.wav";
                proc_filename = "audio_records/whisper_job_" + std::to_string(wav_counter) + "_processed.wav";
                save_wav(raw_filename, raw_samples, 16000);
                save_wav(proc_filename, job.samples, 16000);
            }

            if (ENABLE_DIAGNOSTIC_LOGS) {
                std::cout << "[DIAG][AUDIO] applied gain scale=" << scale << "x" << std::endl;
            }*/

            auto start_time = std::chrono::high_resolution_clock::now();

            whisper_full_params params = whisper_full_default_params(
                job.is_partial ? WHISPER_SAMPLING_GREEDY : WHISPER_SAMPLING_BEAM_SEARCH
            );
            params.print_progress = false;
            params.print_special = false;
            params.print_realtime = false;
            params.print_timestamps = false;
            params.no_timestamps = true;
            params.single_segment = true;
            params.no_context = true;
            params.translate = false;
            params.language = language.empty() ? nullptr : language.c_str();
            int worker_threads = (int)std::thread::hardware_concurrency() - 1;
            params.n_threads = worker_threads > 1 ? worker_threads : 1;

#ifdef WHISPER_LANG_ZH
            params.initial_prompt = "物理耳语。";
#else
            params.initial_prompt = nullptr;
#endif
            params.suppress_blank = true;
            params.suppress_nst = true;
            params.temperature_inc = 0.0f;
            params.temperature = 0.0f;
            params.entropy_thold = 2.0f;
            params.logprob_thold = -1.0f;
            params.no_speech_thold = 0.40f;
            params.greedy.best_of = 5;
            params.beam_search.beam_size = 8;
            FILE_DIAG_LOG("[WHISPER] start"
                << " type=" << (job.is_partial ? "partial" : "final")
                << " strategy=" << (job.is_partial ? "greedy" : "beam")
                << " samples=" << job.samples.size()
                << " duration=" << samples_to_seconds(job.samples.size()) << "s"
                << " threads=" << params.n_threads
                << " no_context=" << params.no_context
                << " no_speech_thold=" << params.no_speech_thold);

            if (ENABLE_DIAGNOSTIC_LOGS) {
                std::cout << "[DIAG][WHISPER] strategy=" << (job.is_partial ? "greedy" : "beam")
                    << " beam_size=" << params.beam_search.beam_size
                    << " language=" << language
                    << " no_speech_thold=" << params.no_speech_thold
                    << std::endl;
            }

            int whisper_result = whisper_full(ctx, params, job.samples.data(), (int)job.samples.size());
            if (whisper_result == 0) {
                auto end_time = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
                FILE_DIAG_LOG("[WHISPER] finished"
                    << " type=" << (job.is_partial ? "partial" : "final")
                    << " inference_ms=" << duration
                    << " segments=" << whisper_full_n_segments(ctx));

                std::string transcription;
                const int n_segments = whisper_full_n_segments(ctx);
                float total_token_prob = 0.0f;
                int total_tokens = 0;
                for (int i = 0; i < n_segments; ++i) {
                    float no_speech_prob = whisper_full_get_segment_no_speech_prob(ctx, i);
                    std::string seg_text = whisper_full_get_segment_text(ctx, i);
                    const int n_tokens = whisper_full_n_tokens(ctx, i);
                    float segment_token_prob = 0.0f;
                    for (int j = 0; j < n_tokens; ++j) {
                        float token_prob = whisper_full_get_token_p(ctx, i, j);
                        segment_token_prob += token_prob;
                        total_token_prob += token_prob;
                        total_tokens++;
                    }
                    float avg_token_prob = n_tokens > 0 ? segment_token_prob / n_tokens : 0.0f;

                    if (ENABLE_DIAGNOSTIC_LOGS) {
                        std::cout << "[DIAG][WHISPER] segment=" << i
                            << " no_speech=" << no_speech_prob
                            << " tokens=" << n_tokens
                            << " avg_token_p=" << avg_token_prob
                            << " text=\"" << utf8_to_ansi(trim_copy(seg_text)) << "\""
                            << std::endl;
                    }
                    FILE_DIAG_LOG("[WHISPER] segment"
                        << " index=" << i
                        << " no_speech=" << no_speech_prob
                        << " tokens=" << n_tokens
                        << " avg_token_p=" << avg_token_prob
                        << " text=\"" << trim_copy(seg_text) << "\"");

                    // 5. 手动非言语幻听过滤器
                    std::string trimmed_seg = trim_copy(seg_text);
                    if (trimmed_seg.empty() || is_punctuation_only_text(trimmed_seg) ||
                        trimmed_seg == "[" || trimmed_seg == "]" || trimmed_seg == "(" || trimmed_seg == ")") {
                        FILE_DIAG_LOG("[WHISPER] skip_segment"
                            << " reason=punctuation_or_empty"
                            << " text=\"" << trimmed_seg << "\"");
                        continue;
                    }

                    /*bool is_technical_marker = false;
                    if ((trimmed_seg.front() == '[' && trimmed_seg.back() == ']') ||
                        (trimmed_seg.front() == '(' && trimmed_seg.back() == ')') ||
                        trimmed_seg.find("BLANK_AUDIO") != std::string::npos ||
                        trimmed_seg.find("crying") != std::string::npos ||
                        trimmed_seg.find("inaudible") != std::string::npos ||
                        trimmed_seg.find("umbling") != std::string::npos ||
                        trimmed_seg.find("screaming") != std::string::npos ||
                        trimmed_seg.find("sighing") != std::string::npos ||
                        trimmed_seg.find("gasping") != std::string::npos ||
                        trimmed_seg.find("coughing") != std::string::npos ||
                        trimmed_seg.find("throat clearing") != std::string::npos ||
                        trimmed_seg.find("snorting") != std::string::npos ||
                        trimmed_seg.find("giggle") != std::string::npos ||
                        trimmed_seg.find("giggling") != std::string::npos ||
                        trimmed_seg.find("laughter") != std::string::npos ||
                        trimmed_seg.find("laughing") != std::string::npos ||
                        trimmed_seg.find("snicker") != std::string::npos ||
                        trimmed_seg.find("snickering") != std::string::npos ||
                        trimmed_seg.find("chuckle") != std::string::npos ||
                        trimmed_seg.find("chuckling") != std::string::npos ||
                        trimmed_seg.find("Sounds of") != std::string::npos ||
                        trimmed_seg.find("whisper") != std::string::npos ||
                        trimmed_seg.find("Whisper") != std::string::npos) {
                        is_technical_marker = true;
                    }

                    if (is_technical_marker) {
                        if (ENABLE_DIAGNOSTIC_LOGS) {
                            std::cout << "[DIAG][WHISPER] Filtered hallucination/technical marker: \"" << utf8_to_ansi(trimmed_seg) << "\"" << std::endl;
                        }
                        continue;
                    }*/

                    transcription += seg_text;
                } 

                transcription = trim_copy(transcription);
                if (!job.is_partial) {
                    transcription = remove_printed_overlap(transcription, finalized_text);
                }
                
                if (is_repetitive_hallucination(transcription)) {
                    FILE_DIAG_LOG("[WHISPER] skip_result"
                        << " reason=repetitive_hallucination"
                        << " text=\"" << transcription << "\"");
                    if (ENABLE_DIAGNOSTIC_LOGS) {
                        std::cout << "[DIAG][WHISPER] skipped repetitive hallucination: "
                                  << utf8_to_ansi(transcription) << std::endl;
                    }
                    if (!job.is_partial && !ENABLE_DIAGNOSTIC_LOGS) {
                        clear_subtitle_line();
                    }
                    continue;
                }

                if (!job.is_partial && transcription.size() <= 10 && !finalized_text.empty()) {
                    if (finalized_text.find(transcription) != std::string::npos) {
                        FILE_DIAG_LOG("[WHISPER] skip_result"
                            << " reason=short_duplicate_tail"
                            << " text=\"" << transcription << "\"");
                        if (ENABLE_DIAGNOSTIC_LOGS) {
                            std::cout << "[DIAG][WHISPER] skipped short duplicate tail: "
                                      << utf8_to_ansi(transcription) << std::endl;
                        }
                        if (!ENABLE_DIAGNOSTIC_LOGS) {
                            clear_subtitle_line();
                        }
                        continue;
                    }
                }

                float avg_all_token_prob = total_tokens > 0 ? total_token_prob / total_tokens : 0.0f;
                if (ENABLE_DIAGNOSTIC_LOGS) {
                    std::cout << "[DIAG][WHISPER] result=\""
                        << utf8_to_ansi(transcription)
                        << "\" avg_token_p=" << avg_all_token_prob
                        << " inference_ms=" << duration
                        << std::endl;
                }

                if (!job.is_partial && avg_all_token_prob < 0.50f) {
                    FILE_DIAG_LOG("[WHISPER] skip_result"
                        << " reason=low_token_confidence"
                        << " text=\"" << transcription << "\""
                        << " avg_token_p=" << avg_all_token_prob
                        << " peak=" << job_stats.peak
                        << " rms=" << job_stats.rms);
                    if (ENABLE_DIAGNOSTIC_LOGS) {
                        std::cout << "[DIAG][WHISPER] skipped final by low token confidence" << std::endl;
                    }
                    if (!ENABLE_DIAGNOSTIC_LOGS) {
                        clear_subtitle_line();
                    }
                    continue;
                }

                if (!job.is_partial && should_skip_final_text(transcription, job_stats.peak, job_stats.rms) &&
                    avg_all_token_prob < 0.65f) {
                    FILE_DIAG_LOG("[WHISPER] skip_result"
                        << " reason=short_low_peak"
                        << " text=\"" << transcription << "\""
                        << " peak=" << job_stats.peak
                        << " avg_token_p=" << avg_all_token_prob);
                    if (ENABLE_DIAGNOSTIC_LOGS) {
                        std::cout << "[DIAG][WHISPER] skipped final by short-low-peak filter" << std::endl;
                    }
                    if (!ENABLE_DIAGNOSTIC_LOGS) {
                        clear_subtitle_line();
                    }
                    continue;
                }

                if (!transcription.empty()) {
                    if (job.is_partial) {
                        if (transcription == last_partial_text) {
                            continue;
                        }
                        last_partial_text = transcription;
                    }
                    if (!job.is_partial) {
                        finalized_text += transcription;
                        last_partial_text.clear();
                    }
                    if (!job.is_partial || ENABLE_PARTIAL_TRANSCRIPTION) {
                        FILE_DIAG_LOG("[OUTPUT] printed"
                            << " type=" << (job.is_partial ? "partial" : "final")
                            << " text=\"" << transcription << "\""
                            << " inference_ms=" << duration
                            << " peak=" << job_stats.peak);
                        print_subtitle_line(job.is_partial ? "Whispering..." : "Whispered", transcription, duration, job_stats.peak, !job.is_partial);
                       /* if (!job.is_partial) {
                            std::cout << "  \033[90m[Saved WAV: " << proc_filename << "]\033[0m" << std::endl;
                        }*/
                    }
                }
                else {
                    FILE_DIAG_LOG("[OUTPUT] empty_after_filters"
                        << " type=" << (job.is_partial ? "partial" : "final")
                        << " inference_ms=" << duration
                        << " peak=" << job_stats.peak
                        << " rms=" << job_stats.rms);
                    if (!job.is_partial && !ENABLE_DIAGNOSTIC_LOGS) {
                        clear_subtitle_line();
                    }
                }
            }
            else {
                FILE_DIAG_LOG("[WHISPER] failed"
                    << " type=" << (job.is_partial ? "partial" : "final")
                    << " code=" << whisper_result
                    << " samples=" << job.samples.size());
                if (!job.is_partial && !ENABLE_DIAGNOSTIC_LOGS) {
                    clear_subtitle_line();
                }
            }
        }
        });

    std::thread processing_thread([&]() {
        auto& audio_queue = recorder.get_queue();
        std::vector<float> current_chunk;

        std::cout << "\n>>> Started Listening (GPU Latency Optimized Mode) <<<" << std::endl;

        std::vector<float> vad_accumulation;
        std::vector<float> active_speech_buffer;

        // 前滚缓存
        constexpr size_t MAX_PRE_ROLL_PACKETS = 32; // 约 1s，避免 hi/hello 这类短开头被 VAD 吃掉
        constexpr size_t MAX_ACTIVE_SPEECH_SAMPLES = 16000 * 6; // 6秒强制输出一次，优先保证短句完整性
        constexpr size_t CHUNK_OVERLAP_SAMPLES = 16000 / 2; // 500ms重叠，避免分段切字
        constexpr size_t PARTIAL_UPDATE_SAMPLES = 6400; // 每0.4秒尝试更新一次临时字幕
        constexpr float MAX_SPEECH_TIMEOUT_SEC = 10.0f; // VAD 长时间未给结束态时强制收尾
        FILE_DIAG_LOG("[VAD] config"
            << " frame_size=" << VAD_FRAME_SIZE
            << " max_pre_roll_packets=" << MAX_PRE_ROLL_PACKETS
            << " max_active_speech_sec=" << samples_to_seconds(MAX_ACTIVE_SPEECH_SAMPLES)
            << " partial_update_sec=" << samples_to_seconds(PARTIAL_UPDATE_SAMPLES)
            << " timeout_sec=" << MAX_SPEECH_TIMEOUT_SEC);
        std::deque<std::vector<float>> pre_roll_history;
        size_t last_partial_sample_count = 0;
        size_t vad_frame_counter = 0;
        int last_vad_state = -1;
        bool in_speech = false;
        auto speech_start_time = std::chrono::steady_clock::now();

        while (keep_running) {
            if (audio_queue.wait_for_pop(current_chunk, 100)) {
                AudioStats chunk_stats = calculate_audio_stats(current_chunk);
                FILE_DIAG_LOG("[AUDIO] chunk"
                    << " samples=" << current_chunk.size()
                    << " duration=" << samples_to_seconds(current_chunk.size()) << "s"
                    << " peak=" << chunk_stats.peak
                    << " rms=" << chunk_stats.rms);
                vad_accumulation.insert(vad_accumulation.end(), current_chunk.begin(), current_chunk.end());

                while (vad_accumulation.size() >= VAD_FRAME_SIZE) {
                    std::vector<float> frame(vad_accumulation.begin(), vad_accumulation.begin() + VAD_FRAME_SIZE);
                    vad_accumulation.erase(vad_accumulation.begin(), vad_accumulation.begin() + VAD_FRAME_SIZE);

                    std::vector<float> vad_frame = frame;
                    vad_hpf.process_slice(vad_frame);

                    int vad_state = vad_tracker->feed_frame(vad_frame.data(), vad_frame.size());
                    float vad_prob = vad_tracker->get_last_prob();
                    AudioStats raw_frame_stats = calculate_audio_stats(frame);
                    AudioStats vad_frame_stats = calculate_audio_stats(vad_frame);
                    vad_frame_counter++;

                    bool should_log_vad_frame = vad_state != last_vad_state || vad_frame_counter % 31 == 0;
                    if (should_log_vad_frame) {
                        FILE_DIAG_LOG("[VAD] frame"
                            << " frame=" << vad_frame_counter
                            << " state=" << vad_state
                            << " last_state=" << last_vad_state
                            << " prob=" << vad_prob
                            << " threshold=" << WHISPER_LOW_VAD_THRESHOLD
                            << " raw_peak=" << raw_frame_stats.peak
                            << " raw_rms=" << raw_frame_stats.rms
                            << " vad_peak=" << vad_frame_stats.peak
                            << " vad_rms=" << vad_frame_stats.rms
                            << " active_sec=" << samples_to_seconds(active_speech_buffer.size())
                            << " pre_roll_frames=" << pre_roll_history.size()
                            << " transcription_queue=" << transcription_queue.size());
                    }

                    if (ENABLE_DIAGNOSTIC_LOGS && should_log_vad_frame) {
                        std::cout << std::endl
                            << "[DIAG][VAD] frame=" << vad_frame_counter
                            << " state=" << vad_state
                            << " prob=" << vad_prob
                            << " threshold=" << WHISPER_LOW_VAD_THRESHOLD
                            << " raw_peak=" << raw_frame_stats.peak
                            << " raw_rms=" << raw_frame_stats.rms
                            << " vad_peak=" << vad_frame_stats.peak
                            << " vad_rms=" << vad_frame_stats.rms
                            << " active_sec=" << samples_to_seconds(active_speech_buffer.size())
                            << " pre_roll_frames=" << pre_roll_history.size()
                            << std::endl;
                    }
                    if (should_log_vad_frame) {
                        last_vad_state = vad_state;
                    }

                    if (vad_state == 1) { // 说话开始
                        transcription_queue.clear(); // drop stale partial jobs from previous utterance
                        active_speech_buffer.clear();
                        for (const auto& hist_frame : pre_roll_history) {
                            active_speech_buffer.insert(active_speech_buffer.end(), hist_frame.begin(), hist_frame.end());
                        }
                        pre_roll_history.clear();
                        active_speech_buffer.insert(active_speech_buffer.end(), frame.begin(), frame.end());
                        last_partial_sample_count = active_speech_buffer.size();
                        in_speech = true;
                        speech_start_time = std::chrono::steady_clock::now();
                        FILE_DIAG_LOG("[VAD] speech_start"
                            << " frame=" << vad_frame_counter
                            << " prob=" << vad_prob
                            << " pre_roll_sec=" << samples_to_seconds(active_speech_buffer.size() - frame.size())
                            << " active_sec=" << samples_to_seconds(active_speech_buffer.size()));
                        if (!ENABLE_DIAGNOSTIC_LOGS) {
                            print_subtitle_line("Whispering...", "", 0, 0.0f, false);
                        }
                        if (ENABLE_DIAGNOSTIC_LOGS) {
                            std::cout << "[DIAG][VAD] speech_start pre_roll_sec="
                                << samples_to_seconds(active_speech_buffer.size() - frame.size())
                                << std::endl;
                        }
                        else if (ENABLE_USER_EVENT_LOGS) {
                            //std::cout << "[Listening] whisper detected, transcribing when complete..." << std::endl;
                        }
                    }
                    else if (vad_state == 2) { // 说话持续
                        if (!in_speech) {
                            FILE_DIAG_LOG("[VAD] ignore_continuing_without_start"
                                << " frame=" << vad_frame_counter
                                << " prob=" << vad_prob
                                << " pre_roll_frames=" << pre_roll_history.size());
                            // 忽略未经过 speech_start 的孤立持续态，避免启动瞬间误触发 partial
                            pre_roll_history.push_back(frame);
                            if (pre_roll_history.size() > MAX_PRE_ROLL_PACKETS) {
                                pre_roll_history.pop_front();
                            }
                            continue;
                        }
                        active_speech_buffer.insert(active_speech_buffer.end(), frame.begin(), frame.end());

                        if (ENABLE_PARTIAL_TRANSCRIPTION &&
                            active_speech_buffer.size() >= last_partial_sample_count + PARTIAL_UPDATE_SAMPLES &&
                            transcription_queue.size() == 0) {
                            if (ENABLE_DIAGNOSTIC_LOGS) {
                                AudioStats partial_stats = calculate_audio_stats(active_speech_buffer);
                                std::cout << "[DIAG][AUDIO] enqueue partial duration="
                                    << samples_to_seconds(active_speech_buffer.size()) << "s"
                                    << " peak=" << partial_stats.peak
                                    << " rms=" << partial_stats.rms
                                    << " queue_size=" << transcription_queue.size()
                                    << std::endl;
                            }
                            transcription_queue.push({ active_speech_buffer, true });
                            FILE_DIAG_LOG("[QUEUE] enqueue"
                                << " type=partial"
                                << " duration=" << samples_to_seconds(active_speech_buffer.size()) << "s"
                                << " samples=" << active_speech_buffer.size()
                                << " queue_size=" << transcription_queue.size());
                            last_partial_sample_count = active_speech_buffer.size();
                        }

                        if (ENABLE_PARTIAL_TRANSCRIPTION &&
                            active_speech_buffer.size() >= MAX_ACTIVE_SPEECH_SAMPLES) {
                            if (ENABLE_DIAGNOSTIC_LOGS) {
                                AudioStats forced_stats = calculate_audio_stats(active_speech_buffer);
                                std::cout << "[DIAG][AUDIO] enqueue forced_partial duration="
                                    << samples_to_seconds(active_speech_buffer.size()) << "s"
                                    << " peak=" << forced_stats.peak
                                    << " rms=" << forced_stats.rms
                                    << std::endl;
                            }
                            transcription_queue.push({ active_speech_buffer, true });
                            FILE_DIAG_LOG("[QUEUE] enqueue"
                                << " type=forced_partial"
                                << " duration=" << samples_to_seconds(active_speech_buffer.size()) << "s"
                                << " samples=" << active_speech_buffer.size()
                                << " queue_size=" << transcription_queue.size());
                            if (active_speech_buffer.size() > CHUNK_OVERLAP_SAMPLES) {
                                active_speech_buffer.erase(
                                    active_speech_buffer.begin(),
                                    active_speech_buffer.end() - CHUNK_OVERLAP_SAMPLES
                                );
                            }
                            else {
                                active_speech_buffer.clear();
                            }
                            last_partial_sample_count = active_speech_buffer.size();
                        }

                        float speech_elapsed_sec = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - speech_start_time
                        ).count() / 1000.0f;
                        if (speech_elapsed_sec >= MAX_SPEECH_TIMEOUT_SEC && active_speech_buffer.size() >= 16000 * 0.6f) {
                            AudioStats timeout_stats = calculate_audio_stats(active_speech_buffer);
                            FILE_DIAG_LOG("[VAD] timeout_force_final"
                                << " elapsed=" << speech_elapsed_sec << "s"
                                << " active_sec=" << samples_to_seconds(active_speech_buffer.size())
                                << " peak=" << timeout_stats.peak
                                << " rms=" << timeout_stats.rms
                                << " queue_before_clear=" << transcription_queue.size());
                            if (ENABLE_DIAGNOSTIC_LOGS) {
                                std::cout << "[DIAG][VAD] timeout_force_final elapsed="
                                    << speech_elapsed_sec << "s"
                                    << " peak=" << timeout_stats.peak
                                    << " rms=" << timeout_stats.rms
                                    << std::endl;
                            }
                            transcription_queue.clear();
                            transcription_queue.push({ active_speech_buffer, false });
                            FILE_DIAG_LOG("[QUEUE] enqueue"
                                << " type=timeout_final"
                                << " duration=" << samples_to_seconds(active_speech_buffer.size()) << "s"
                                << " samples=" << active_speech_buffer.size()
                                << " queue_size=" << transcription_queue.size());
                            active_speech_buffer.clear();
                            last_partial_sample_count = 0;
                            in_speech = false;
                            vad_tracker->reset();
                            FILE_DIAG_LOG("[VAD] reset_after_timeout");
                        }
                    }
                    else if (vad_state == 3) { // 说话结束
                        active_speech_buffer.insert(active_speech_buffer.end(), frame.begin(), frame.end());

                        if (active_speech_buffer.size() >= 16000 * 0.6f) { // 至少 0.6 秒有效音
                            AudioStats final_stats = calculate_audio_stats(active_speech_buffer);
                            FILE_DIAG_LOG("[VAD] speech_end"
                                << " frame=" << vad_frame_counter
                                << " prob=" << vad_prob
                                << " duration=" << samples_to_seconds(active_speech_buffer.size()) << "s"
                                << " peak=" << final_stats.peak
                                << " rms=" << final_stats.rms
                                << " queue_before_clear=" << transcription_queue.size());
                            if (ENABLE_USER_EVENT_LOGS) {
                                //std::cout << "[Listening] whisper ended, finalizing..." << std::endl;
                            }
                            if (ENABLE_DIAGNOSTIC_LOGS) {
                                std::cout << "[DIAG][AUDIO] enqueue final duration="
                                    << samples_to_seconds(active_speech_buffer.size()) << "s"
                                    << " peak=" << final_stats.peak
                                    << " rms=" << final_stats.rms
                                    << " queue_size_before_clear=" << transcription_queue.size()
                                    << std::endl;
                            }
                            transcription_queue.clear();
                            transcription_queue.push({ active_speech_buffer, false });
                            FILE_DIAG_LOG("[QUEUE] enqueue"
                                << " type=final"
                                << " duration=" << samples_to_seconds(active_speech_buffer.size()) << "s"
                                << " samples=" << active_speech_buffer.size()
                                << " queue_size=" << transcription_queue.size());
                        }
                        else if (ENABLE_DIAGNOSTIC_LOGS) {
                            std::cout << "[DIAG][VAD] dropped short speech duration="
                                << samples_to_seconds(active_speech_buffer.size()) << "s"
                                << std::endl;
                        }
                        else {
                            FILE_DIAG_LOG("[VAD] drop_short_speech"
                                << " duration=" << samples_to_seconds(active_speech_buffer.size()) << "s"
                                << " samples=" << active_speech_buffer.size());
                            clear_subtitle_line();
                        }
                        active_speech_buffer.clear();
                        last_partial_sample_count = 0;
                        in_speech = false;
                    }
                    else {
                        // 静音时，记录前滚缓存
                        pre_roll_history.push_back(frame);
                        if (pre_roll_history.size() > MAX_PRE_ROLL_PACKETS) {
                            pre_roll_history.pop_front();
                        }
                        active_speech_buffer.clear();
                        last_partial_sample_count = 0;
                        in_speech = false;
                    }
                }
            }
        }
        });

    // 7. Wait for user input to exit
    std::cin.get();

    std::cout << "[Shutdown] Stopping recorder..." << std::endl;
    recorder.stop();

    std::cout << "[Shutdown] Stopping processing thread..." << std::endl;
    keep_running = false;
    if (processing_thread.joinable()) {
        processing_thread.join();
    }
    if (transcription_thread.joinable()) {
        transcription_thread.join();
    }

    std::cout << "[Shutdown] Freeing resources..." << std::endl;
    whisper_free(ctx);

    std::cout << "[Shutdown] Done. Exiting." << std::endl;
    return 0;
}
