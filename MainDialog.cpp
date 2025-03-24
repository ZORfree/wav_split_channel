#include "MainDialog.h"
#include <shobjidl.h> 
#include <shlwapi.h>
#include <windowsx.h>
#include <filesystem>
#include <commctrl.h>


MainDialog* MainDialog::instance_ = nullptr;

MainDialog::MainDialog() : hwnd_(nullptr), audio_processor_(std::make_unique<AudioProcessor>()), 
    worker_thread_(nullptr), is_processing_(false) {
    instance_ = this;
}

MainDialog::~MainDialog() {
    // 确保工作线程已经结束
    if (worker_thread_ != nullptr) {
        // 等待线程结束，最多等待5秒
        WaitForSingleObject(worker_thread_, 5000);
        CloseHandle(worker_thread_);
        worker_thread_ = nullptr;
    }
    instance_ = nullptr;
}

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
            std::wstring* error_msg = reinterpret_cast<std::wstring*>(wParam);
            if (error_msg) {
                MessageBox(hwnd, error_msg->c_str(), L"错误", MB_OK | MB_ICONERROR);
                delete error_msg; // 释放内存
            }
            return TRUE;
        }
    }
    return FALSE;
}

INT_PTR MainDialog::OnInitDialog() {
    InitializeControls();
    return static_cast<INT_PTR>(TRUE);
}

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

INT_PTR MainDialog::OnTimer(WPARAM wParam) {
    if (wParam == 1) { // 进度更新定时器
        int progress = audio_processor_->GetProgress();
        UpdateProgress(progress);
        
        // 检查工作线程是否完成
        if (is_processing_ && worker_thread_ != nullptr) {
            DWORD exitCode = 0;
            if (GetExitCodeThread(worker_thread_, &exitCode) && exitCode != STILL_ACTIVE) {
                // 线程已完成
                CloseHandle(worker_thread_);
                worker_thread_ = nullptr;
                is_processing_ = false;
                KillTimer(hwnd_, 1);
                
                // 更新状态
                UpdateStatus(L"处理完成");
            }
        }
        
        return static_cast<INT_PTR>(TRUE);
    }
    return static_cast<INT_PTR>(FALSE);
}

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

void MainDialog::SplitChannels() {
    if (file_list_.empty()) {
        MessageBox(hwnd_, L"请先导入文件", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
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

    // 设置通道数
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
    
    // 标记为正在处理
    is_processing_ = true;
    UpdateStatus(L"正在处理文件...");
    
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
        MessageBox(hwnd_, L"创建工作线程失败", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    
    // 设置定时器，定期更新UI
    SetTimer(hwnd_, 1, 100, nullptr);
}

// 工作线程函数，处理所有文件
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
    
    // 开始处理文件
    bool all_success = true;
    int total_files = static_cast<int>(dlg->file_list_.size());
    int processed_files = 0;
    
    for (const auto& file : dlg->file_list_) {
        // 使用PostMessage更新状态，避免直接调用UI函数
        std::wstring status_msg = L"正在加载: " + std::filesystem::path(file).filename().wstring();
        PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status_msg)), 0);
        
        // 获取输入文件的目录作为输出目录
        std::wstring output_dir = std::filesystem::path(file).parent_path().wstring();
        
        // 处理单个文件
        if (!dlg->ProcessSingleFile(file, output_dir, format, output_format)) {
            all_success = false;
        }
        
        // 更新已处理文件数量和总体进度
        processed_files++;
        int overall_progress = static_cast<int>((processed_files * 100) / total_files);
        PostMessage(dlg->hwnd_, WM_USER + 1, static_cast<WPARAM>(overall_progress), 0);
    }
    
    // 计算总耗时
    LARGE_INTEGER end_time;
    QueryPerformanceCounter(&end_time);
    double elapsed_seconds = static_cast<double>(end_time.QuadPart - start_time.QuadPart) / frequency.QuadPart;
    
    // 更新状态
    std::wstring status = (all_success ? L"处理完成" : L"处理完成，但有错误发生");
    status += L"，总耗时：" + std::to_wstring(static_cast<int>(elapsed_seconds)) + L" 秒";
    PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status)), 0);
    
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
    SendMessage(progress_bar_, PBM_SETPOS, progress, 0);
}

void MainDialog::UpdateStatus(const std::wstring& status) {
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
    LONG style = GetWindowLong(num_channels_combo, GWL_STYLE);
    SetWindowLong(num_channels_combo, GWL_STYLE, style | CBS_DROPDOWN);

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

        RegCloseKey(hKey);
    }
}