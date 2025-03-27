#pragma once

#include "framework.h"
#include "resource.h"
#include "audio_processor.h"
#include <vector>
#include <string>
#include <memory>
#include <commctrl.h>
#include <queue>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <map>

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
    INT_PTR OnDropFiles(WPARAM wParam);

    // 功能实现
    void ImportFile();
    void SplitChannels();
    void UpdateProgress(int progress);
    void UpdateStatus(const std::wstring& status);
    void InitializeControls();
    void ProcessDroppedFolder(const std::wstring& folderPath);
    void AddFileToList(const std::wstring& filePath);

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
    std::map<std::wstring, int> file_progress_; // 存储每个文件的处理进度
    static MainDialog* instance_;
    
    // 工作线程相关
    HANDLE worker_thread_;
    bool is_processing_;
    
    // 线程池相关
    struct FileTask {
        std::wstring file_path;
        std::wstring output_dir;
        AudioProcessor::AudioFormat format;
        AudioProcessor::OutputFormat output_format;
    };
    
    std::queue<FileTask> task_queue_;
    std::mutex task_mutex_;
    std::mutex progress_mutex_; // 用于保护文件进度映射的互斥锁
    std::condition_variable task_cv_;
    std::vector<HANDLE> worker_threads_;
    std::atomic<int> active_threads_;
    std::atomic<int> completed_files_;
    std::atomic<int> error_files_; // 记录处理失败的文件数量
    std::atomic<int> total_files_;
    int max_threads_;
    bool shutdown_threads_;
    
    // 工作线程函数
    static DWORD WINAPI ProcessFilesThreadProc(LPVOID lpParam);
    
    // 工作线程池函数
    static DWORD WINAPI WorkerThreadProc(LPVOID lpParam);
    
    // 处理单个文件
    bool ProcessSingleFile(const std::wstring& file, const std::wstring& output_dir, AudioProcessor::AudioFormat& format, AudioProcessor::OutputFormat output_format);
    
    // 初始化线程池
    void InitThreadPool(int thread_count);
    
    // 关闭线程池
    void ShutdownThreadPool();
    
    // 添加任务到队列
    void AddTask(const FileTask& task);
    
    // 获取任务从队列
    bool GetTask(FileTask& task);
};