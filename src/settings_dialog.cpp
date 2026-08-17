#include "settings_dialog.h"

#include "core.h"
#include "ui_helpers.h"

#include <commctrl.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace mmid::ui {
namespace {

constexpr wchar_t kSettingsClass[] = L"MMIDownloaderSettingsWindow";
constexpr wchar_t kSourceClass[] = L"MMIDownloaderSourceWindow";

enum SettingsControlId {
    IDC_SETTINGS_SOURCE_TEXT = 3001,
    IDC_SETTINGS_SOURCE_ADD = 3002,
    IDC_SETTINGS_METADATA = 3004,
    IDC_SETTINGS_TEMP = 3005,
    IDC_SETTINGS_OK = 3006,
    IDC_SETTINGS_CANCEL = 3007,
    IDC_SETTINGS_LOGGING = 3008,
    IDC_SETTINGS_LOG = 3009,
    IDC_SETTINGS_LOG_ADD = 3010,
    IDC_SETTINGS_METADATA_BROWSE = 3012,
    IDC_SETTINGS_TEMP_BROWSE = 3013
};

enum SourceControlId {
    IDC_SOURCE_USE_LOCAL = 3101,
    IDC_SOURCE_LOCAL = 3102,
    IDC_SOURCE_NETWORK = 3103,
    IDC_SOURCE_USERNAME = 3104,
    IDC_SOURCE_PASSWORD = 3105,
    IDC_SOURCE_OK = 3106,
    IDC_SOURCE_CANCEL = 3107,
    IDC_SOURCE_TEST = 3108,
    IDC_SOURCE_LOCAL_ADD = 3109,
    IDC_SOURCE_FTP_MODE = 3110
};


struct SettingsDialogState {
    HWND owner = nullptr;
    HWND source_static = nullptr;
    HWND metadata_edit = nullptr;
    HWND temp_edit = nullptr;
    HWND logging_check = nullptr;
    HWND log_edit = nullptr;
    HWND log_add_button = nullptr;
    HFONT font = nullptr;
    bool saved = false;
};

struct SourceDialogState {
    HWND owner = nullptr;
    HWND use_local_check = nullptr;
    HWND local_edit = nullptr;
    HWND local_add_button = nullptr;
    HWND network_edit = nullptr;
    HWND ftp_mode_combo = nullptr;
    HWND username_edit = nullptr;
    HWND password_edit = nullptr;
    HWND test_button = nullptr;
    HFONT font = nullptr;
    std::wstring initial_network;
    bool loaded_legacy_webdav = false;
    bool saved = false;
};


void set_control_text(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

void make_settings_label(HWND parent, const wchar_t* text, int x, int y, HFONT font) {
    HWND label = make_control(parent, L"STATIC", text, 0, 0, 0);
    MoveWindow(label, x, y + 4, 120, 22, TRUE);
    apply_font(label, font);
}

std::wstring source_summary_text(const Config& cfg) {
    if (!catalog_source_is_configured(cfg)) {
        return L"Not configured";
    }
    if (cfg.use_local_catalog) {
        return cfg.local_catalog_root.empty() ? L"Local catalog: <empty>" : L"Local catalog: " + cfg.local_catalog_root.wstring();
    }
    std::wstring network = trim_w(cfg.catalog_root);
    if (!network.empty()) {
        std::wstring summary = L"Network: " + network;
        if (looks_like_ftp_url(network)) {
            summary += L" [" + ftp_mode_to_string(cfg.ftp_mode) + L"]";
        }
        return summary;
    }
    return L"WebDAV: " + webdav_home_url(cfg);
}

bool source_use_local(const SourceDialogState& state) {
    return SendMessageW(state.use_local_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool looks_like_unc_catalog_path(std::wstring_view value) {
    return value.size() >= 3 &&
        ((value[0] == L'\\' && value[1] == L'\\') ||
            (value[0] == L'/' && value[1] == L'/'));
}

bool local_catalog_marker_exists(const fs::path& root) {
    std::error_code ec;
    fs::path marker = root / L"common" / L"Settings" / L"brands.xml";
    return fs::is_regular_file(marker, ec) && !ec;
}

void configure_local_catalog(Config& cfg, const fs::path& requested_root) {
    static constexpr wchar_t kDefaultChannel[] = L"Trade-Retail";

    cfg.use_local_catalog = true;
    cfg.local_catalog_root = requested_root;
    cfg.catalog_root.clear();

    // A selected channel directory already contains common/Settings/brands.xml.
    // A Service42-MMI root needs exactly one channel component appended later by
    // catalog_file_source_path(). Preserve a valid configured channel and fall
    // back to the donor-compatible retail channel for a newly selected root.
    if (local_catalog_marker_exists(requested_root)) {
        cfg.distribution_channel.clear();
        return;
    }

    std::wstring channel = trim_slashes(cfg.distribution_channel);
    cfg.distribution_channel.clear();
    if (!channel.empty() && local_catalog_marker_exists(requested_root / channel)) {
        cfg.distribution_channel = channel;
        return;
    }
    if (local_catalog_marker_exists(requested_root / kDefaultChannel)) {
        cfg.distribution_channel = kDefaultChannel;
    }
}

void add_ftp_mode_item(HWND combo, const wchar_t* text, FtpMode mode) {
    LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(static_cast<int>(mode)));
}

void fill_ftp_mode_combo(HWND combo, FtpMode selected_mode) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    add_ftp_mode_item(combo, L"Auto: passive, then active", FtpMode::Auto);
    add_ftp_mode_item(combo, L"Passive (PASV)", FtpMode::Passive);
    add_ftp_mode_item(combo, L"Active (PORT)", FtpMode::Active);
    for (int i = 0; i < 3; ++i) {
        LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0);
        if (data != CB_ERR && static_cast<FtpMode>(static_cast<int>(data)) == selected_mode) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

FtpMode selected_ftp_mode(HWND combo) {
    LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        return FtpMode::Auto;
    }
    LRESULT data = SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    if (data == CB_ERR) {
        return FtpMode::Auto;
    }
    return static_cast<FtpMode>(static_cast<int>(data));
}

void update_source_controls(SourceDialogState& state) {
    bool use_local = source_use_local(state);
    EnableWindow(state.local_edit, use_local);
    EnableWindow(state.local_add_button, TRUE);
    EnableWindow(state.network_edit, !use_local);
    EnableWindow(state.ftp_mode_combo, !use_local);
    EnableWindow(state.username_edit, !use_local);
    EnableWindow(state.password_edit, !use_local);
    EnableWindow(state.test_button, TRUE);
}

void fill_source_dialog(SourceDialogState& state) {
    Config cfg = load_config();
    SendMessageW(state.use_local_check, BM_SETCHECK, cfg.use_local_catalog ? BST_CHECKED : BST_UNCHECKED, 0);
    set_control_text(state.local_edit, cfg.local_catalog_root.wstring());
    std::wstring network = trim_w(cfg.catalog_root);
    state.loaded_legacy_webdav = !cfg.use_local_catalog &&
        (cfg.source_transport == SourceTransport::WebDav ||
            (cfg.source_transport == SourceTransport::Auto && network.empty())) &&
        legacy_webdav_source_is_configured(cfg);
    if (state.loaded_legacy_webdav) {
        network = webdav_home_url(cfg);
    }
    state.initial_network = network;
    set_control_text(state.network_edit, network);
    fill_ftp_mode_combo(state.ftp_mode_combo, cfg.ftp_mode);
    set_control_text(state.username_edit, cfg.username);
    try {
        set_control_text(state.password_edit, unprotect_password(cfg.password_dpapi));
    } catch (...) {
        set_control_text(state.password_edit, L"");
    }
    update_source_controls(state);
}

Config source_config_from_dialog(SourceDialogState& state) {
    Config cfg = load_config();
    cfg.source_transport = SourceTransport::Auto;
    bool use_local = source_use_local(state);
    std::wstring local = trim_w(get_window_text(state.local_edit));
    std::wstring network = trim_w(get_window_text(state.network_edit));
    if (!use_local && looks_like_unc_catalog_path(network)) {
        use_local = true;
        local = network;
    }
    cfg.use_local_catalog = use_local;
    if (!cfg.use_local_catalog && !state.loaded_legacy_webdav &&
        network == state.initial_network && !trim_w(cfg.catalog_root).empty() &&
        !trim_slashes(cfg.distribution_channel).empty()) {
        network = catalog_network_url_full(cfg, L"");
    }
    if (cfg.use_local_catalog) {
        if (local.empty()) {
            throw AppError("Local catalog directory is empty");
        }
        configure_local_catalog(cfg, fs::path(local));
    } else {
        if (!looks_like_url(network)) {
            throw AppError("Network catalog source requires an http://, https://, or ftp:// URL");
        }
        cfg.catalog_root = network;
        cfg.local_catalog_root.clear();
    }
    cfg.server.clear();
    cfg.top_level_collection.clear();
    if (!cfg.use_local_catalog) {
        cfg.distribution_channel.clear();
    }
    cfg.ftp_mode = selected_ftp_mode(state.ftp_mode_combo);
    if (cfg.use_local_catalog) {
        cfg.username.clear();
        cfg.password_dpapi.clear();
    } else {
        cfg.username = trim_w(get_window_text(state.username_edit));
        std::wstring password = get_window_text(state.password_edit);
        cfg.password_dpapi = password.empty() ? std::string{} : protect_password(password);
        validate_config_credentials_complete(cfg);
        if (config_has_credentials(cfg) && !credentials_target_an_https_source(cfg)) {
            throw AppError("Credentials require an HTTPS catalog source");
        }
    }
    return cfg;
}

void save_source_dialog(SourceDialogState& state) {
    Config cfg = source_config_from_dialog(state);
    save_config(cfg);
    state.saved = true;
}

void test_source_dialog(HWND window, SourceDialogState& state) {
    Config cfg = source_config_from_dialog(state);
    DWORD status = probe_catalog_source(cfg, L"common/Settings/brands.xml");
    std::wstring message = L"Connection OK: " + std::to_wstring(status) +
        L"\r\n" + catalog_location_description(cfg, L"common/Settings/brands.xml");
    MessageBoxW(window, message.c_str(), L"Source", MB_ICONINFORMATION);
}


void choose_source_local_catalog(HWND window, SourceDialogState& state) {
    FolderPickerResult result = pick_folder_path(
        window,
        L"Select local catalog",
        trim_w(get_window_text(state.local_edit)));
    if (result.failure_stage != FolderPickerFailureStage::None) {
        throw_folder_picker_failure(result);
    }
    if (!result.path) {
        return;
    }

    set_control_text(state.local_edit, result.path->c_str());
    SendMessageW(state.use_local_check, BM_SETCHECK, BST_CHECKED, 0);
    update_source_controls(state);
}

LRESULT CALLBACK source_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        auto* state = reinterpret_cast<SourceDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        const int margin = 14;
        const int label_w = 120;
        const int edit_x = margin + label_w;
        const int edit_w = 420;
        const int local_add_w = 82;
        const int gap = 8;
        int y = margin;

        state->use_local_check = make_control(window, L"BUTTON", L"Use folder catalog (local or network/UNC)", BS_AUTOCHECKBOX, 0, IDC_SOURCE_USE_LOCAL);
        MoveWindow(state->use_local_check, edit_x, y, edit_w, 24, TRUE);
        apply_font(state->use_local_check, state->font);
        y += 34;

        make_settings_label(window, L"Folder", margin, y, state->font);
        state->local_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SOURCE_LOCAL);
        state->local_add_button = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_SOURCE_LOCAL_ADD);
        SendMessageW(state->local_edit, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"Путь к папке Service42-MMI"));
        MoveWindow(state->local_edit, edit_x, y, edit_w - local_add_w - gap, 24, TRUE);
        MoveWindow(state->local_add_button, edit_x + edit_w - local_add_w, y, local_add_w, 24, TRUE);
        apply_font(state->local_edit, state->font);
        apply_font(state->local_add_button, state->font);
        y += 34;

        make_settings_label(window, L"URL", margin, y, state->font);
        state->network_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SOURCE_NETWORK);
        MoveWindow(state->network_edit, edit_x, y, edit_w, 24, TRUE);
        apply_font(state->network_edit, state->font);
        y += 34;

        make_settings_label(window, L"FTP mode", margin, y, state->font);
        state->ftp_mode_combo = make_control(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, IDC_SOURCE_FTP_MODE);
        MoveWindow(state->ftp_mode_combo, edit_x, y, edit_w, 120, TRUE);
        apply_font(state->ftp_mode_combo, state->font);
        y += 34;

        make_settings_label(window, L"Username", margin, y, state->font);
        state->username_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SOURCE_USERNAME);
        MoveWindow(state->username_edit, edit_x, y, edit_w, 24, TRUE);
        apply_font(state->username_edit, state->font);
        y += 34;

        make_settings_label(window, L"Password", margin, y, state->font);
        state->password_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD, WS_EX_CLIENTEDGE, IDC_SOURCE_PASSWORD);
        MoveWindow(state->password_edit, edit_x, y, edit_w, 24, TRUE);
        apply_font(state->password_edit, state->font);
        y += 42;

        state->test_button = make_control(window, L"BUTTON", L"Test", BS_PUSHBUTTON, 0, IDC_SOURCE_TEST);
        HWND ok = make_control(window, L"BUTTON", L"OK", BS_DEFPUSHBUTTON, 0, IDC_SOURCE_OK);
        HWND cancel = make_control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON, 0, IDC_SOURCE_CANCEL);
        MoveWindow(state->test_button, edit_x, y, 82, 28, TRUE);
        MoveWindow(ok, edit_x + edit_w - 176, y, 82, 28, TRUE);
        MoveWindow(cancel, edit_x + edit_w - 88, y, 88, 28, TRUE);
        apply_font(state->test_button, state->font);
        apply_font(ok, state->font);
        apply_font(cancel, state->font);

        fill_source_dialog(*state);
        return 0;
    }
    case WM_COMMAND: {
        auto* state = reinterpret_cast<SourceDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        int id = LOWORD(wparam);
        if (id == IDC_SOURCE_OK) {
            try {
                save_source_dialog(*state);
                DestroyWindow(window);
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_message(ex).c_str(), L"Source", MB_ICONERROR);
            }
            return 0;
        }
        if (id == IDC_SOURCE_CANCEL || id == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (id == IDC_SOURCE_TEST && HIWORD(wparam) == BN_CLICKED) {
            try {
                HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                test_source_dialog(window, *state);
                SetCursor(previous);
            } catch (const std::exception& ex) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                MessageBoxW(window, exception_message(ex).c_str(), L"Source", MB_ICONERROR);
            }
            return 0;
        }
        if (id == IDC_SOURCE_LOCAL_ADD && HIWORD(wparam) == BN_CLICKED) {
            try {
                choose_source_local_catalog(window, *state);
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_message(ex).c_str(), L"Source", MB_ICONERROR);
            }
            return 0;
        }
        if (id == IDC_SOURCE_USE_LOCAL && HIWORD(wparam) == BN_CLICKED) {
            update_source_controls(*state);
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        return handle_static_control_color(window, message, wparam, lparam);
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

class DisabledOwnerGuard {
public:
    explicit DisabledOwnerGuard(HWND owner) : owner_(owner) {
        EnableWindow(owner_, FALSE);
    }

    DisabledOwnerGuard(const DisabledOwnerGuard&) = delete;
    DisabledOwnerGuard& operator=(const DisabledOwnerGuard&) = delete;

    ~DisabledOwnerGuard() {
        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
    }

private:
    HWND owner_ = nullptr;
};

void run_modal_window_loop(HWND dialog) {
    MSG msg{};
    while (IsWindow(dialog)) {
        BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result == -1) {
            throw AppError("GetMessage failed in modal window: " + format_win32_error(GetLastError()));
        }
        if (result == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

bool show_source_dialog(HWND owner) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = source_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSourceClass;
    RegisterClassExW(&wc);

    SourceDialogState state;
    state.owner = owner;
    DisabledOwnerGuard owner_guard(owner);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME,
        kSourceClass,
        L"Source",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        600,
        324,
        owner,
        nullptr,
        instance,
        &state);
    if (!dialog) {
        throw AppError("CreateWindowEx failed for source dialog");
    }
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    run_modal_window_loop(dialog);
    return state.saved;
}

bool settings_logging_enabled(const SettingsDialogState& state) {
    return SendMessageW(state.logging_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void update_settings_controls(SettingsDialogState& state) {
    bool enabled = settings_logging_enabled(state);
    EnableWindow(state.log_edit, enabled);
    EnableWindow(state.log_add_button, enabled);
}

void choose_settings_folder(HWND window, HWND target_edit, const wchar_t* title) {
    FolderPickerResult result = pick_folder_path(
        window,
        title,
        trim_w(get_window_text(target_edit)));
    if (result.failure_stage != FolderPickerFailureStage::None) {
        throw_folder_picker_failure(result);
    }
    if (result.path) {
        set_control_text(target_edit, result.path->c_str());
    }
}

void fill_settings_dialog(SettingsDialogState& state) {
    Config cfg = load_config();
    set_control_text(state.source_static, source_summary_text(cfg));
    set_control_text(state.metadata_edit, cfg.metadata_dir.wstring());
    set_control_text(state.temp_edit, cfg.temp_dir.wstring());
    SendMessageW(state.logging_check, BM_SETCHECK, cfg.logging_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    set_control_text(state.log_edit, log_dir(cfg).wstring());
    update_settings_controls(state);
}

void save_settings_dialog(SettingsDialogState& state) {
    Config cfg = load_config();
    std::wstring metadata = trim_w(get_window_text(state.metadata_edit));
    std::wstring temp = trim_w(get_window_text(state.temp_edit));
    std::wstring log = trim_w(get_window_text(state.log_edit));
    if (!metadata.empty()) {
        cfg.metadata_dir = fs::path(metadata);
    }
    if (!temp.empty()) {
        cfg.temp_dir = fs::path(temp);
    }
    cfg.logging_enabled = settings_logging_enabled(state);
    cfg.log_dir = log.empty() ? executable_dir() / L"Logs" : fs::path(log);
    save_config(cfg);
    fs::create_directories(cfg.metadata_dir);
    fs::create_directories(cfg.temp_dir);
    if (cfg.logging_enabled) {
        fs::create_directories(log_dir(cfg));
    }
    state.saved = true;
}

LRESULT CALLBACK settings_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        auto* state = reinterpret_cast<SettingsDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        const int margin = 16;
        const int label_w = 126;
        const int edit_x = margin + label_w;
        const int content_w = 638;
        const int browse_w = 96;
        const int gap = 8;
        const int edit_w = content_w - label_w - browse_w - gap;
        int y = margin;

        auto add_heading = [&](const wchar_t* text) {
            HWND heading = make_control(window, L"STATIC", text, SS_LEFT, 0, 0);
            MoveWindow(heading, margin, y, content_w, 20, TRUE);
            apply_font(heading, state->font);
            y += 24;
        };

        add_heading(L"SOURCE");
        state->source_static = make_control(window, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, WS_EX_CLIENTEDGE, IDC_SETTINGS_SOURCE_TEXT);
        HWND add_source = make_control(window, L"BUTTON", L"Change...", BS_PUSHBUTTON, 0, IDC_SETTINGS_SOURCE_ADD);
        MoveWindow(state->source_static, margin, y, content_w - browse_w - gap, 26, TRUE);
        MoveWindow(add_source, margin + content_w - browse_w, y, browse_w, 26, TRUE);
        apply_font(state->source_static, state->font);
        apply_font(add_source, state->font);
        y += 40;

        add_heading(L"STORAGE");
        make_settings_label(window, L"Metadata folder", margin, y, state->font);
        state->metadata_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SETTINGS_METADATA);
        HWND metadata_browse = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_SETTINGS_METADATA_BROWSE);
        MoveWindow(state->metadata_edit, edit_x, y, edit_w, 24, TRUE);
        MoveWindow(metadata_browse, edit_x + edit_w + gap, y, browse_w, 24, TRUE);
        apply_font(state->metadata_edit, state->font);
        apply_font(metadata_browse, state->font);
        y += 34;

        make_settings_label(window, L"Temporary folder", margin, y, state->font);
        state->temp_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SETTINGS_TEMP);
        HWND temp_browse = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_SETTINGS_TEMP_BROWSE);
        MoveWindow(state->temp_edit, edit_x, y, edit_w, 24, TRUE);
        MoveWindow(temp_browse, edit_x + edit_w + gap, y, browse_w, 24, TRUE);
        apply_font(state->temp_edit, state->font);
        apply_font(temp_browse, state->font);
        y += 42;

        add_heading(L"LOGGING");
        state->logging_check = make_control(window, L"BUTTON", L"Write operation logs to files", BS_AUTOCHECKBOX, 0, IDC_SETTINGS_LOGGING);
        MoveWindow(state->logging_check, margin, y, content_w, 24, TRUE);
        apply_font(state->logging_check, state->font);
        y += 30;

        make_settings_label(window, L"Log folder", margin, y, state->font);
        state->log_edit = make_control(window, L"EDIT", L"", ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SETTINGS_LOG);
        state->log_add_button = make_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 0, IDC_SETTINGS_LOG_ADD);
        MoveWindow(state->log_edit, edit_x, y, edit_w, 24, TRUE);
        MoveWindow(state->log_add_button, edit_x + edit_w + gap, y, browse_w, 24, TRUE);
        apply_font(state->log_edit, state->font);
        apply_font(state->log_add_button, state->font);
        y += 34;

        y += 8;

        HWND ok = make_control(window, L"BUTTON", L"Save", BS_DEFPUSHBUTTON, 0, IDC_SETTINGS_OK);
        HWND cancel = make_control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON, 0, IDC_SETTINGS_CANCEL);
        MoveWindow(ok, margin + content_w - 192, y, 92, 30, TRUE);
        MoveWindow(cancel, margin + content_w - 92, y, 92, 30, TRUE);
        apply_font(ok, state->font);
        apply_font(cancel, state->font);

        fill_settings_dialog(*state);
        return 0;
    }
    case WM_COMMAND: {
        auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        int id = LOWORD(wparam);
        if (id == IDC_SETTINGS_OK) {
            try {
                save_settings_dialog(*state);
                DestroyWindow(window);
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_message(ex).c_str(), L"Settings", MB_ICONERROR);
            }
            return 0;
        }
        if (id == IDC_SETTINGS_CANCEL || id == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (id == IDC_SETTINGS_SOURCE_ADD && HIWORD(wparam) == BN_CLICKED) {
            if (show_source_dialog(window)) {
                fill_settings_dialog(*state);
                state->saved = true;
            }
            return 0;
        }
        if (id == IDC_SETTINGS_LOGGING && HIWORD(wparam) == BN_CLICKED) {
            update_settings_controls(*state);
            return 0;
        }
        if (id == IDC_SETTINGS_LOG_ADD && HIWORD(wparam) == BN_CLICKED) {
            try {
                choose_settings_folder(window, state->log_edit, L"Select log folder");
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_message(ex).c_str(), L"Settings", MB_ICONERROR);
            }
            return 0;
        }
        if ((id == IDC_SETTINGS_METADATA_BROWSE || id == IDC_SETTINGS_TEMP_BROWSE) &&
            HIWORD(wparam) == BN_CLICKED) {
            try {
                HWND target = id == IDC_SETTINGS_METADATA_BROWSE ? state->metadata_edit : state->temp_edit;
                const wchar_t* title = id == IDC_SETTINGS_METADATA_BROWSE
                    ? L"Select metadata folder"
                    : L"Select temporary folder";
                choose_settings_folder(window, target, title);
            } catch (const std::exception& ex) {
                MessageBoxW(window, exception_message(ex).c_str(), L"Settings", MB_ICONERROR);
            }
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        return handle_static_control_color(window, message, wparam, lparam);
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

bool show_settings_dialog(HWND owner) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = settings_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);

    SettingsDialogState state;
    state.owner = owner;
    DisabledOwnerGuard owner_guard(owner);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME,
        kSettingsClass,
        L"Settings",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        700,
        424,
        owner,
        nullptr,
        instance,
        &state);
    if (!dialog) {
        throw AppError("CreateWindowEx failed for settings dialog");
    }
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    run_modal_window_loop(dialog);
    return state.saved;
}


} // namespace mmid::ui
