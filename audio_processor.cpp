#include "audio_processor.h"
#include <fstream>
#include <filesystem>
#include <algorithm>

AudioProcessor::AudioProcessor() : progress_(0) {
    wav_header_ = std::make_unique<WAVHeader>();
    file_path_.clear();
}

AudioProcessor::~AudioProcessor() = default;

bool AudioProcessor::LoadWavFile(const std::wstring& file_path) {
    file_path_ = file_path;
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // 读取文件头部来判断格式
    char header[4];
    file.read(header, 4);
    file.seekg(0); // 重置文件指针

    // 检查是否为WAV格式（RIFF标识）
    if (std::string(header, 4) == "RIFF") {
        // 读取WAV文件头
        file.read(reinterpret_cast<char*>(wav_header_.get()), sizeof(WAVHeader));

        // 验证WAV文件格式
        if (std::string(wav_header_->riff_id, 4) != "RIFF" ||
            std::string(wav_header_->wave_id, 4) != "WAVE" ||
            std::string(wav_header_->fmt_id, 4) != "fmt " ||
            std::string(wav_header_->data_id, 4) != "data") {
            return false;
        }

        // 更新音频格式
        audio_format_.sample_rate = wav_header_->sample_rate;
        audio_format_.bits_per_sample = wav_header_->bits_per_sample;
        audio_format_.num_channels = wav_header_->num_channels;

        // 读取音频数据
        audio_data_.resize(wav_header_->data_size);
        file.read(reinterpret_cast<char*>(audio_data_.data()), wav_header_->data_size);

        return true;
    } else {
        // 如果不是WAV格式，尝试作为PCM格式加载
        return LoadPcmFile(file_path);
    }


}

bool AudioProcessor::LoadPcmFile(const std::wstring& file_path) {
    file_path_ = file_path;
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    std::streampos file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // 计算数据大小
    int bytes_per_sample = audio_format_.bits_per_sample / 8;
    int block_align = bytes_per_sample * audio_format_.num_channels;
    if (file_size % block_align != 0) {
        return false; // 文件大小必须是块对齐的整数倍
    }

    // 创建WAV头
    wav_header_ = std::make_unique<WAVHeader>();
    std::memcpy(wav_header_->riff_id, "RIFF", 4);
    std::memcpy(wav_header_->wave_id, "WAVE", 4);
    std::memcpy(wav_header_->fmt_id, "fmt ", 4);
    std::memcpy(wav_header_->data_id, "data", 4);

    wav_header_->fmt_size = 16;
    wav_header_->audio_format = 1; // PCM格式
    wav_header_->num_channels = audio_format_.num_channels;
    wav_header_->sample_rate = audio_format_.sample_rate;
    wav_header_->bits_per_sample = audio_format_.bits_per_sample;
    wav_header_->block_align = block_align;
    wav_header_->byte_rate = wav_header_->sample_rate * block_align;
    wav_header_->data_size = static_cast<uint32_t>(file_size);
    wav_header_->file_size = static_cast<uint32_t>(file_size) + sizeof(WAVHeader) - 8;

    // 读取PCM数据
    audio_data_.resize(file_size);
    file.read(reinterpret_cast<char*>(audio_data_.data()), file_size);

    return true;
}

bool AudioProcessor::SplitChannels(const std::wstring& output_dir) {
    if (audio_data_.empty() || !wav_header_) {
        return false;
    }

    // 使用用户设置的音频参数
    int num_channels = audio_format_.num_channels;
    int bytes_per_sample = audio_format_.bits_per_sample / 8;
    int block_align = bytes_per_sample * num_channels;
    int samples_per_channel = audio_data_.size() / block_align;

    // 为每个通道创建数据缓冲区，确保每次都是全新的缓冲区
    std::vector<std::vector<uint8_t>> channel_data(num_channels);
    for (auto& channel : channel_data) {
        // 每个通道的数据大小应该是样本数乘以每个样本的字节数
        channel.clear(); // 确保缓冲区为空
        channel.reserve(samples_per_channel * bytes_per_sample);
    }

    // 分离通道数据
    for (int i = 0; i < samples_per_channel; ++i) {
        for (int ch = 0; ch < num_channels; ++ch) {
            // 计算当前样本中该通道数据的起始位置
            int src_pos = i * block_align + ch * bytes_per_sample;
            // 只复制该通道的数据
            for (int b = 0; b < bytes_per_sample; ++b) {
                channel_data[ch].push_back(audio_data_[src_pos + b]);
            }
        }
        // 每处理1%的数据更新一次进度，避免过于频繁的更新
        int current_progress = static_cast<int>((i + 1) * 100 / samples_per_channel);
        if (current_progress != progress_) {
            progress_ = current_progress;
            if (progress_callback_) {
                progress_callback_(progress_);
            }
        }
    }

    // 写入每个通道的WAV文件
    std::filesystem::path input_path(file_path_);
    std::wstring stem = input_path.stem().wstring();
    std::wstring parent_path = input_path.parent_path().wstring();
    
    for (int ch = 0; ch < num_channels; ++ch) {
        std::wstring output_path = (std::filesystem::path(parent_path) / 
            (stem + L"_channel" + std::to_wstring(ch + 1) + 
             (output_format_ == OutputFormat::WAV ? L".wav" : L".pcm"))).wstring();
        if (!WriteSingleChannelWav(output_path, channel_data[ch], ch)) {
            return false;
        }
    }

    return true;
}

bool AudioProcessor::WriteSingleChannelWav(const std::wstring& file_path,
                                          const std::vector<uint8_t>& channel_data,
                                          int channel_index) {
    std::ofstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    if (output_format_ == OutputFormat::WAV) {
        // 创建单通道WAV文件头
        WAVHeader single_channel_header = *wav_header_;
        single_channel_header.num_channels = 1;
        // 正确计算单通道的block_align和byte_rate
        single_channel_header.block_align = single_channel_header.bits_per_sample / 8 * single_channel_header.num_channels; // 确保block_align正确反映单通道
        single_channel_header.byte_rate = single_channel_header.sample_rate * single_channel_header.block_align;
        single_channel_header.data_size = static_cast<uint32_t>(channel_data.size());
        single_channel_header.file_size = single_channel_header.data_size + sizeof(WAVHeader) - 8;

        // 写入文件头
        file.write(reinterpret_cast<const char*>(&single_channel_header), sizeof(WAVHeader));
    }

    // 写入音频数据
    file.write(reinterpret_cast<const char*>(channel_data.data()), channel_data.size());

    return true;
}

void AudioProcessor::SetAudioFormat(const AudioFormat& format) {
    audio_format_ = format;
}

AudioProcessor::AudioFormat AudioProcessor::GetAudioFormat() const {
    return audio_format_;
}

int AudioProcessor::GetProgress() const {
    return progress_;
}

void AudioProcessor::SetOutputFormat(OutputFormat format) {
    output_format_ = format;
}

AudioProcessor::OutputFormat AudioProcessor::GetOutputFormat() const {
    return output_format_;
}