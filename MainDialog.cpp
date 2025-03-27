#include "MainDialog.h"
#include <shobjidl.h> 
#include <shlwapi.h>
#include <windowsx.h>
#include <filesystem>
#include <commctrl.h>
#include <map>


MainDialog* MainDialog::instance_ = nullptr;

MainDialog::MainDialog() : hwnd_(nullptr), audio_processor_(std::make_unique<AudioProcessor>()), 
    worker_thread_(nullptr), is_processing_(false), active_threads_(0), completed_files_(0),
    error_files_(0), total_files_(0), max_threads_(1), shutdown_threads_(false) {
    instance_ = this;
}
// 
MainDialog::~MainDialog() {
    // 确保工作线程已经结束
    if (worker_thread_ != nullptr) {
        // 等待线程结束，最多等待5秒
        WaitForSingleObject(worker_thread_, 5000);
        CloseHandle(worker_thread_);
        worker_thread_ = nullptr;
    }
    
    // 关闭线程池
    ShutdownThreadPool();
    
    instance_ = nullptr;
}
// 创建GUI对话框
bool MainDialog::Create(HINSTANCE hInstance) {
    hwnd_ = CreateDialogParam(hInstance, MAKEINTRESOURCE(IDD_WAVSPLITCHANNEL_DIALOG), nullptr,
                            DialogProc, reinterpret_cast<LPARAM>(this));
    if (!hwnd_) {
        DWORD error = GetLastError();
        WCHAR errorMsg[256];
        wsprintf(errorMsg, L"创建对话框失败，错误代码：%d", error);
        MessageBox(nullptr, errorMsg, L"错误", MB_OK | MB_ICONERROR);
        return false;
    }

    InitializeControls();
    LoadSettings();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

// 定义列表项更新结构体
struct ListItemUpdate {
    int item_index;
    int sub_item_index;
    std::wstring* text;
};
// GUI界面控制
INT_PTR CALLBACK MainDialog::DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtr(hwnd, DWLP_USER, lParam);
        return TRUE;
    }

    MainDialog* dlg = reinterpret_cast<MainDialog*>(GetWindowLongPtr(hwnd, DWLP_USER));
    if (!dlg) {
        return FALSE;
    }

    switch (message) {
        case WM_COMMAND:
            return dlg->OnCommand(wParam, lParam);
        case WM_NOTIFY:
            return dlg->OnNotify(wParam, lParam);
        case WM_TIMER:
            return dlg->OnTimer(wParam);
        case WM_CLOSE:
            dlg->SaveSettings();
            DestroyWindow(hwnd);
            return TRUE;
        case WM_DESTROY:
            PostQuitMessage(0);
            return TRUE;
        // 处理工作线程发送的状态更新消息
        case WM_USER + 2: {
            // 更新状态文本
            std::wstring* status_msg = reinterpret_cast<std::wstring*>(wParam);
            if (status_msg) {
                dlg->UpdateStatus(*status_msg);
                delete status_msg; // 释放内存
            }
            return TRUE;
        }
        // 处理工作线程发送的错误消息
        case WM_USER + 3: {
            // 显示错误消息框
            // std::wstring* error_msg = reinterpret_cast<std::wstring*>(wParam);
            // if (error_msg) {
            //     MessageBox(hwnd, error_msg->c_str(), L"错误", MB_OK | MB_ICONERROR);
            //     delete error_msg; // 释放内存
            // }
            return TRUE;
        }
        // 处理列表项更新消息
        case WM_USER + 4: {
            // 更新列表项
            ListItemUpdate* update_info = reinterpret_cast<ListItemUpdate*>(wParam);
            if (update_info && update_info->text) {
                // 设置列表项文本
                LVITEM lvi = { 0 };
                lvi.mask = LVIF_TEXT;
                lvi.iItem = update_info->item_index;
                lvi.iSubItem = update_info->sub_item_index;
                lvi.pszText = const_cast<LPWSTR>(update_info->text->c_str());
                ListView_SetItem(dlg->list_view_, &lvi);
                
                // 释放内存
                delete update_info->text;
                delete update_info;
            }
            return TRUE;
        }
    }
    return FALSE;
}
// 程序入口
INT_PTR MainDialog::OnInitDialog() {
    InitializeControls();
    return static_cast<INT_PTR>(TRUE);
}
// GUI界面按钮功能
INT_PTR MainDialog::OnCommand(WPARAM wParam, LPARAM lParam) {
    switch (LOWORD(wParam)) {
        case IDM_IMPORT:
            ImportFile();
            return static_cast<INT_PTR>(TRUE);
        case IDM_SPLIT:
            SplitChannels();
            return static_cast<INT_PTR>(TRUE);
        case IDM_EXIT:
            SendMessage(hwnd_, WM_CLOSE, 0, 0);
            return static_cast<INT_PTR>(TRUE);
    }
    return static_cast<INT_PTR>(FALSE);
}
// 鼠标右键功能
INT_PTR MainDialog::OnNotify(WPARAM wParam, LPARAM lParam) {
    LPNMHDR pnmh = reinterpret_cast<LPNMHDR>(lParam);
    if (pnmh->idFrom == IDC_FILE_LIST) {
        switch (pnmh->code) {
            case NM_RCLICK: {
                // 获取选中项数量
                int selectedCount = ListView_GetSelectedCount(list_view_);
                if (selectedCount == 0) return TRUE;

                // 获取鼠标点击位置
                POINT pt;
                GetCursorPos(&pt);

                // 创建弹出菜单
                HMENU hPopupMenu = CreatePopupMenu();
                WCHAR menuText[32];
                swprintf_s(menuText, L"删除选中的%d个文件", selectedCount);
                AppendMenu(hPopupMenu, MF_STRING, 1, menuText);

                // 显示菜单并处理选择
                int cmd = TrackPopupMenu(hPopupMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                        pt.x, pt.y, 0, hwnd_, nullptr);
                if (cmd == 1) {
                    // 从后向前删除选中项，以避免索引变化的问题
                    std::vector<int> selectedItems;
                    int item = -1;
                    while ((item = ListView_GetNextItem(list_view_, item, LVNI_SELECTED)) != -1) {
                        selectedItems.push_back(item);
                    }

                    // 按索引从大到小排序
                    std::sort(selectedItems.rbegin(), selectedItems.rend());

                    // 删除选中的项
                    for (int index : selectedItems) {
                        ListView_DeleteItem(list_view_, index);
                        file_list_.erase(file_list_.begin() + index);
                    }
                }

                DestroyMenu(hPopupMenu);
                return static_cast<INT_PTR>(TRUE);
            }
        }
    }
    return static_cast<INT_PTR>(FALSE);
}
//定时器, 不再更新状态文本
INT_PTR MainDialog::OnTimer(WPARAM wParam) {
    if (wParam == 1) { // 进度更新定时器
        // 计算总体进度
        int overall_progress = 0;
        if (total_files_ > 0) {
            // 计算所有文件的总进度
            double total_progress = 0.0;
            int file_count = 0;
            
            // 锁定进度映射，防止多线程冲突
            std::lock_guard<std::mutex> lock(progress_mutex_);
            
            // 遍历所有文件的进度
            for (const auto& progress_pair : file_progress_) {
                // 将每个文件的进度百分比转换为小数后累加
                total_progress += progress_pair.second / 100.0;
                file_count++;
            }
            
            // 计算总体进度：所有文件进度之和除以文件总数
            if (file_count > 0) {
                // 将总进度转换为百分比
                overall_progress = static_cast<int>((total_progress / total_files_) * 100);
            } else {
                // 如果没有文件进度信息，使用已完成文件数计算
                overall_progress = static_cast<int>((completed_files_ * 100) / total_files_);
            }
            
            // 确保进度不超过100%
            if (overall_progress > 100) overall_progress = 100;
        }
        
        // 更新进度条
        UpdateProgress(overall_progress);
        
        // 更新状态文本 - 显示更详细的进度信息
        // std::wstring status = L"正在处理文件... " + std::to_wstring(completed_files_) + L"/" + std::to_wstring(total_files_);
        //status += L" (" + std::to_wstring(overall_progress) + L"%)";
        //if (active_threads_ > 0) {
        //    status += L" - 活动线程: " + std::to_wstring(active_threads_);
        //}
        // UpdateStatus(status);
        
        // 检查工作线程是否完成
        if (is_processing_ && worker_thread_ != nullptr) {
            DWORD exitCode = 0;
            if (GetExitCodeThread(worker_thread_, &exitCode) && exitCode != STILL_ACTIVE) {
                // 主线程已完成，检查是否所有工作线程也已完成
                if (active_threads_ == 0) {
                    // 关闭线程池
                    ShutdownThreadPool();
                    
                    // 清理资源
                    CloseHandle(worker_thread_);
                    worker_thread_ = nullptr;
                    is_processing_ = false;
                    KillTimer(hwnd_, 1);
                    
                    // 计算总耗时
                    // LARGE_INTEGER end_time, frequency;
                    // QueryPerformanceCounter(&end_time);
                    // QueryPerformanceFrequency(&frequency);
                    
                    // // 更新状态
                    // std::wstring complete_status = L"处理完成，共处理 " + std::to_wstring(completed_files_) + L" 个文件";
                    // UpdateStatus(complete_status);
                }
            }
        }
        
        return static_cast<INT_PTR>(TRUE);
    }
    return static_cast<INT_PTR>(FALSE);
}
//导入按钮
void MainDialog::ImportFile() {
    IFileOpenDialog* pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                 IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        // 设置文件类型过滤器
        COMDLG_FILTERSPEC fileTypes[] = {
            { L"Audio Files", L"*.wav;*.pcm" },
            { L"WAV Files", L"*.wav" },
            { L"PCM Files", L"*.pcm" },
            { L"All Files", L"*.*" }
        };
        pFileOpen->SetFileTypes(4, fileTypes);

        // 启用多选功能
        DWORD dwFlags;
        pFileOpen->GetOptions(&dwFlags);
        pFileOpen->SetOptions(dwFlags | FOS_ALLOWMULTISELECT);

        // 显示对话框
        hr = pFileOpen->Show(hwnd_);
        if (SUCCEEDED(hr)) {
            IShellItemArray* pItemArray;
            hr = pFileOpen->GetResults(&pItemArray);
            if (SUCCEEDED(hr)) {
                DWORD count;
                hr = pItemArray->GetCount(&count);
                if (SUCCEEDED(hr)) {
                    for (DWORD i = 0; i < count; i++) {
                        IShellItem* pItem;
                        hr = pItemArray->GetItemAt(i, &pItem);
                        if (SUCCEEDED(hr)) {
                            PWSTR filePath;
                            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
                            if (SUCCEEDED(hr)) {
                                // 添加文件到列表
                                file_list_.push_back(filePath);
                                
                                // 更新列表视图
                                LVITEM lvi = { 0 };
                                lvi.mask = LVIF_TEXT;
                                lvi.iItem = ListView_GetItemCount(list_view_);
                                lvi.iSubItem = 0;
                                lvi.pszText = PathFindFileName(filePath);
                                ListView_InsertItem(list_view_, &lvi);

                                // 获取文件大小并格式化
                                WIN32_FILE_ATTRIBUTE_DATA fileAttr;
                                if (GetFileAttributesEx(filePath, GetFileExInfoStandard, &fileAttr)) {
                                    ULARGE_INTEGER fileSize;
                                    fileSize.HighPart = fileAttr.nFileSizeHigh;
                                    fileSize.LowPart = fileAttr.nFileSizeLow;
                                    
                                    WCHAR sizeBuf[32];
                                    if (fileSize.QuadPart < 1024) {
                                        swprintf_s(sizeBuf, L"%llu B", fileSize.QuadPart);
                                    } else if (fileSize.QuadPart < 1024 * 1024) {
                                        swprintf_s(sizeBuf, L"%.2f KB", fileSize.QuadPart / 1024.0);
                                    } else if (fileSize.QuadPart < 1024 * 1024 * 1024) {
                                        swprintf_s(sizeBuf, L"%.2f MB", fileSize.QuadPart / (1024.0 * 1024.0));
                                    } else {
                                        swprintf_s(sizeBuf, L"%.2f GB", fileSize.QuadPart / (1024.0 * 1024.0 * 1024.0));
                                    }

                                    lvi.iSubItem = 1;
                                    lvi.pszText = sizeBuf;
                                    ListView_SetItem(list_view_, &lvi);
                                }

                                CoTaskMemFree(filePath);
                            }
                            pItem->Release();
                        }
                    }
                }
                pItemArray->Release();
            }
        }
        pFileOpen->Release();
    }
}
//开始处理按钮
void MainDialog::SplitChannels() {
    if (file_list_.empty()) {
        MessageBox(hwnd_, L"请先导入文件", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 重置所有文件进度为0%
    for (int i = 0; i < file_list_.size(); ++i) {
        // 创建进度更新结构体
        ListItemUpdate* update_info = new ListItemUpdate;
        update_info->item_index = i;
        update_info->sub_item_index = 2; // 进度列
        update_info->text = new std::wstring(L"0%");
        
        // 发送消息更新列表项
        PostMessage(hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
    }

    // 清空进度记录
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        file_progress_.clear();
    }

    // 如果已经在处理中，不要重复启动
    if (is_processing_) {
        MessageBox(hwnd_, L"正在处理文件，请等待当前任务完成", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 设置进度回调函数
    audio_processor_->SetProgressCallback([this](int progress) {
        // 使用PostMessage异步更新进度，避免阻塞处理线程
        PostMessage(hwnd_, WM_USER + 1, static_cast<WPARAM>(progress), 0);
    });

    // 添加消息处理
    SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    
    // 使用SetWindowSubclass函数进行窗口子类化
    SetWindowSubclass(hwnd_, [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*idSubclass*/, DWORD_PTR dwRefData) -> LRESULT {
        if (msg == WM_USER + 1) {
            if (MainDialog* dlg = reinterpret_cast<MainDialog*>(GetWindowLongPtr(hwnd, GWLP_USERDATA))) {
                dlg->UpdateProgress(static_cast<int>(wParam));
            }
            return 0;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }, 1, reinterpret_cast<DWORD_PTR>(this));

    // 开始计时
    LARGE_INTEGER frequency, start_time;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);

    // 获取音频参数设置
    HWND sample_rate_combo = GetDlgItem(hwnd_, IDC_SAMPLE_RATE);
    HWND bits_per_sample_combo = GetDlgItem(hwnd_, IDC_BITS_PER_SAMPLE);
    HWND num_channels_combo = GetDlgItem(hwnd_, IDC_NUM_CHANNELS);
    HWND num_thread_combo = GetDlgItem(hwnd_, IDC_THREAD_COUNT);

    AudioProcessor::AudioFormat format;
    int sample_rate_index = ComboBox_GetCurSel(sample_rate_combo);
    int bits_per_sample_index = ComboBox_GetCurSel(bits_per_sample_combo);
    int num_channels = GetDlgItemInt(hwnd_, IDC_NUM_CHANNELS, nullptr, FALSE);

    // 设置采样率
    switch (sample_rate_index) {
        case 0: format.sample_rate = 8000; break;
        case 1: format.sample_rate = 16000; break;
        case 2: format.sample_rate = 22050; break;
        case 3: format.sample_rate = 44100; break;
        case 4: format.sample_rate = 48000; break;
        default: format.sample_rate = 16000;
    }

    // 设置位深度
    switch (bits_per_sample_index) {
        case 0: format.bits_per_sample = 8; break;
        case 1: format.bits_per_sample = 16; break;
        case 2: format.bits_per_sample = 24; break;
        case 3: format.bits_per_sample = 32; break;
        default: format.bits_per_sample = 16;
    }

    // 设置通道数，若没有设置，默认为2通道
    format.num_channels = num_channels > 0 ? num_channels : 2;
    audio_processor_->SetAudioFormat(format);

    // 重置进度条
    SendMessage(progress_bar_, PBM_SETPOS, 0, 0);
    
    // 获取输出格式设置
    HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
    int output_format = ComboBox_GetCurSel(output_format_combo);
    AudioProcessor::OutputFormat audio_output_format = output_format == 0 ? 
        AudioProcessor::OutputFormat::WAV : AudioProcessor::OutputFormat::PCM;
    audio_processor_->SetOutputFormat(audio_output_format);
    
    // 获取线程数设置，默认为5
    int thread_count = GetDlgItemInt(hwnd_, IDC_THREAD_COUNT, nullptr, FALSE);
    if (thread_count <= 0) {
        thread_count = 5; // 默认使用5个线程
    }
    
    // 标记为正在处理
    is_processing_ = true;
    UpdateStatus(L"开始处理文件...");
    
    // 初始化线程池
    InitThreadPool(thread_count);
    
    // 重置计数器
    completed_files_ = 0;
    error_files_ = 0;
    total_files_ = static_cast<int>(file_list_.size());
    
    // 创建工作线程处理文件
    worker_thread_ = CreateThread(
        nullptr,                   // 默认安全属性
        0,                          // 默认堆栈大小
        ProcessFilesThreadProc,     // 线程函数
        this,                       // 线程参数
        0,                          // 默认创建标志
        nullptr                    // 不接收线程ID
    );
    
    if (worker_thread_ == nullptr) {
        is_processing_ = false;
        ShutdownThreadPool();
        MessageBox(hwnd_, L"创建工作线程失败", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    
    // 启动定时器，每200ms，定期更新UI
    SetTimer(hwnd_, 1, 200, nullptr);
}

// 开始处理按钮，启动的工作线程，处理所有文件
DWORD WINAPI MainDialog::ProcessFilesThreadProc(LPVOID lpParam) {
    MainDialog* dlg = static_cast<MainDialog*>(lpParam);
    if (!dlg) return 1;
    
    // 开始计时
    LARGE_INTEGER frequency, start_time;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    
    // 获取音频参数设置
    AudioProcessor::AudioFormat format = dlg->audio_processor_->GetAudioFormat();
    AudioProcessor::OutputFormat output_format = dlg->audio_processor_->GetOutputFormat();
    
    // 更新状态
    // PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(L"准备处理文件...")), 0);
    
    // 将所有文件添加到任务队列
    for (const auto& file : dlg->file_list_) {
        // 获取输入文件的目录作为输出目录
        std::wstring output_dir = std::filesystem::path(file).parent_path().wstring();
        
        // 创建任务并添加到队列
        FileTask task;
        task.file_path = file;
        task.output_dir = output_dir;
        task.format = format;
        task.output_format = output_format;
        
        dlg->AddTask(task);
    }
    
    // 更新状态
    // PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(L"正在处理文件...")), 0);
    
    // 等待所有任务完成
    while (dlg->completed_files_ < dlg->total_files_ && !dlg->shutdown_threads_) {
        Sleep(200); // 每100毫秒检查一次
        
        // 更新总体进度
        int overall_progress = static_cast<int>((dlg->completed_files_ * 100) / dlg->total_files_);
        PostMessage(dlg->hwnd_, WM_USER + 1, static_cast<WPARAM>(overall_progress), 0);
        
        // 更新状态文本，包含成功和失败文件数量
        std::wstring status = L"正在处理文件... " + std::to_wstring(dlg->completed_files_) + L"/" + std::to_wstring(dlg->total_files_);
        if (dlg->error_files_ > 0) {
            status += L" (成功: " + std::to_wstring(dlg->completed_files_ - dlg->error_files_) + 
                     L", 失败: " + std::to_wstring(dlg->error_files_) + L")";
        }
        PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status)), 0);
    }
    
    // 计算总耗时
    LARGE_INTEGER end_time;
    QueryPerformanceCounter(&end_time);
    double elapsed_seconds = static_cast<double>(end_time.QuadPart - start_time.QuadPart) / frequency.QuadPart;
    
    // 更新状态，包含成功和失败文件数量的详细统计
    std::wstring status = L"处理完成";
    status += L"，总耗时：" + std::to_wstring(static_cast<int>(elapsed_seconds)) + L" 秒";
    status += L"，共处理 " + std::to_wstring(dlg->completed_files_) + L" 个文件";
    if (dlg->error_files_ > 0) {
        status += L" (成功: " + std::to_wstring(dlg->completed_files_ - dlg->error_files_) + 
                 L", 失败: " + std::to_wstring(dlg->error_files_) + L")";
    }
    PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status)), 0);
    
    return 0;
}

// 添加任务到队列
void MainDialog::AddTask(const FileTask& task) {
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        task_queue_.push(task);
    }
    task_cv_.notify_one();
}

// 获取任务从队列
bool MainDialog::GetTask(FileTask& task) {
    std::unique_lock<std::mutex> lock(task_mutex_);
    
    // 等待任务或关闭信号
    task_cv_.wait(lock, [this] {
        return !task_queue_.empty() || shutdown_threads_;
    });
    
    // 如果收到关闭信号且队列为空，返回false
    if (task_queue_.empty()) {
        return false;
    }
    
    // 获取任务
    task = task_queue_.front();
    task_queue_.pop();
    return true;
}

// 工作线程函数，处理任务队列中的文件
DWORD WINAPI MainDialog::WorkerThreadProc(LPVOID lpParam) {
    MainDialog* dlg = static_cast<MainDialog*>(lpParam);
    if (!dlg) return 1;
    
    // 创建每个线程自己的AudioProcessor实例
    std::unique_ptr<AudioProcessor> thread_audio_processor = std::make_unique<AudioProcessor>();
    
    // 设置进度回调函数 - 这个初始回调只用于调试目的
    thread_audio_processor->SetProgressCallback([dlg](int progress) {
        // 不在这里更新进度，而是在每个文件的专用回调中更新
    });
    
    // 循环处理任务队列中的任务
    FileTask task;
    while (dlg->GetTask(task)) {
        // 增加活动线程计数
        dlg->active_threads_++;
        
        // 获取文件名和在列表中的索引
        std::wstring filename = std::filesystem::path(task.file_path).filename().wstring();
        int file_index = -1;
        
        // 查找文件在列表中的索引
        for (int i = 0; i < dlg->file_list_.size(); i++) {
            if (std::filesystem::path(dlg->file_list_[i]).filename().wstring() == filename) {
                file_index = i;
                break;
            }
        }
        
        // 初始化进度为0%
        if (file_index >= 0) {
            // 使用PostMessage更新列表项的进度
            WCHAR progress_text[16];
            swprintf_s(progress_text, L"0%%");
            
            // 创建一个结构体来传递更新信息
            struct ListItemUpdate* update_info = new ListItemUpdate;
            update_info->item_index = file_index;
            update_info->sub_item_index = 2; // 进度列
            update_info->text = new std::wstring(progress_text);
            
            // 发送消息更新列表项
            PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
            
            // 初始化文件进度
            std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
            dlg->file_progress_[task.file_path] = 0;
        }
        
        // 加载文件
        if (!thread_audio_processor->LoadWavFile(task.file_path)) {
            // 发送错误消息
            std::wstring error_msg = L"无法加载音频文件: " + filename;
            PostMessage(dlg->hwnd_, WM_USER + 3, reinterpret_cast<WPARAM>(new std::wstring(error_msg)), 0);
            
            // 更新列表项显示错误信息
            if (file_index >= 0) {
                // 创建一个结构体来传递更新信息
                struct ListItemUpdate* update_info = new ListItemUpdate;
                update_info->item_index = file_index;
                update_info->sub_item_index = 2; // 进度列
                update_info->text = new std::wstring(L"加载失败");
                
                // 发送消息更新列表项
                PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
            }
            
            // 增加错误文件计数
            dlg->error_files_++;
            
            // 将失败文件的进度设置为100%，确保总进度计算正确
            std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
            dlg->file_progress_[task.file_path] = 100;
        } else {
            // 设置音频格式
            thread_audio_processor->SetAudioFormat(task.format);
            thread_audio_processor->SetOutputFormat(task.output_format);
            
            // 发送状态更新消息
            // std::wstring status_msg = L"正在处理: " + filename;
            // PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status_msg)), 0);
            
            // 设置进度回调函数，用于更新特定文件的进度
            thread_audio_processor->SetProgressCallback([dlg, task, file_index](int progress) {
                // 更新文件进度 - 使用互斥锁确保线程安全
                {
                    std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
                    dlg->file_progress_[task.file_path] = progress;
                }
                
                // 如果找到了文件索引，更新列表项
                if (file_index >= 0) {
                    // 格式化进度文本
                    WCHAR progress_text[16];
                    swprintf_s(progress_text, L"%d%%", progress);
                    
                    // 创建一个结构体来传递更新信息
                    struct ListItemUpdate* update_info = new ListItemUpdate;
                    update_info->item_index = file_index;
                    update_info->sub_item_index = 2; // 进度列
                    update_info->text = new std::wstring(progress_text);
                    
                    // 发送消息更新列表项
                    PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
                }
                
                // 不在这里触发总进度更新，而是依靠定时器统一更新
            });
            
            // 执行通道拆分
            if (!thread_audio_processor->SplitChannels(task.output_dir)) {
                // 发送错误消息
                std::wstring error_msg = L"处理音频文件失败: " + filename;
                PostMessage(dlg->hwnd_, WM_USER + 3, reinterpret_cast<WPARAM>(new std::wstring(error_msg)), 0);
                
                // 更新列表项显示错误信息
                if (file_index >= 0) {
                    // 创建一个结构体来传递更新信息
                    struct ListItemUpdate* update_info = new ListItemUpdate;
                    update_info->item_index = file_index;
                    update_info->sub_item_index = 2; // 进度列
                    update_info->text = new std::wstring(L"处理失败");
                    
                    // 发送消息更新列表项
                    PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
                }
                
                // 增加错误文件计数
                dlg->error_files_++;
                
                // 将失败文件的进度设置为100%，确保总进度计算正确
                std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
                dlg->file_progress_[task.file_path] = 100;
            }
            else if (file_index >= 0) {
                // 处理成功，设置进度为100%
                WCHAR progress_text[16];
                swprintf_s(progress_text, L"100%%");
                
                // 创建一个结构体来传递更新信息
                struct ListItemUpdate* update_info = new ListItemUpdate;
                update_info->item_index = file_index;
                update_info->sub_item_index = 2; // 进度列
                update_info->text = new std::wstring(progress_text);
                
                // 发送消息更新列表项
                PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
                
                // 确保文件进度设置为100%
                std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
                dlg->file_progress_[task.file_path] = 100;
            }
        }
        
        // 更新已完成文件数量
        dlg->completed_files_++;
        
        // 不在这里计算和发送总体进度，而是依靠定时器统一更新
        // 这样可以避免多个线程同时更新进度导致的闪烁问题
        
        // 减少活动线程计数
        dlg->active_threads_--;
    }
    
    return 0;
}

// 处理单个文件
bool MainDialog::ProcessSingleFile(const std::wstring& file, const std::wstring& output_dir, 
                                  AudioProcessor::AudioFormat& format, AudioProcessor::OutputFormat output_format) {
    // 加载文件
    if (!audio_processor_->LoadWavFile(file)) {
        // 使用PostMessage发送错误消息，避免直接调用UI函数
        PostMessage(hwnd_, WM_USER + 3, reinterpret_cast<WPARAM>(new std::wstring(L"无法加载音频文件")), 0);
        return false;
    }
    
    // 设置音频格式
    audio_processor_->SetAudioFormat(format);
    audio_processor_->SetOutputFormat(output_format);
    
    // 发送状态更新消息
    std::wstring status_msg = L"正在处理: " + std::filesystem::path(file).filename().wstring();
    PostMessage(hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status_msg)), 0);
    
    // 执行通道拆分
    if (!audio_processor_->SplitChannels(output_dir)) {
        // 发送错误消息
        PostMessage(hwnd_, WM_USER + 3, reinterpret_cast<WPARAM>(new std::wstring(L"处理音频文件失败")), 0);
        return false;
    }
    
    return true;
}

void MainDialog::UpdateProgress(int progress) {
    // 确保进度值在有效范围内（0-100）
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    
    // 获取当前的进度值
    int current_progress = SendMessage(progress_bar_, PBM_GETPOS, 0, 0);
    
    // 只有当新进度大于当前进度时才更新，避免进度条倒退
    // 但不要完全阻止相等的进度更新，这可能导致进度停滞
    if (current_progress >= progress) return;
    
    // 更新进度条位置
    SendMessage(progress_bar_, PBM_SETPOS, progress, 0);
}

void MainDialog::UpdateStatus(const std::wstring& status) {
	//获取当前的状态文本，如果当前状态文本与新的状态文本相同，则不更新
	WCHAR current_status[256];
	GetWindowText(status_text_, current_status, 256);
	if (wcscmp(current_status, status.c_str()) == 0) return;

    SetWindowText(status_text_, status.c_str());
}

void MainDialog::InitializeControls() {
    // 获取控件句柄
    list_view_ = GetDlgItem(hwnd_, IDC_FILE_LIST);
    progress_bar_ = GetDlgItem(hwnd_, IDC_PROGRESS);
    status_text_ = GetDlgItem(hwnd_, IDC_STATUS);
    HWND sample_rate_combo = GetDlgItem(hwnd_, IDC_SAMPLE_RATE);
    HWND bits_per_sample_combo = GetDlgItem(hwnd_, IDC_BITS_PER_SAMPLE);
    HWND num_channels_combo = GetDlgItem(hwnd_, IDC_NUM_CHANNELS);
    HWND num_thread_combo = GetDlgItem(hwnd_, IDC_THREAD_COUNT);

    // 初始化列表视图
    LVCOLUMN lvc = { 0 };
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 300;
    lvc.pszText = const_cast<LPWSTR>(L"文件名");
    ListView_InsertColumn(list_view_, 0, &lvc);

    // 添加文件大小列
    lvc.cx = 100;
    lvc.pszText = const_cast<LPWSTR>(L"文件大小");
    ListView_InsertColumn(list_view_, 1, &lvc);
    
    // 添加处理进度列
    lvc.cx = 100;
    lvc.pszText = const_cast<LPWSTR>(L"处理进度");
    ListView_InsertColumn(list_view_, 2, &lvc);

    // 启用列表视图的完整行选择、网格线和多选功能
    ListView_SetExtendedListViewStyle(list_view_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    // 设置进度条范围
    SendMessage(progress_bar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    // 初始化采样率选项
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"8000 Hz"));
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, (LPARAM)L"16000 Hz");
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, (LPARAM)L"22050 Hz");
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, (LPARAM)L"44100 Hz");
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, (LPARAM)L"48000 Hz");
    SendMessage(sample_rate_combo, CB_SETCURSEL, 1, 0); // 默认选择16000 Hz

    // 初始化位深度选项
    SendMessage(bits_per_sample_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"8 bit"));
    SendMessage(bits_per_sample_combo, CB_ADDSTRING, 0, (LPARAM)L"16 bit");
    SendMessage(bits_per_sample_combo, CB_ADDSTRING, 0, (LPARAM)L"24 bit");
    SendMessage(bits_per_sample_combo, CB_ADDSTRING, 0, (LPARAM)L"32 bit");
    SendMessage(bits_per_sample_combo, CB_SETCURSEL, 1, 0); // 默认选择16 bit

    // 初始化通道数选项
    SendMessage(num_channels_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2"));
    SendMessage(num_channels_combo, CB_ADDSTRING, 0, (LPARAM)L"4");
    SendMessage(num_channels_combo, CB_ADDSTRING, 0, (LPARAM)L"6");
    SendMessage(num_channels_combo, CB_ADDSTRING, 0, (LPARAM)L"8");
    // 设置组合框样式为可编辑
    LONG style_channels = GetWindowLong(num_channels_combo, GWL_STYLE);
    SetWindowLong(num_channels_combo, GWL_STYLE, style_channels | CBS_DROPDOWN);

    // 初始化多线程选项
    SendMessage(num_thread_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1"));
    SendMessage(num_thread_combo, CB_ADDSTRING, 0, (LPARAM)L"5");
    SendMessage(num_thread_combo, CB_ADDSTRING, 0, (LPARAM)L"10");
    SendMessage(num_thread_combo, CB_SETCURSEL, 1, 0); // 默认选择5线程
    // 设置组合框样式为可编辑
    LONG style_thread = GetWindowLong(num_thread_combo, GWL_STYLE);
    SetWindowLong(num_thread_combo, GWL_STYLE, style_thread | CBS_DROPDOWN);

    // 初始化输出格式选项
    HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
    SendMessage(output_format_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAV"));
    SendMessage(output_format_combo, CB_ADDSTRING, 0, (LPARAM)L"PCM");
    SendMessage(output_format_combo, CB_SETCURSEL, 1, 0); // 默认选择PCM
}

void MainDialog::LoadSettings() {
    // 从注册表加载设置
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\WavSplitChannel", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value;
        DWORD size = sizeof(DWORD);

        // 加载采样率设置
        if (RegQueryValueEx(hKey, L"SampleRate", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            HWND sample_rate_combo = GetDlgItem(hwnd_, IDC_SAMPLE_RATE);
            SendMessage(sample_rate_combo, CB_SETCURSEL, value, 0);
        }

        // 加载位深度设置
        if (RegQueryValueEx(hKey, L"BitsPerSample", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            HWND bits_per_sample_combo = GetDlgItem(hwnd_, IDC_BITS_PER_SAMPLE);
            SendMessage(bits_per_sample_combo, CB_SETCURSEL, value, 0);
        }

        // 加载输出格式设置
        if (RegQueryValueEx(hKey, L"OutputFormat", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
            SendMessage(output_format_combo, CB_SETCURSEL, value, 0);
        }
        
        // 加载线程数设置
        if (RegQueryValueEx(hKey, L"ThreadCount", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            HWND thread_count_combo = GetDlgItem(hwnd_, IDC_THREAD_COUNT);
            if (value > 0) {
                SetDlgItemInt(hwnd_, IDC_THREAD_COUNT, value, FALSE);
            } else {
                SetDlgItemInt(hwnd_, IDC_THREAD_COUNT, 5, FALSE); // 默认5个线程
            }
        } else {
            // 如果没有保存过线程数设置，设置默认值
            SetDlgItemInt(hwnd_, IDC_THREAD_COUNT, 5, FALSE); // 默认5个线程
        }

        RegCloseKey(hKey);
    }
}

void MainDialog::SaveSettings() {
    // 保存设置到注册表
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\WavSplitChannel", 0, nullptr,
                      REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        // 保存采样率设置
        HWND sample_rate_combo = GetDlgItem(hwnd_, IDC_SAMPLE_RATE);
        DWORD value = SendMessage(sample_rate_combo, CB_GETCURSEL, 0, 0);
        RegSetValueEx(hKey, L"SampleRate", 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));

        // 保存位深度设置
        HWND bits_per_sample_combo = GetDlgItem(hwnd_, IDC_BITS_PER_SAMPLE);
        value = SendMessage(bits_per_sample_combo, CB_GETCURSEL, 0, 0);
        RegSetValueEx(hKey, L"BitsPerSample", 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));

        // 保存输出格式设置
        HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
        value = SendMessage(output_format_combo, CB_GETCURSEL, 0, 0);
        RegSetValueEx(hKey, L"OutputFormat", 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));
        
        // 保存线程数设置
        HWND thread_count_combo = GetDlgItem(hwnd_, IDC_THREAD_COUNT);
        value = GetDlgItemInt(hwnd_, IDC_THREAD_COUNT, nullptr, FALSE);
        RegSetValueEx(hKey, L"ThreadCount", 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));

        RegCloseKey(hKey);
    }
}

// 初始化线程池
void MainDialog::InitThreadPool(int thread_count) {
    // 确保先关闭已有的线程池
    ShutdownThreadPool();
    
    // 设置最大线程数
    max_threads_ = thread_count > 0 ? thread_count : 1;
    shutdown_threads_ = false;
    active_threads_ = 0;
    
    // 创建工作线程
    for (int i = 0; i < max_threads_; i++) {
        HANDLE thread = CreateThread(
            nullptr,                   // 默认安全属性
            0,                          // 默认堆栈大小
            WorkerThreadProc,           // 线程函数
            this,                       // 线程参数
            0,                          // 默认创建标志
            nullptr                    // 不接收线程ID
        );
        
        if (thread != nullptr) {
            worker_threads_.push_back(thread);
        }
    }
}

// 关闭线程池
void MainDialog::ShutdownThreadPool() {
    // 标记线程池关闭
    shutdown_threads_ = true;
    
    // 通知所有等待的线程
    task_cv_.notify_all();
    
    // 等待所有线程结束
    if (!worker_threads_.empty()) {
        WaitForMultipleObjects(static_cast<DWORD>(worker_threads_.size()), worker_threads_.data(), TRUE, 5000);
        
        // 关闭所有线程句柄
        for (HANDLE thread : worker_threads_) {
            CloseHandle(thread);
        }
        
        worker_threads_.clear();
    }
    
    // 清空任务队列
    std::queue<FileTask> empty;
    std::swap(task_queue_, empty);
}