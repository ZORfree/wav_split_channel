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
    // ȷ�������߳��Ѿ�����
    if (worker_thread_ != nullptr) {
        // �ȴ��߳̽��������ȴ�5��
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
        wsprintf(errorMsg, L"�����Ի���ʧ�ܣ�������룺%d", error);
        MessageBox(nullptr, errorMsg, L"����", MB_OK | MB_ICONERROR);
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
        // �������̷߳��͵�״̬������Ϣ
        case WM_USER + 2: {
            // ����״̬�ı�
            std::wstring* status_msg = reinterpret_cast<std::wstring*>(wParam);
            if (status_msg) {
                dlg->UpdateStatus(*status_msg);
                delete status_msg; // �ͷ��ڴ�
            }
            return TRUE;
        }
        // �������̷߳��͵Ĵ�����Ϣ
        case WM_USER + 3: {
            // ��ʾ������Ϣ��
            std::wstring* error_msg = reinterpret_cast<std::wstring*>(wParam);
            if (error_msg) {
                MessageBox(hwnd, error_msg->c_str(), L"����", MB_OK | MB_ICONERROR);
                delete error_msg; // �ͷ��ڴ�
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
                // ��ȡѡ��������
                int selectedCount = ListView_GetSelectedCount(list_view_);
                if (selectedCount == 0) return TRUE;

                // ��ȡ�����λ��
                POINT pt;
                GetCursorPos(&pt);

                // ���������˵�
                HMENU hPopupMenu = CreatePopupMenu();
                WCHAR menuText[32];
                swprintf_s(menuText, L"ɾ��ѡ�е�%d���ļ�", selectedCount);
                AppendMenu(hPopupMenu, MF_STRING, 1, menuText);

                // ��ʾ�˵�������ѡ��
                int cmd = TrackPopupMenu(hPopupMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                        pt.x, pt.y, 0, hwnd_, nullptr);
                if (cmd == 1) {
                    // �Ӻ���ǰɾ��ѡ����Ա��������仯������
                    std::vector<int> selectedItems;
                    int item = -1;
                    while ((item = ListView_GetNextItem(list_view_, item, LVNI_SELECTED)) != -1) {
                        selectedItems.push_back(item);
                    }

                    // �������Ӵ�С����
                    std::sort(selectedItems.rbegin(), selectedItems.rend());

                    // ɾ��ѡ�е���
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
    if (wParam == 1) { // ���ȸ��¶�ʱ��
        int progress = audio_processor_->GetProgress();
        UpdateProgress(progress);
        
        // ��鹤���߳��Ƿ����
        if (is_processing_ && worker_thread_ != nullptr) {
            DWORD exitCode = 0;
            if (GetExitCodeThread(worker_thread_, &exitCode) && exitCode != STILL_ACTIVE) {
                // �߳������
                CloseHandle(worker_thread_);
                worker_thread_ = nullptr;
                is_processing_ = false;
                KillTimer(hwnd_, 1);
                
                // ����״̬
                UpdateStatus(L"�������");
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
        // �����ļ����͹�����
        COMDLG_FILTERSPEC fileTypes[] = {
            { L"Audio Files", L"*.wav;*.pcm" },
            { L"WAV Files", L"*.wav" },
            { L"PCM Files", L"*.pcm" },
            { L"All Files", L"*.*" }
        };
        pFileOpen->SetFileTypes(4, fileTypes);

        // ���ö�ѡ����
        DWORD dwFlags;
        pFileOpen->GetOptions(&dwFlags);
        pFileOpen->SetOptions(dwFlags | FOS_ALLOWMULTISELECT);

        // ��ʾ�Ի���
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
                                // ����ļ����б�
                                file_list_.push_back(filePath);
                                
                                // �����б���ͼ
                                LVITEM lvi = { 0 };
                                lvi.mask = LVIF_TEXT;
                                lvi.iItem = ListView_GetItemCount(list_view_);
                                lvi.iSubItem = 0;
                                lvi.pszText = PathFindFileName(filePath);
                                ListView_InsertItem(list_view_, &lvi);

                                // ��ȡ�ļ���С����ʽ��
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
        MessageBox(hwnd_, L"���ȵ����ļ�", L"��ʾ", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // ����Ѿ��ڴ����У���Ҫ�ظ�����
    if (is_processing_) {
        MessageBox(hwnd_, L"���ڴ����ļ�����ȴ���ǰ�������", L"��ʾ", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // ���ý��Ȼص�����
    audio_processor_->SetProgressCallback([this](int progress) {
        // ʹ��PostMessage�첽���½��ȣ��������������߳�
        PostMessage(hwnd_, WM_USER + 1, static_cast<WPARAM>(progress), 0);
    });

    // �����Ϣ����
    SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    
    // ʹ��SetWindowSubclass�������д������໯
    SetWindowSubclass(hwnd_, [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR /*idSubclass*/, DWORD_PTR dwRefData) -> LRESULT {
        if (msg == WM_USER + 1) {
            if (MainDialog* dlg = reinterpret_cast<MainDialog*>(GetWindowLongPtr(hwnd, GWLP_USERDATA))) {
                dlg->UpdateProgress(static_cast<int>(wParam));
            }
            return 0;
        }
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }, 1, reinterpret_cast<DWORD_PTR>(this));

    // ��ʼ��ʱ
    LARGE_INTEGER frequency, start_time;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);

    // ��ȡ��Ƶ��������
    HWND sample_rate_combo = GetDlgItem(hwnd_, IDC_SAMPLE_RATE);
    HWND bits_per_sample_combo = GetDlgItem(hwnd_, IDC_BITS_PER_SAMPLE);
    HWND num_channels_combo = GetDlgItem(hwnd_, IDC_NUM_CHANNELS);

    AudioProcessor::AudioFormat format;
    int sample_rate_index = ComboBox_GetCurSel(sample_rate_combo);
    int bits_per_sample_index = ComboBox_GetCurSel(bits_per_sample_combo);
    int num_channels = GetDlgItemInt(hwnd_, IDC_NUM_CHANNELS, nullptr, FALSE);

    // ���ò�����
    switch (sample_rate_index) {
        case 0: format.sample_rate = 8000; break;
        case 1: format.sample_rate = 16000; break;
        case 2: format.sample_rate = 22050; break;
        case 3: format.sample_rate = 44100; break;
        case 4: format.sample_rate = 48000; break;
        default: format.sample_rate = 16000;
    }

    // ����λ���
    switch (bits_per_sample_index) {
        case 0: format.bits_per_sample = 8; break;
        case 1: format.bits_per_sample = 16; break;
        case 2: format.bits_per_sample = 24; break;
        case 3: format.bits_per_sample = 32; break;
        default: format.bits_per_sample = 16;
    }

    // ����ͨ����
    format.num_channels = num_channels > 0 ? num_channels : 2;
    audio_processor_->SetAudioFormat(format);

    // ���ý�����
    SendMessage(progress_bar_, PBM_SETPOS, 0, 0);
    
    // ��ȡ�����ʽ����
    HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
    int output_format = ComboBox_GetCurSel(output_format_combo);
    AudioProcessor::OutputFormat audio_output_format = output_format == 0 ? 
        AudioProcessor::OutputFormat::WAV : AudioProcessor::OutputFormat::PCM;
    audio_processor_->SetOutputFormat(audio_output_format);
    
    // ���Ϊ���ڴ���
    is_processing_ = true;
    UpdateStatus(L"���ڴ����ļ�...");
    
    // ���������̴߳����ļ�
    worker_thread_ = CreateThread(
        nullptr,                   // Ĭ�ϰ�ȫ����
        0,                          // Ĭ�϶�ջ��С
        ProcessFilesThreadProc,     // �̺߳���
        this,                       // �̲߳���
        0,                          // Ĭ�ϴ�����־
        nullptr                    // �������߳�ID
    );
    
    if (worker_thread_ == nullptr) {
        is_processing_ = false;
        MessageBox(hwnd_, L"���������߳�ʧ��", L"����", MB_OK | MB_ICONERROR);
        return;
    }
    
    // ���ö�ʱ�������ڸ���UI
    SetTimer(hwnd_, 1, 100, nullptr);
}

// �����̺߳��������������ļ�
DWORD WINAPI MainDialog::ProcessFilesThreadProc(LPVOID lpParam) {
    MainDialog* dlg = static_cast<MainDialog*>(lpParam);
    if (!dlg) return 1;
    
    // ��ʼ��ʱ
    LARGE_INTEGER frequency, start_time;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_time);
    
    // ��ȡ��Ƶ��������
    AudioProcessor::AudioFormat format = dlg->audio_processor_->GetAudioFormat();
    AudioProcessor::OutputFormat output_format = dlg->audio_processor_->GetOutputFormat();
    
    // ��ʼ�����ļ�
    bool all_success = true;
    int total_files = static_cast<int>(dlg->file_list_.size());
    int processed_files = 0;
    
    for (const auto& file : dlg->file_list_) {
        // ʹ��PostMessage����״̬������ֱ�ӵ���UI����
        std::wstring status_msg = L"���ڼ���: " + std::filesystem::path(file).filename().wstring();
        PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status_msg)), 0);
        
        // ��ȡ�����ļ���Ŀ¼��Ϊ���Ŀ¼
        std::wstring output_dir = std::filesystem::path(file).parent_path().wstring();
        
        // �������ļ�
        if (!dlg->ProcessSingleFile(file, output_dir, format, output_format)) {
            all_success = false;
        }
        
        // �����Ѵ����ļ��������������
        processed_files++;
        int overall_progress = static_cast<int>((processed_files * 100) / total_files);
        PostMessage(dlg->hwnd_, WM_USER + 1, static_cast<WPARAM>(overall_progress), 0);
    }
    
    // �����ܺ�ʱ
    LARGE_INTEGER end_time;
    QueryPerformanceCounter(&end_time);
    double elapsed_seconds = static_cast<double>(end_time.QuadPart - start_time.QuadPart) / frequency.QuadPart;
    
    // ����״̬
    std::wstring status = (all_success ? L"�������" : L"������ɣ����д�����");
    status += L"���ܺ�ʱ��" + std::to_wstring(static_cast<int>(elapsed_seconds)) + L" ��";
    PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status)), 0);
    
    return 0;
}

// �������ļ�
bool MainDialog::ProcessSingleFile(const std::wstring& file, const std::wstring& output_dir, 
                                  AudioProcessor::AudioFormat& format, AudioProcessor::OutputFormat output_format) {
    // �����ļ�
    if (!audio_processor_->LoadWavFile(file)) {
        // ʹ��PostMessage���ʹ�����Ϣ������ֱ�ӵ���UI����
        PostMessage(hwnd_, WM_USER + 3, reinterpret_cast<WPARAM>(new std::wstring(L"�޷�������Ƶ�ļ�")), 0);
        return false;
    }
    
    // ������Ƶ��ʽ
    audio_processor_->SetAudioFormat(format);
    audio_processor_->SetOutputFormat(output_format);
    
    // ����״̬������Ϣ
    std::wstring status_msg = L"���ڴ���: " + std::filesystem::path(file).filename().wstring();
    PostMessage(hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status_msg)), 0);
    
    // ִ��ͨ�����
    if (!audio_processor_->SplitChannels(output_dir)) {
        // ���ʹ�����Ϣ
        PostMessage(hwnd_, WM_USER + 3, reinterpret_cast<WPARAM>(new std::wstring(L"������Ƶ�ļ�ʧ��")), 0);
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
    // ��ȡ�ؼ����
    list_view_ = GetDlgItem(hwnd_, IDC_FILE_LIST);
    progress_bar_ = GetDlgItem(hwnd_, IDC_PROGRESS);
    status_text_ = GetDlgItem(hwnd_, IDC_STATUS);
    HWND sample_rate_combo = GetDlgItem(hwnd_, IDC_SAMPLE_RATE);
    HWND bits_per_sample_combo = GetDlgItem(hwnd_, IDC_BITS_PER_SAMPLE);
    HWND num_channels_combo = GetDlgItem(hwnd_, IDC_NUM_CHANNELS);

    // ��ʼ���б���ͼ
    LVCOLUMN lvc = { 0 };
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 300;
    lvc.pszText = const_cast<LPWSTR>(L"�ļ���");
    ListView_InsertColumn(list_view_, 0, &lvc);

    // ����ļ���С��
    lvc.cx = 100;
    lvc.pszText = const_cast<LPWSTR>(L"�ļ���С");
    ListView_InsertColumn(list_view_, 1, &lvc);

    // �����б���ͼ��������ѡ�������ߺͶ�ѡ����
    ListView_SetExtendedListViewStyle(list_view_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    // ���ý�������Χ
    SendMessage(progress_bar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    // ��ʼ��������ѡ��
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"8000 Hz"));
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, (LPARAM)L"16000 Hz");
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, (LPARAM)L"22050 Hz");
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, (LPARAM)L"44100 Hz");
    SendMessage(sample_rate_combo, CB_ADDSTRING, 0, (LPARAM)L"48000 Hz");
    SendMessage(sample_rate_combo, CB_SETCURSEL, 1, 0); // Ĭ��ѡ��16000 Hz

    // ��ʼ��λ���ѡ��
    SendMessage(bits_per_sample_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"8 bit"));
    SendMessage(bits_per_sample_combo, CB_ADDSTRING, 0, (LPARAM)L"16 bit");
    SendMessage(bits_per_sample_combo, CB_ADDSTRING, 0, (LPARAM)L"24 bit");
    SendMessage(bits_per_sample_combo, CB_ADDSTRING, 0, (LPARAM)L"32 bit");
    SendMessage(bits_per_sample_combo, CB_SETCURSEL, 1, 0); // Ĭ��ѡ��16 bit

    // ��ʼ��ͨ����ѡ��
    SendMessage(num_channels_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2"));
    SendMessage(num_channels_combo, CB_ADDSTRING, 0, (LPARAM)L"4");
    SendMessage(num_channels_combo, CB_ADDSTRING, 0, (LPARAM)L"6");
    SendMessage(num_channels_combo, CB_ADDSTRING, 0, (LPARAM)L"8");
    // ������Ͽ���ʽΪ�ɱ༭
    LONG style = GetWindowLong(num_channels_combo, GWL_STYLE);
    SetWindowLong(num_channels_combo, GWL_STYLE, style | CBS_DROPDOWN);

    // ��ʼ�������ʽѡ��
    HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
    SendMessage(output_format_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAV"));
    SendMessage(output_format_combo, CB_ADDSTRING, 0, (LPARAM)L"PCM");
    SendMessage(output_format_combo, CB_SETCURSEL, 1, 0); // Ĭ��ѡ��PCM
}

void MainDialog::LoadSettings() {
    // ��ע����������
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\WavSplitChannel", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value;
        DWORD size = sizeof(DWORD);

        // ���ز���������
        if (RegQueryValueEx(hKey, L"SampleRate", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            HWND sample_rate_combo = GetDlgItem(hwnd_, IDC_SAMPLE_RATE);
            SendMessage(sample_rate_combo, CB_SETCURSEL, value, 0);
        }

        // ����λ�������
        if (RegQueryValueEx(hKey, L"BitsPerSample", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            HWND bits_per_sample_combo = GetDlgItem(hwnd_, IDC_BITS_PER_SAMPLE);
            SendMessage(bits_per_sample_combo, CB_SETCURSEL, value, 0);
        }

        // ���������ʽ����
        if (RegQueryValueEx(hKey, L"OutputFormat", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
            SendMessage(output_format_combo, CB_SETCURSEL, value, 0);
        }

        RegCloseKey(hKey);
    }
}

void MainDialog::SaveSettings() {
    // �������õ�ע���
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\WavSplitChannel", 0, nullptr,
                      REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        // �������������
        HWND sample_rate_combo = GetDlgItem(hwnd_, IDC_SAMPLE_RATE);
        DWORD value = SendMessage(sample_rate_combo, CB_GETCURSEL, 0, 0);
        RegSetValueEx(hKey, L"SampleRate", 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));

        // ����λ�������
        HWND bits_per_sample_combo = GetDlgItem(hwnd_, IDC_BITS_PER_SAMPLE);
        value = SendMessage(bits_per_sample_combo, CB_GETCURSEL, 0, 0);
        RegSetValueEx(hKey, L"BitsPerSample", 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));

        // ���������ʽ����
        HWND output_format_combo = GetDlgItem(hwnd_, IDC_OUTPUT_FORMAT);
        value = SendMessage(output_format_combo, CB_GETCURSEL, 0, 0);
        RegSetValueEx(hKey, L"OutputFormat", 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));

        RegCloseKey(hKey);
    }
}