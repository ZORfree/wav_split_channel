#pragma once

#include "framework.h"
#include "resource.h"
#include "audio_processor.h"
#include <vector>
#include <string>
#include <memory>
#include <commctrl.h>

class MainDialog {
public:
    MainDialog();
    ~MainDialog();

    // 创建并显示主窗口
    bool Create(HINSTANCE hInstance);

    // 获取窗口句柄
    HWND GetHwnd() const { return hwnd_; }

    // 对话框过程函数
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    // 消息处理函数
    INT_PTR OnInitDialog();
    INT_PTR OnCommand(WPARAM wParam, LPARAM lParam);
    INT_PTR OnNotify(WPARAM wParam, LPARAM lParam);
    INT_PTR OnTimer(WPARAM wParam);

    // 功能实现
    void ImportFile();
    void SplitChannels();
    void UpdateProgress(int progress);
    void UpdateStatus(const std::wstring& status);
    void InitializeControls();

    // 设置相关
    void LoadSettings();
    void SaveSettings();

    // 成员变量
    HWND hwnd_;
    HWND list_view_;
    HWND progress_bar_;
    HWND status_text_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::vector<std::wstring> file_list_;
    static MainDialog* instance_;
    
    // 工作线程相关
    HANDLE worker_thread_;
    bool is_processing_;
    
    // 工作线程函数
    static DWORD WINAPI ProcessFilesThreadProc(LPVOID lpParam);
    
    // 处理单个文件
    bool ProcessSingleFile(const std::wstring& file, const std::wstring& output_dir, AudioProcessor::AudioFormat& format, AudioProcessor::OutputFormat output_format);
};