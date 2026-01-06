#include "MessageInput.h"

#include "MaaUtils/Encoding.h"
#include "MaaUtils/Logger.h"
#include "MaaUtils/Platform.h"
#include "MaaUtils/SafeWindows.hpp"

#include "InputUtils.h"

#pragma pack(push, 1)
struct LunaPacket {
    int32_t type;
    int32_t x;
    int32_t y;
};
#pragma pack(pop)

MAA_CTRL_UNIT_NS_BEGIN

MessageInput::~MessageInput()
{
    if (luna_pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(luna_pipe_);
        luna_pipe_ = INVALID_HANDLE_VALUE;
    }
    if (block_input_) {
        BlockInput(FALSE);
    }
}

void MessageInput::ensure_foreground()
{
    ::MaaNS::CtrlUnitNs::ensure_foreground(hwnd_);
}

bool MessageInput::send_or_post_w(UINT message, WPARAM wParam, LPARAM lParam)
{
    bool success = false;

    if (mode_ == Mode::PostMessage) {
        success = PostMessageW(hwnd_, message, wParam, lParam) != 0;
    }
    else {
        SendMessageW(hwnd_, message, wParam, lParam);
        success = true; // SendMessage 总是返回，除非窗口句柄无效
    }

    if (!success) {
        DWORD error = GetLastError();
        LogError << "Failed to" << mode_ << VAR(message) << VAR(wParam) << VAR(lParam) << VAR(error);
    }

    return success;
}

POINT MessageInput::client_to_screen(int x, int y)
{
    POINT point = { x, y };
    if (hwnd_) {
        ClientToScreen(hwnd_, &point);
    }
    return point;
}

void MessageInput::save_cursor_pos()
{
    GetCursorPos(&saved_cursor_pos_);
    cursor_pos_saved_ = true;
}

void MessageInput::restore_cursor_pos()
{
    if (cursor_pos_saved_) {
        SetCursorPos(saved_cursor_pos_.x, saved_cursor_pos_.y);
        cursor_pos_saved_ = false;
    }
}

LPARAM MessageInput::prepare_mouse_position(int x, int y)
{
    if (with_cursor_pos_) {
        // Genshin 模式：移动真实光标到目标位置
        POINT screen_pos = client_to_screen(x, y);
        SetCursorPos(screen_pos.x, screen_pos.y);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return MAKELPARAM(x, y);
}

std::pair<int, int> MessageInput::get_target_pos() const
{
    if (last_pos_set_) {
        return last_pos_;
    }

    // 未设置时返回窗口客户区中心
    RECT rect = {};
    if (hwnd_ && GetClientRect(hwnd_, &rect)) {
        return { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
    }
    return { 0, 0 };
}

MaaControllerFeature MessageInput::get_features() const
{
    return MaaControllerFeature_UseMouseDownAndUpInsteadOfClick | MaaControllerFeature_UseKeyboardDownAndUpInsteadOfClick;
}

bool MessageInput::click(int x, int y)
{
    LogError << "deprecated" << VAR(mode_) << VAR(with_cursor_pos_) << VAR(x) << VAR(y);
    return false;
}

bool MessageInput::swipe(int x1, int y1, int x2, int y2, int duration)
{
    LogError << "deprecated" << VAR(mode_) << VAR(with_cursor_pos_) << VAR(x1) << VAR(y1) << VAR(x2) << VAR(y2) << VAR(duration);
    return false;
}

bool MessageInput::touch_down(int contact, int x, int y, int pressure)
{
    // 1. Try Luna First
    // Note: touch_down usually implies moving to the point then pressing
    if (try_send_luna(LUNA_CMD_DOWN, x, y)) {
        last_pos_ = { x, y };
        last_pos_set_ = true;
        return true;
    }

    // 2. Fallback
    LogInfo << VAR(mode_) << VAR(with_cursor_pos_) << VAR(contact) << VAR(x) << VAR(y) << VAR(pressure);

    std::ignore = pressure;

    if (!hwnd_) {
        LogError << "hwnd_ is nullptr";
        return false;
    }

    MouseMessageInfo move_info;
    if (!contact_to_mouse_move_message(contact, move_info)) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "contact out of range" << VAR(contact);
        return false;
    }

    MouseMessageInfo down_info;
    if (!contact_to_mouse_down_message(contact, down_info)) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "contact out of range" << VAR(contact);
        return false;
    }

    ensure_foreground();

    if (block_input_) {
        BlockInput(TRUE);
    }

    if (with_cursor_pos_) {
        save_cursor_pos();
    }

    // 准备位置（with_cursor_pos_ 模式下会移动光标）并发送 MOVE 消息
    LPARAM lParam = prepare_mouse_position(x, y);

    if (!send_or_post_w(move_info.message, move_info.w_param, lParam)) {
        if (with_cursor_pos_) {
            restore_cursor_pos();
        }
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 发送 DOWN 消息
    if (!send_or_post_w(down_info.message, down_info.w_param, lParam)) {
        if (with_cursor_pos_) {
            restore_cursor_pos();
        }
        return false;
    }

    last_pos_ = { x, y };
    last_pos_set_ = true;

    return true;
}

bool MessageInput::touch_move(int contact, int x, int y, int pressure)
{
    // LogInfo << VAR(contact) << VAR(x) << VAR(y) << VAR(pressure);

    // 1. Try Luna First
    if (try_send_luna(LUNA_CMD_MOVE, x, y)) {
        last_pos_ = { x, y };
        last_pos_set_ = true;
        return true;
    }

    // 2. Fallback
    std::ignore = pressure;

    if (!hwnd_) {
        LogError << "hwnd_ is nullptr";
        return false;
    }

    MouseMessageInfo msg_info;
    if (!contact_to_mouse_move_message(contact, msg_info)) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "contact out of range" << VAR(contact);
        return false;
    }

    // 准备位置（with_cursor_pos_ 模式下会移动光标）并发送 MOVE 消息
    LPARAM lParam = prepare_mouse_position(x, y);

    if (!send_or_post_w(msg_info.message, msg_info.w_param, lParam)) {
        return false;
    }

    last_pos_ = { x, y };
    last_pos_set_ = true;

    return true;
}

bool MessageInput::touch_up(int contact)
{
    // 1. Try Luna First
    auto target_pos = get_target_pos();
    if (try_send_luna(LUNA_CMD_UP, target_pos.first, target_pos.second)) {
        return true;
    }

    // 2. Fallback
    
    LogInfo << VAR(mode_) << VAR(with_cursor_pos_) << VAR(contact);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    OnScopeLeave([this]() {
        if (block_input_) {
            BlockInput(FALSE);
        }
    });

    MouseMessageInfo msg_info;
    if (!contact_to_mouse_up_message(contact, msg_info)) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "contact out of range" << VAR(contact);
        return false;
    }

    auto target_pos = get_target_pos();
    if (!send_or_post_w(msg_info.message, msg_info.w_param, MAKELPARAM(target_pos.first, target_pos.second))) {
        return false;
    }

    // touch_up 时恢复光标位置（与 touch_down 配对）
    if (with_cursor_pos_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        restore_cursor_pos();
    }

    return true;
}

bool MessageInput::click_key(int key)
{
    LogError << "deprecated" << VAR(mode_) << VAR(with_cursor_pos_) << VAR(key);
    return false;
}

bool MessageInput::input_text(const std::string& text)
{
    LogInfo << VAR(mode_) << VAR(with_cursor_pos_) << VAR(text);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    // 文本输入仅发送 WM_CHAR
    for (const auto ch : to_u16(text)) {
        if (!send_or_post_w(WM_CHAR, static_cast<WPARAM>(ch), 0)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

bool MessageInput::key_down(int key)
{
    LogInfo << VAR(mode_) << VAR(key);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    LPARAM lParam = make_keydown_lparam(key);
    return send_or_post_w(WM_KEYDOWN, static_cast<WPARAM>(key), lParam);
}

bool MessageInput::key_up(int key)
{
    LogInfo << VAR(mode_) << VAR(key);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    LPARAM lParam = make_keyup_lparam(key);
    return send_or_post_w(WM_KEYUP, static_cast<WPARAM>(key), lParam);
}

bool MessageInput::scroll(int dx, int dy)
{
    LogInfo << VAR(mode_) << VAR(with_cursor_pos_) << VAR(dx) << VAR(dy);

    if (!hwnd_) {
        LogError << VAR(mode_) << VAR(with_cursor_pos_) << "hwnd_ is nullptr";
        return false;
    }

    ensure_foreground();

    if (block_input_) {
        BlockInput(TRUE);
    }

    OnScopeLeave([this]() {
        if (block_input_) {
            BlockInput(FALSE);
        }
    });

    auto target_pos = get_target_pos();

    if (with_cursor_pos_) {
        // 保存当前光标位置，并移动到上次记录的位置
        save_cursor_pos();
        POINT screen_pos = client_to_screen(target_pos.first, target_pos.second);
        SetCursorPos(screen_pos.x, screen_pos.y);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // WM_MOUSEWHEEL 的 lParam 应为屏幕坐标
    POINT screen_pos = client_to_screen(target_pos.first, target_pos.second);
    LPARAM lParam = MAKELPARAM(screen_pos.x, screen_pos.y);

    if (dy != 0) {
        WPARAM wParam = MAKEWPARAM(0, static_cast<short>(dy));
        if (!send_or_post_w(WM_MOUSEWHEEL, wParam, lParam)) {
            if (with_cursor_pos_) {
                restore_cursor_pos();
            }
            return false;
        }
    }

    if (dx != 0) {
        WPARAM wParam = MAKEWPARAM(0, static_cast<short>(dx));
        if (!send_or_post_w(WM_MOUSEHWHEEL, wParam, lParam)) {
            if (with_cursor_pos_) {
                restore_cursor_pos();
            }
            return false;
        }
    }

    if (with_cursor_pos_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // 恢复光标位置
        restore_cursor_pos();
    }

    return true;
}

bool MessageInput::connect_luna_pipe()
{
    if (luna_pipe_ != INVALID_HANDLE_VALUE) {
        return true;
    }
    
    // Try to open the named pipe created by Python
    luna_pipe_ = CreateFileW(
        L"\\\\.\\pipe\\MaaLunaPipe",
        GENERIC_WRITE,
        0,              // No sharing
        NULL,           // Default security attributes
        OPEN_EXISTING,  // Opens existing pipe
        0,              // Default attributes
        NULL            // No template file
    );

    if (luna_pipe_ == INVALID_HANDLE_VALUE) {
        // Only log once or periodically to avoid spamming if Luna isn't running
        // LogError << "Failed to connect to Luna Pipe. Error: " << GetLastError();
        return false;
    }
    
    return true;
}

bool MessageInput::try_send_luna(int type, int x, int y)
{
    if (!luna_available_) {
        return false;
    }

    if (!connect_luna_pipe()) {
        // If connection fails, assume Luna is not active, fallback to legacy
        // You might want a retry mechanism, but for now fallback is safer
        return false; 
    }

    LunaPacket packet;
    packet.type = type;
    packet.x = x;
    packet.y = y;

    DWORD bytes_written = 0;
    BOOL success = WriteFile(
        luna_pipe_,
        &packet,
        sizeof(LunaPacket),
        &bytes_written,
        NULL
    );

    if (!success || bytes_written != sizeof(LunaPacket)) {
        LogError << "Failed to write to Luna Pipe. Error: " << GetLastError();
        CloseHandle(luna_pipe_);
        luna_pipe_ = INVALID_HANDLE_VALUE;
        return false;
    }

    return true;
}



MAA_CTRL_UNIT_NS_END
