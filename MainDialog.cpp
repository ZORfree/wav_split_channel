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
    // ȷ�������߳��Ѿ�����
    if (worker_thread_ != nullptr) {
        // �ȴ��߳̽��������ȴ�5��
        WaitForSingleObject(worker_thread_, 5000);
        CloseHandle(worker_thread_);
        worker_thread_ = nullptr;
    }
    
    // �ر��̳߳�
    ShutdownThreadPool();
    
    instance_ = nullptr;
}
// ����GUI�Ի���
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

// �����б�����½ṹ��
struct ListItemUpdate {
    int item_index;
    int sub_item_index;
    std::wstring* text;
};
// GUI�������
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
            // std::wstring* error_msg = reinterpret_cast<std::wstring*>(wParam);
            // if (error_msg) {
            //     MessageBox(hwnd, error_msg->c_str(), L"����", MB_OK | MB_ICONERROR);
            //     delete error_msg; // �ͷ��ڴ�
            // }
            return TRUE;
        }
        // �����б��������Ϣ
        case WM_USER + 4: {
            // �����б���
            ListItemUpdate* update_info = reinterpret_cast<ListItemUpdate*>(wParam);
            if (update_info && update_info->text) {
                // �����б����ı�
                LVITEM lvi = { 0 };
                lvi.mask = LVIF_TEXT;
                lvi.iItem = update_info->item_index;
                lvi.iSubItem = update_info->sub_item_index;
                lvi.pszText = const_cast<LPWSTR>(update_info->text->c_str());
                ListView_SetItem(dlg->list_view_, &lvi);
                
                // �ͷ��ڴ�
                delete update_info->text;
                delete update_info;
            }
            return TRUE;
        }
    }
    return FALSE;
}
// �������
INT_PTR MainDialog::OnInitDialog() {
    InitializeControls();
    return static_cast<INT_PTR>(TRUE);
}
// GUI���水ť����
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
// ����Ҽ�����
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
//��ʱ��, ���ٸ���״̬�ı�
INT_PTR MainDialog::OnTimer(WPARAM wParam) {
    if (wParam == 1) { // ���ȸ��¶�ʱ��
        // �����������
        int overall_progress = 0;
        if (total_files_ > 0) {
            // ���������ļ����ܽ���
            double total_progress = 0.0;
            int file_count = 0;
            
            // ��������ӳ�䣬��ֹ���̳߳�ͻ
            std::lock_guard<std::mutex> lock(progress_mutex_);
            
            // ���������ļ��Ľ���
            for (const auto& progress_pair : file_progress_) {
                // ��ÿ���ļ��Ľ��Ȱٷֱ�ת��ΪС�����ۼ�
                total_progress += progress_pair.second / 100.0;
                file_count++;
            }
            
            // ����������ȣ������ļ�����֮�ͳ����ļ�����
            if (file_count > 0) {
                // ���ܽ���ת��Ϊ�ٷֱ�
                overall_progress = static_cast<int>((total_progress / total_files_) * 100);
            } else {
                // ���û���ļ�������Ϣ��ʹ��������ļ�������
                overall_progress = static_cast<int>((completed_files_ * 100) / total_files_);
            }
            
            // ȷ�����Ȳ�����100%
            if (overall_progress > 100) overall_progress = 100;
        }
        
        // ���½�����
        UpdateProgress(overall_progress);
        
        // ����״̬�ı� - ��ʾ����ϸ�Ľ�����Ϣ
        // std::wstring status = L"���ڴ����ļ�... " + std::to_wstring(completed_files_) + L"/" + std::to_wstring(total_files_);
        //status += L" (" + std::to_wstring(overall_progress) + L"%)";
        //if (active_threads_ > 0) {
        //    status += L" - ��߳�: " + std::to_wstring(active_threads_);
        //}
        // UpdateStatus(status);
        
        // ��鹤���߳��Ƿ����
        if (is_processing_ && worker_thread_ != nullptr) {
            DWORD exitCode = 0;
            if (GetExitCodeThread(worker_thread_, &exitCode) && exitCode != STILL_ACTIVE) {
                // ���߳�����ɣ�����Ƿ����й����߳�Ҳ�����
                if (active_threads_ == 0) {
                    // �ر��̳߳�
                    ShutdownThreadPool();
                    
                    // ������Դ
                    CloseHandle(worker_thread_);
                    worker_thread_ = nullptr;
                    is_processing_ = false;
                    KillTimer(hwnd_, 1);
                    
                    // �����ܺ�ʱ
                    // LARGE_INTEGER end_time, frequency;
                    // QueryPerformanceCounter(&end_time);
                    // QueryPerformanceFrequency(&frequency);
                    
                    // // ����״̬
                    // std::wstring complete_status = L"������ɣ������� " + std::to_wstring(completed_files_) + L" ���ļ�";
                    // UpdateStatus(complete_status);
                }
            }
        }
        
        return static_cast<INT_PTR>(TRUE);
    }
    return static_cast<INT_PTR>(FALSE);
}
//���밴ť
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
//��ʼ����ť
void MainDialog::SplitChannels() {
    if (file_list_.empty()) {
        MessageBox(hwnd_, L"���ȵ����ļ�", L"��ʾ", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // ���������ļ�����Ϊ0%
    for (int i = 0; i < file_list_.size(); ++i) {
        // �������ȸ��½ṹ��
        ListItemUpdate* update_info = new ListItemUpdate;
        update_info->item_index = i;
        update_info->sub_item_index = 2; // ������
        update_info->text = new std::wstring(L"0%");
        
        // ������Ϣ�����б���
        PostMessage(hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
    }

    // ��ս��ȼ�¼
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        file_progress_.clear();
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
    HWND num_thread_combo = GetDlgItem(hwnd_, IDC_THREAD_COUNT);

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

    // ����ͨ��������û�����ã�Ĭ��Ϊ2ͨ��
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
    
    // ��ȡ�߳������ã�Ĭ��Ϊ5
    int thread_count = GetDlgItemInt(hwnd_, IDC_THREAD_COUNT, nullptr, FALSE);
    if (thread_count <= 0) {
        thread_count = 5; // Ĭ��ʹ��5���߳�
    }
    
    // ���Ϊ���ڴ���
    is_processing_ = true;
    UpdateStatus(L"��ʼ�����ļ�...");
    
    // ��ʼ���̳߳�
    InitThreadPool(thread_count);
    
    // ���ü�����
    completed_files_ = 0;
    error_files_ = 0;
    total_files_ = static_cast<int>(file_list_.size());
    
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
        ShutdownThreadPool();
        MessageBox(hwnd_, L"���������߳�ʧ��", L"����", MB_OK | MB_ICONERROR);
        return;
    }
    
    // ������ʱ����ÿ200ms�����ڸ���UI
    SetTimer(hwnd_, 1, 200, nullptr);
}

// ��ʼ����ť�������Ĺ����̣߳����������ļ�
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
    
    // ����״̬
    // PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(L"׼�������ļ�...")), 0);
    
    // �������ļ���ӵ��������
    for (const auto& file : dlg->file_list_) {
        // ��ȡ�����ļ���Ŀ¼��Ϊ���Ŀ¼
        std::wstring output_dir = std::filesystem::path(file).parent_path().wstring();
        
        // ����������ӵ�����
        FileTask task;
        task.file_path = file;
        task.output_dir = output_dir;
        task.format = format;
        task.output_format = output_format;
        
        dlg->AddTask(task);
    }
    
    // ����״̬
    // PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(L"���ڴ����ļ�...")), 0);
    
    // �ȴ������������
    while (dlg->completed_files_ < dlg->total_files_ && !dlg->shutdown_threads_) {
        Sleep(200); // ÿ100������һ��
        
        // �����������
        int overall_progress = static_cast<int>((dlg->completed_files_ * 100) / dlg->total_files_);
        PostMessage(dlg->hwnd_, WM_USER + 1, static_cast<WPARAM>(overall_progress), 0);
        
        // ����״̬�ı��������ɹ���ʧ���ļ�����
        std::wstring status = L"���ڴ����ļ�... " + std::to_wstring(dlg->completed_files_) + L"/" + std::to_wstring(dlg->total_files_);
        if (dlg->error_files_ > 0) {
            status += L" (�ɹ�: " + std::to_wstring(dlg->completed_files_ - dlg->error_files_) + 
                     L", ʧ��: " + std::to_wstring(dlg->error_files_) + L")";
        }
        PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status)), 0);
    }
    
    // �����ܺ�ʱ
    LARGE_INTEGER end_time;
    QueryPerformanceCounter(&end_time);
    double elapsed_seconds = static_cast<double>(end_time.QuadPart - start_time.QuadPart) / frequency.QuadPart;
    
    // ����״̬�������ɹ���ʧ���ļ���������ϸͳ��
    std::wstring status = L"�������";
    status += L"���ܺ�ʱ��" + std::to_wstring(static_cast<int>(elapsed_seconds)) + L" ��";
    status += L"�������� " + std::to_wstring(dlg->completed_files_) + L" ���ļ�";
    if (dlg->error_files_ > 0) {
        status += L" (�ɹ�: " + std::to_wstring(dlg->completed_files_ - dlg->error_files_) + 
                 L", ʧ��: " + std::to_wstring(dlg->error_files_) + L")";
    }
    PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status)), 0);
    
    return 0;
}

// ������񵽶���
void MainDialog::AddTask(const FileTask& task) {
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        task_queue_.push(task);
    }
    task_cv_.notify_one();
}

// ��ȡ����Ӷ���
bool MainDialog::GetTask(FileTask& task) {
    std::unique_lock<std::mutex> lock(task_mutex_);
    
    // �ȴ������ر��ź�
    task_cv_.wait(lock, [this] {
        return !task_queue_.empty() || shutdown_threads_;
    });
    
    // ����յ��ر��ź��Ҷ���Ϊ�գ�����false
    if (task_queue_.empty()) {
        return false;
    }
    
    // ��ȡ����
    task = task_queue_.front();
    task_queue_.pop();
    return true;
}

// �����̺߳�����������������е��ļ�
DWORD WINAPI MainDialog::WorkerThreadProc(LPVOID lpParam) {
    MainDialog* dlg = static_cast<MainDialog*>(lpParam);
    if (!dlg) return 1;
    
    // ����ÿ���߳��Լ���AudioProcessorʵ��
    std::unique_ptr<AudioProcessor> thread_audio_processor = std::make_unique<AudioProcessor>();
    
    // ���ý��Ȼص����� - �����ʼ�ص�ֻ���ڵ���Ŀ��
    thread_audio_processor->SetProgressCallback([dlg](int progress) {
        // ����������½��ȣ�������ÿ���ļ���ר�ûص��и���
    });
    
    // ѭ��������������е�����
    FileTask task;
    while (dlg->GetTask(task)) {
        // ���ӻ�̼߳���
        dlg->active_threads_++;
        
        // ��ȡ�ļ��������б��е�����
        std::wstring filename = std::filesystem::path(task.file_path).filename().wstring();
        int file_index = -1;
        
        // �����ļ����б��е�����
        for (int i = 0; i < dlg->file_list_.size(); i++) {
            if (std::filesystem::path(dlg->file_list_[i]).filename().wstring() == filename) {
                file_index = i;
                break;
            }
        }
        
        // ��ʼ������Ϊ0%
        if (file_index >= 0) {
            // ʹ��PostMessage�����б���Ľ���
            WCHAR progress_text[16];
            swprintf_s(progress_text, L"0%%");
            
            // ����һ���ṹ�������ݸ�����Ϣ
            struct ListItemUpdate* update_info = new ListItemUpdate;
            update_info->item_index = file_index;
            update_info->sub_item_index = 2; // ������
            update_info->text = new std::wstring(progress_text);
            
            // ������Ϣ�����б���
            PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
            
            // ��ʼ���ļ�����
            std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
            dlg->file_progress_[task.file_path] = 0;
        }
        
        // �����ļ�
        if (!thread_audio_processor->LoadWavFile(task.file_path)) {
            // ���ʹ�����Ϣ
            std::wstring error_msg = L"�޷�������Ƶ�ļ�: " + filename;
            PostMessage(dlg->hwnd_, WM_USER + 3, reinterpret_cast<WPARAM>(new std::wstring(error_msg)), 0);
            
            // �����б�����ʾ������Ϣ
            if (file_index >= 0) {
                // ����һ���ṹ�������ݸ�����Ϣ
                struct ListItemUpdate* update_info = new ListItemUpdate;
                update_info->item_index = file_index;
                update_info->sub_item_index = 2; // ������
                update_info->text = new std::wstring(L"����ʧ��");
                
                // ������Ϣ�����б���
                PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
            }
            
            // ���Ӵ����ļ�����
            dlg->error_files_++;
            
            // ��ʧ���ļ��Ľ�������Ϊ100%��ȷ���ܽ��ȼ�����ȷ
            std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
            dlg->file_progress_[task.file_path] = 100;
        } else {
            // ������Ƶ��ʽ
            thread_audio_processor->SetAudioFormat(task.format);
            thread_audio_processor->SetOutputFormat(task.output_format);
            
            // ����״̬������Ϣ
            // std::wstring status_msg = L"���ڴ���: " + filename;
            // PostMessage(dlg->hwnd_, WM_USER + 2, reinterpret_cast<WPARAM>(new std::wstring(status_msg)), 0);
            
            // ���ý��Ȼص����������ڸ����ض��ļ��Ľ���
            thread_audio_processor->SetProgressCallback([dlg, task, file_index](int progress) {
                // �����ļ����� - ʹ�û�����ȷ���̰߳�ȫ
                {
                    std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
                    dlg->file_progress_[task.file_path] = progress;
                }
                
                // ����ҵ����ļ������������б���
                if (file_index >= 0) {
                    // ��ʽ�������ı�
                    WCHAR progress_text[16];
                    swprintf_s(progress_text, L"%d%%", progress);
                    
                    // ����һ���ṹ�������ݸ�����Ϣ
                    struct ListItemUpdate* update_info = new ListItemUpdate;
                    update_info->item_index = file_index;
                    update_info->sub_item_index = 2; // ������
                    update_info->text = new std::wstring(progress_text);
                    
                    // ������Ϣ�����б���
                    PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
                }
                
                // �������ﴥ���ܽ��ȸ��£�����������ʱ��ͳһ����
            });
            
            // ִ��ͨ�����
            if (!thread_audio_processor->SplitChannels(task.output_dir)) {
                // ���ʹ�����Ϣ
                std::wstring error_msg = L"������Ƶ�ļ�ʧ��: " + filename;
                PostMessage(dlg->hwnd_, WM_USER + 3, reinterpret_cast<WPARAM>(new std::wstring(error_msg)), 0);
                
                // �����б�����ʾ������Ϣ
                if (file_index >= 0) {
                    // ����һ���ṹ�������ݸ�����Ϣ
                    struct ListItemUpdate* update_info = new ListItemUpdate;
                    update_info->item_index = file_index;
                    update_info->sub_item_index = 2; // ������
                    update_info->text = new std::wstring(L"����ʧ��");
                    
                    // ������Ϣ�����б���
                    PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
                }
                
                // ���Ӵ����ļ�����
                dlg->error_files_++;
                
                // ��ʧ���ļ��Ľ�������Ϊ100%��ȷ���ܽ��ȼ�����ȷ
                std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
                dlg->file_progress_[task.file_path] = 100;
            }
            else if (file_index >= 0) {
                // ����ɹ������ý���Ϊ100%
                WCHAR progress_text[16];
                swprintf_s(progress_text, L"100%%");
                
                // ����һ���ṹ�������ݸ�����Ϣ
                struct ListItemUpdate* update_info = new ListItemUpdate;
                update_info->item_index = file_index;
                update_info->sub_item_index = 2; // ������
                update_info->text = new std::wstring(progress_text);
                
                // ������Ϣ�����б���
                PostMessage(dlg->hwnd_, WM_USER + 4, reinterpret_cast<WPARAM>(update_info), 0);
                
                // ȷ���ļ���������Ϊ100%
                std::lock_guard<std::mutex> lock(dlg->progress_mutex_);
                dlg->file_progress_[task.file_path] = 100;
            }
        }
        
        // ����������ļ�����
        dlg->completed_files_++;
        
        // �����������ͷ���������ȣ�����������ʱ��ͳһ����
        // �������Ա������߳�ͬʱ���½��ȵ��µ���˸����
        
        // ���ٻ�̼߳���
        dlg->active_threads_--;
    }
    
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
    // ȷ������ֵ����Ч��Χ�ڣ�0-100��
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    
    // ��ȡ��ǰ�Ľ���ֵ
    int current_progress = SendMessage(progress_bar_, PBM_GETPOS, 0, 0);
    
    // ֻ�е��½��ȴ��ڵ�ǰ����ʱ�Ÿ��£��������������
    // ����Ҫ��ȫ��ֹ��ȵĽ��ȸ��£�����ܵ��½���ͣ��
    if (current_progress >= progress) return;
    
    // ���½�����λ��
    SendMessage(progress_bar_, PBM_SETPOS, progress, 0);
}

void MainDialog::UpdateStatus(const std::wstring& status) {
	//��ȡ��ǰ��״̬�ı��������ǰ״̬�ı����µ�״̬�ı���ͬ���򲻸���
	WCHAR current_status[256];
	GetWindowText(status_text_, current_status, 256);
	if (wcscmp(current_status, status.c_str()) == 0) return;

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
    HWND num_thread_combo = GetDlgItem(hwnd_, IDC_THREAD_COUNT);

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
    
    // ��Ӵ��������
    lvc.cx = 100;
    lvc.pszText = const_cast<LPWSTR>(L"�������");
    ListView_InsertColumn(list_view_, 2, &lvc);

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
    LONG style_channels = GetWindowLong(num_channels_combo, GWL_STYLE);
    SetWindowLong(num_channels_combo, GWL_STYLE, style_channels | CBS_DROPDOWN);

    // ��ʼ�����߳�ѡ��
    SendMessage(num_thread_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1"));
    SendMessage(num_thread_combo, CB_ADDSTRING, 0, (LPARAM)L"5");
    SendMessage(num_thread_combo, CB_ADDSTRING, 0, (LPARAM)L"10");
    SendMessage(num_thread_combo, CB_SETCURSEL, 1, 0); // Ĭ��ѡ��5�߳�
    // ������Ͽ���ʽΪ�ɱ༭
    LONG style_thread = GetWindowLong(num_thread_combo, GWL_STYLE);
    SetWindowLong(num_thread_combo, GWL_STYLE, style_thread | CBS_DROPDOWN);

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
        
        // �����߳�������
        if (RegQueryValueEx(hKey, L"ThreadCount", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            HWND thread_count_combo = GetDlgItem(hwnd_, IDC_THREAD_COUNT);
            if (value > 0) {
                SetDlgItemInt(hwnd_, IDC_THREAD_COUNT, value, FALSE);
            } else {
                SetDlgItemInt(hwnd_, IDC_THREAD_COUNT, 5, FALSE); // Ĭ��5���߳�
            }
        } else {
            // ���û�б�����߳������ã�����Ĭ��ֵ
            SetDlgItemInt(hwnd_, IDC_THREAD_COUNT, 5, FALSE); // Ĭ��5���߳�
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
        
        // �����߳�������
        HWND thread_count_combo = GetDlgItem(hwnd_, IDC_THREAD_COUNT);
        value = GetDlgItemInt(hwnd_, IDC_THREAD_COUNT, nullptr, FALSE);
        RegSetValueEx(hKey, L"ThreadCount", 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD));

        RegCloseKey(hKey);
    }
}

// ��ʼ���̳߳�
void MainDialog::InitThreadPool(int thread_count) {
    // ȷ���ȹر����е��̳߳�
    ShutdownThreadPool();
    
    // ��������߳���
    max_threads_ = thread_count > 0 ? thread_count : 1;
    shutdown_threads_ = false;
    active_threads_ = 0;
    
    // ���������߳�
    for (int i = 0; i < max_threads_; i++) {
        HANDLE thread = CreateThread(
            nullptr,                   // Ĭ�ϰ�ȫ����
            0,                          // Ĭ�϶�ջ��С
            WorkerThreadProc,           // �̺߳���
            this,                       // �̲߳���
            0,                          // Ĭ�ϴ�����־
            nullptr                    // �������߳�ID
        );
        
        if (thread != nullptr) {
            worker_threads_.push_back(thread);
        }
    }
}

// �ر��̳߳�
void MainDialog::ShutdownThreadPool() {
    // ����̳߳عر�
    shutdown_threads_ = true;
    
    // ֪ͨ���еȴ����߳�
    task_cv_.notify_all();
    
    // �ȴ������߳̽���
    if (!worker_threads_.empty()) {
        WaitForMultipleObjects(static_cast<DWORD>(worker_threads_.size()), worker_threads_.data(), TRUE, 5000);
        
        // �ر������߳̾��
        for (HANDLE thread : worker_threads_) {
            CloseHandle(thread);
        }
        
        worker_threads_.clear();
    }
    
    // ����������
    std::queue<FileTask> empty;
    std::swap(task_queue_, empty);
}