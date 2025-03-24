#include "MainDialog.h"
#include <shobjidl.h> 
#include <shlwapi.h>
#include <windowsx.h>
#include <filesystem>
#include <commctrl.h>


MainDialog* MainDialog::instance_ = nullptr;

MainDialog::MainDialog() : hwnd_(nullptr), audio_processor_(std::make_unique<AudioProcessor>()) {
    instance_ = this;
}

MainDialog::~MainDialog() {
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
        if (progress >= 100) {
            KillTimer(hwnd_, 1);
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
    
    // 开始处理文件
    bool all_success = true;
    for (const auto& file : file_list_) {
        UpdateStatus(L"正在加载: " + std::filesystem::path(file).filename().wstring());
        
        if (!audio_processor_->LoadWavFile(file)) {
            MessageBox(hwnd_, L"无法加载音频文件", L"错误", MB_OK | MB_ICONERROR);
            all_success = false;
            continue;
        }
        
        // 获取输入文件的目录作为输出目录
        std::wstring output_dir = std::filesystem::path(file).parent_path().wstring();
        
        // 获取输出格式设置
        HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
        int output_format = ComboBox_GetCurSel(output_format_combo);
        audio_processor_->SetOutputFormat(output_format == 0 ? AudioProcessor::OutputFormat::WAV : AudioProcessor::OutputFormat::PCM);
                                
            UpdateStatus(L"正在处理: " + std::filesystem::path(file).filename().wstring());
        SetTimer(hwnd_, 1, 100, nullptr); // 启动进度更新定时器

        if (!audio_processor_->SplitChannels(output_dir)) {
            MessageBox(hwnd_, L"处理音频文件失败", L"错误", MB_OK | MB_ICONERROR);
            all_success = false;
            continue;
        }

        // 等待处理完成
        while (audio_processor_->GetProgress() < 100) {
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            Sleep(10);
        }

        KillTimer(hwnd_, 1); // 停止进度更新定时器
    }

    // 计算总耗时
    LARGE_INTEGER end_time;
    QueryPerformanceCounter(&end_time);
    double elapsed_seconds = static_cast<double>(end_time.QuadPart - start_time.QuadPart) / frequency.QuadPart;

    std::wstring status = (all_success ? L"处理完成" : L"处理完成，但有错误发生");
    status += L"，总耗时：" + std::to_wstring(static_cast<int>(elapsed_seconds)) + L" 秒";
    UpdateStatus(status);
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