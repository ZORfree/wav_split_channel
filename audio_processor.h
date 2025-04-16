#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <functional>

// WAV文件头结构
struct WAVHeader {
    // RIFF块
    char riff_id[4];        // "RIFF"
    uint32_t file_size;     // 文件总大小 - 8
    char wave_id[4];        // "WAVE"
    
    // fmt子块
    char fmt_id[4];         // "fmt "
    uint32_t fmt_size;      // fmt块大小: 16
    uint16_t audio_format;  // 音频格式: 1 = PCM
    uint16_t num_channels;  // 通道数
    uint32_t sample_rate;   // 采样率
    uint32_t byte_rate;     // 字节率
    uint16_t block_align;   // 块对齐
    uint16_t bits_per_sample; // 采样位数
    
    // data子块
    char data_id[4];        // "data"
    uint32_t data_size;     // 音频数据大小
};

class AudioProcessor {
public:
    // 音频格式设置
    struct AudioFormat {
        uint32_t sample_rate = 16000;    // 采样率
        uint16_t bits_per_sample = 16;   // 采样位数
        uint16_t num_channels = 2;       // 通道数
    };

    AudioProcessor();
    ~AudioProcessor();

    // 加载WAV文件
    bool LoadWavFile(const std::wstring& file_path);
    
    // 加载PCM文件
    bool LoadPcmFile(const std::wstring& file_path);
    
    // 拆分通道
    bool SplitChannels(const std::wstring& output_dir, const std::wstring& suffix = L"");
    
    // 获取和设置音频格式
    void SetAudioFormat(const AudioFormat& format);
    AudioFormat GetAudioFormat() const;
    
    // 输出格式枚举
    enum class OutputFormat {
        WAV,
        PCM
    };
    
    // 设置输出格式
    void SetOutputFormat(OutputFormat format);
    
    // 获取输出格式
    OutputFormat GetOutputFormat() const;
    
    // 进度回调函数类型定义
    using ProgressCallback = std::function<void(int)>;

    // 设置进度回调函数
    void SetProgressCallback(ProgressCallback callback) {
        progress_callback_ = callback;
    }

    // 获取处理进度（0-100）
    int GetProgress() const;

private:
    OutputFormat output_format_ = OutputFormat::WAV;
    std::unique_ptr<WAVHeader> wav_header_;
    std::vector<uint8_t> audio_data_;
    AudioFormat audio_format_;
    int progress_;
    std::wstring file_path_;
    ProgressCallback progress_callback_;

    // 写入单通道WAV文件
    bool WriteSingleChannelWav(const std::wstring& file_path, 
                              const std::vector<uint8_t>& channel_data,
                              int channel_index);
};