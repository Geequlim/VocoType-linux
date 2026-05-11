/*
 * VoCoType Fcitx5 Addon Implementation
 */

#include "vocotype.h"
#include <fcitx-config/iniparser.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/candidatelist.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/utf8.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <chrono>

namespace {

constexpr auto FCITX_CONFIG_PATH = "inputmethod/vocotype.conf";
constexpr uint64_t RECORDING_ANIMATION_INTERVAL_US = 200000;
constexpr uint64_t POLISH_POLL_INTERVAL_US = 100000;
constexpr uint64_t PTT_RELEASE_DEBOUNCE_US = 50000;
// Suppress only near-simultaneous duplicate commits from the same IC/client.
constexpr uint64_t DUPLICATE_COMMIT_SUPPRESS_US = 250000;
constexpr uint64_t XDOTOOL_SPACE_INJECT_GUARD_US = 1000000;

constexpr std::array<const char *, 8> RECORDING_ANIMATION_FRAMES = {
    "🟢 正在听 ●     ",
    "🟢 正在听  ●    ",
    "🟢 正在听   ●   ",
    "🟢 正在听    ●  ",
    "⚫ 正在听     ● ",
    "⚫ 正在听    ●  ",
    "⚫ 正在听   ●   ",
    "⚫ 正在听  ●    ",
};

constexpr std::array<const char *, 8> LONG_RECORDING_ANIMATION_FRAMES = {
    "✨ 正在听·将润色 ●     ",
    "✨ 正在听·将润色  ●    ",
    "✨ 正在听·将润色   ●   ",
    "✨ 正在听·将润色    ●  ",
    "✨ 正在听·将润色     ● ",
    "✨ 正在听·将润色    ●  ",
    "✨ 正在听·将润色   ●   ",
    "✨ 正在听·将润色  ●    ",
};

constexpr std::array<const char *, 8> POLISHING_ANIMATION_FRAMES = {
    "✨ 正在润色 ●     ",
    "✨ 正在润色  ●    ",
    "✨ 正在润色   ●   ",
    "✨ 正在润色    ●  ",
    "✨ 正在润色     ● ",
    "✨ 正在润色    ●  ",
    "✨ 正在润色   ●   ",
    "✨ 正在润色  ●    ",
};

class CommitCandidateWord final : public fcitx::CandidateWord {
public:
    explicit CommitCandidateWord(std::string text)
        : fcitx::CandidateWord(fcitx::Text(text)), text_(std::move(text)) {}

    void select(fcitx::InputContext *inputContext) const override {
        auto &inputPanel = inputContext->inputPanel();
        inputPanel.reset();
        inputPanel.setClientPreedit(fcitx::Text());
        inputContext->updatePreedit();
        inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        inputContext->commitString(text_);
    }

private:
    std::string text_;
};

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string stripTrailingCommitPeriod(std::string text) {
    if (text.ends_with("。")) {
        text.resize(text.size() - std::char_traits<char>::length("。"));
    } else if (text.ends_with(".")) {
        text.pop_back();
    }
    return text;
}

bool isAsciiWhitespaceOnly(const std::string &text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char ch) {
               return std::isspace(ch);
           });
}

bool hasBlockingModifiers(fcitx::KeyStates states) {
    return (states & fcitx::KeyState::Ctrl) ||
           (states & fcitx::KeyState::Alt) ||
           (states & fcitx::KeyState::Super) ||
           (states & fcitx::KeyState::Super2) ||
           (states & fcitx::KeyState::Hyper) ||
           (states & fcitx::KeyState::Hyper2) ||
           (states & fcitx::KeyState::Meta);
}

std::string clientProgram(fcitx::InputContext *ic) {
    return ic ? toLower(ic->program()) : std::string();
}

bool usesXdotoolSpaceInjection(fcitx::InputContext *ic) {
    const std::string program = clientProgram(ic);
    return program.find("ghostty") != std::string::npos ||
           program.find("blackbox") != std::string::npos;
}

std::string stopRecorderProcess(pid_t pid, int stdin_fd, FILE* stdout_file) {
    if (stdin_fd >= 0) {
        close(stdin_fd);
    }

    std::string audio_path;
    if (stdout_file) {
        char buffer[1024];
        if (fgets(buffer, sizeof(buffer), stdout_file) != nullptr) {
            audio_path = buffer;
            while (!audio_path.empty() &&
                   (audio_path.back() == '\n' || audio_path.back() == '\r')) {
                audio_path.pop_back();
            }
        }
        fclose(stdout_file);
    }

    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }

    return audio_path;
}

bool copyTextToWaylandClipboard(const std::string &text) {
    FILE *pipe = popen("wl-copy", "w");
    if (!pipe) {
        return false;
    }

    const size_t expected = text.size();
    const size_t written = fwrite(text.data(), 1, expected, pipe);
    const int status = pclose(pipe);
    return written == expected && status == 0;
}

bool sendSpaceWithXdotool() {
    const int status = std::system("xdotool key --clearmodifiers space");
    return status == 0;
}

bool pasteTextToX11Client(const std::string &text) {
    constexpr auto command =
        "python3 -c 'import subprocess, sys, tkinter as tk; "
        "data = sys.stdin.read(); "
        "root = tk.Tk(); root.withdraw(); "
        "sentinel = \"__VOCOTYPE_CLIPBOARD_EMPTY__\"; "
        "previous = root.tk.eval(\"if {[catch {clipboard get} result]} {set result {__VOCOTYPE_CLIPBOARD_EMPTY__}}; set result\"); "
        "root.clipboard_clear(); root.clipboard_append(data); root.update(); "
        "root.after(50, lambda: subprocess.Popen([\"xdotool\", \"key\", \"--clearmodifiers\", \"ctrl+v\"])); "
        "root.after(800, lambda: (root.clipboard_clear(), None if previous == sentinel else root.clipboard_append(previous), root.update())); "
        "root.after(30000, root.destroy); "
        "root.mainloop()'";

    FILE *pipe = popen(command, "w");
    if (!pipe) {
        return false;
    }

    const size_t expected = text.size();
    const size_t written = fwrite(text.data(), 1, expected, pipe);
    const int status = pclose(pipe);
    return written == expected && status == 0;
}

} // namespace

namespace vocotype {

VoCoTypeAddon::VoCoTypeAddon(fcitx::Instance* instance)
    : instance_(instance),
      ipc_client_(std::make_unique<IPCClient>("/tmp/vocotype-fcitx5.sock")) {

    // 获取安装路径
    const char* home = std::getenv("HOME");
    if (home) {
        recorder_launcher_path_ = std::string(home) + "/.local/bin/vocotype-fcitx5-recorder";
    } else {
        FCITX_ERROR() << "HOME environment variable not set";
    }

    reloadConfig();

    FCITX_INFO() << "VoCoType Addon initialized";

    // 测试 Backend 连接
    if (ipc_client_->ping()) {
        FCITX_INFO() << "Backend connection OK";
    } else {
        FCITX_WARN() << "Backend not responding, please ensure fcitx5_server.py is running";
    }
}

VoCoTypeAddon::~VoCoTypeAddon() {
    cancelPendingRecordingStart();
    cancelPendingRecordingStop();
    stopRecordingAnimation();
    cancelActivePolishTask();
    if (recorder_pid_ > 0 || recorder_stdout_ || recorder_stdin_fd_ >= 0) {
        std::string audio_path =
            stopRecorderProcess(recorder_pid_, recorder_stdin_fd_, recorder_stdout_);
        if (!audio_path.empty()) {
            std::remove(audio_path.c_str());
        }
        recorder_pid_ = -1;
        recorder_stdin_fd_ = -1;
        recorder_stdout_ = nullptr;
        is_recording_ = false;
    }
    FCITX_INFO() << "VoCoType Addon destroyed";
}

void VoCoTypeAddon::reloadConfig() {
    fcitx::readAsIni(config_, fcitx::StandardPathsType::PkgConfig,
                     FCITX_CONFIG_PATH);
    applyHotkeyConfig();
}

void VoCoTypeAddon::save() {
    if (!fcitx::safeSaveAsIni(config_, fcitx::StandardPathsType::PkgConfig,
                              FCITX_CONFIG_PATH)) {
        FCITX_WARN() << "保存 Fcitx5 输入法配置失败: " << FCITX_CONFIG_PATH;
    }
}

const fcitx::Configuration* VoCoTypeAddon::getConfigForInputMethod(
    const fcitx::InputMethodEntry& entry) const {
    FCITX_UNUSED(entry);
    return &config_;
}

void VoCoTypeAddon::setConfigForInputMethod(
    const fcitx::InputMethodEntry& entry, const fcitx::RawConfig& config) {
    FCITX_UNUSED(entry);
    auto updated_config = config_;
    updated_config.load(config, true);
    config_ = updated_config;
    applyHotkeyConfig();
    save();
}

void VoCoTypeAddon::applyHotkeyConfig() {
    auto ptt_key = config_.pttKey.value().normalize();
    if (!ptt_key.isValid()) {
        ptt_key = fcitx::Key(FcitxKey_F9);
    }

    auto long_mode_modifier = config_.longModeModifier.value().normalize();
    auto modifier_state = fcitx::Key::keySymToStates(long_mode_modifier.sym());
    if (!long_mode_modifier.isValid() ||
        modifier_state == fcitx::KeyState::NoState) {
        long_mode_modifier = fcitx::Key(FcitxKey_Shift_L);
        modifier_state = fcitx::KeyState::Shift;
    }

    ptt_key_sym_ = ptt_key.sym();
    ptt_key_name_ = ptt_key.toString();
    ptt_hold_threshold_ms_ = config_.pttHoldThresholdMs.value();
    long_mode_modifier_ = modifier_state;
    long_mode_modifier_name_ = long_mode_modifier.toString();
    strip_trailing_period_on_commit_ =
        config_.stripTrailingPeriodOnCommit.value();

    FCITX_INFO() << "Fcitx5 热键配置: ptt=" << ptt_key_name_
                 << ", long_mode_modifier=" << long_mode_modifier_name_
                 << ", ptt_hold_threshold_ms=" << ptt_hold_threshold_ms_
                 << ", strip_trailing_period_on_commit="
                 << strip_trailing_period_on_commit_;
}

void VoCoTypeAddon::armPendingRecordingStart(fcitx::InputContext* ic, bool long_mode) {
    cancelPendingRecordingStart();
    ptt_pressed_ = true;
    pending_long_mode_ = long_mode;

    if (ptt_hold_threshold_ms_ <= 0) {
        startRecording(ic, long_mode);
        return;
    }

    auto ic_ref =
        ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    const uint64_t trigger_time =
        fcitx::now(CLOCK_MONOTONIC) +
        static_cast<uint64_t>(ptt_hold_threshold_ms_) * 1000ULL;
    ptt_hold_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        trigger_time,
        0,
        [this, ic_ref](fcitx::EventSourceTime*, uint64_t) {
            this->ptt_hold_timer_.reset();
            if (!this->ptt_pressed_ || this->is_recording_) {
                return false;
            }

            auto* ic_ptr = ic_ref.get();
            if (!ic_ptr) {
                this->ptt_pressed_ = false;
                this->pending_long_mode_ = false;
                return false;
            }

            this->startRecording(ic_ptr, this->pending_long_mode_);
            return false;
        });
    ptt_hold_timer_->setOneShot();
}

void VoCoTypeAddon::cancelPendingRecordingStart() {
    ptt_pressed_ = false;
    pending_long_mode_ = false;
    pending_ptt_states_ = fcitx::KeyState::NoState;
    ptt_hold_timer_.reset();
}

void VoCoTypeAddon::armPendingRecordingStop(fcitx::InputContext* ic) {
    cancelPendingRecordingStop();
    auto ic_ref =
        ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    const uint64_t trigger_time =
        fcitx::now(CLOCK_MONOTONIC) + PTT_RELEASE_DEBOUNCE_US;
    ptt_release_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        trigger_time,
        0,
        [this, ic_ref](fcitx::EventSourceTime*, uint64_t) {
            this->ptt_release_timer_.reset();
            auto* ic_ptr = ic_ref.get();
            if (!ic_ptr || !this->is_recording_) {
                return false;
            }
            this->cancelPendingRecordingStart();
            this->stopAndTranscribe(ic_ptr);
            return false;
        });
    ptt_release_timer_->setOneShot();
}

void VoCoTypeAddon::cancelPendingRecordingStop() {
    ptt_release_timer_.reset();
}

bool VoCoTypeAddon::forwardKeyToRime(fcitx::InputContext* ic, fcitx::KeySym keyval,
                                     fcitx::KeyStates states) {
    fcitx::Key key(keyval, states);
    if (isIMSwitchHotkey(key)) {
        return false;
    }

    // Let system/global shortcuts pass through in Chinese mode as well.
    // Otherwise combinations like Super+V / Hyper+E may be swallowed by IME.
    const bool has_system_shortcut_modifier =
        (states & fcitx::KeyState::Super) ||
        (states & fcitx::KeyState::Super2) ||
        (states & fcitx::KeyState::Hyper) ||
        (states & fcitx::KeyState::Hyper2) ||
        (states & fcitx::KeyState::Meta);
    if (has_system_shortcut_modifier) {
        return false;
    }

    int mask = 0;
    if (states & fcitx::KeyState::Shift) {
        mask |= (1 << 0);  // kShiftMask
    }
    if (states & fcitx::KeyState::CapsLock) {
        mask |= (1 << 1);  // kLockMask
    }
    if (states & fcitx::KeyState::Ctrl) {
        mask |= (1 << 2);  // kControlMask
    }
    if (states & fcitx::KeyState::Alt) {
        mask |= (1 << 3);  // kAltMask
    }

    try {
        RimeUIState state = ipc_client_->processKey(keyval, mask);
        const bool has_commit_text = !state.commit_text.empty();

        if (has_commit_text) {
            commitText(ic, state.commit_text);
        }

        updateUI(ic, state);

        if (state.handled || has_commit_text) {
            return true;
        }
    } catch (const std::exception& e) {
        FCITX_ERROR() << "Rime key processing failed: " << e.what();
    }

    return false;
}

bool VoCoTypeAddon::handlePendingFallbackKey(fcitx::InputContext* ic,
                                             fcitx::KeySym keyval,
                                             fcitx::KeyStates states,
                                             bool is_release) {
    if (pending_fallback_text_.empty()) {
        return false;
    }

    if (is_release) {
        return keyval == FcitxKey_1 || keyval == FcitxKey_space ||
               keyval == FcitxKey_Return || keyval == FcitxKey_KP_Enter ||
               keyval == FcitxKey_Escape;
    }

    const bool has_modifier = (states & fcitx::KeyState::Ctrl) ||
                              (states & fcitx::KeyState::Alt) ||
                              (states & fcitx::KeyState::Super);
    if (!has_modifier &&
        (keyval == FcitxKey_1 || keyval == FcitxKey_space ||
         keyval == FcitxKey_Return || keyval == FcitxKey_KP_Enter)) {
        std::string text = pending_fallback_text_;
        pending_fallback_text_.clear();
        commitText(ic, text);
        return true;
    }

    if (keyval == FcitxKey_Escape) {
        pending_fallback_text_.clear();
        clearUI(ic);
        return true;
    }

    pending_fallback_text_.clear();
    clearUI(ic);
    return false;
}

bool VoCoTypeAddon::isBareShiftToggleKey(fcitx::KeySym keyval,
                                         fcitx::KeyStates states) const {
    if (keyval != FcitxKey_Shift_L && keyval != FcitxKey_Shift_R) {
        return false;
    }

    const bool has_other_modifiers =
        (states & fcitx::KeyState::Ctrl) ||
        (states & fcitx::KeyState::Alt) ||
        (states & fcitx::KeyState::Super) ||
        (states & fcitx::KeyState::Super2) ||
        (states & fcitx::KeyState::Hyper) ||
        (states & fcitx::KeyState::Hyper2) ||
        (states & fcitx::KeyState::Meta);
    return !has_other_modifiers;
}

void VoCoTypeAddon::cancelShiftToggle() {
    shift_toggle_armed_ = false;
    pending_shift_toggle_key_ = static_cast<fcitx::KeySym>(0);
}

void VoCoTypeAddon::updateRawCompositionBuffer(fcitx::KeySym keyval,
                                               fcitx::KeyStates states,
                                               bool is_release) {
    if (is_release) {
        return;
    }

    const bool has_blocking_modifiers =
        (states & fcitx::KeyState::Ctrl) ||
        (states & fcitx::KeyState::Alt) ||
        (states & fcitx::KeyState::Super) ||
        (states & fcitx::KeyState::Super2) ||
        (states & fcitx::KeyState::Hyper) ||
        (states & fcitx::KeyState::Hyper2) ||
        (states & fcitx::KeyState::Meta);
    if (has_blocking_modifiers) {
        return;
    }

    if (keyval == FcitxKey_BackSpace) {
        if (!raw_composition_buffer_.empty()) {
            raw_composition_buffer_.pop_back();
        }
        return;
    }

    if (keyval == FcitxKey_Escape) {
        clearRawCompositionBuffer();
        return;
    }

    std::string text = fcitx::Key::keySymToUTF8(keyval);
    if (text.size() != 1) {
        return;
    }

    unsigned char ch = static_cast<unsigned char>(text[0]);
    if (std::isalnum(ch) || ch == '\'' || ch == ';') {
        raw_composition_buffer_.push_back(static_cast<char>(std::tolower(ch)));
    }
}

void VoCoTypeAddon::clearRawCompositionBuffer() {
    raw_composition_buffer_.clear();
}

void VoCoTypeAddon::showModeIndicator(fcitx::InputContext* ic,
                                      const std::string& indicator) {
    mode_indicator_timer_.reset();
    auto& inputPanel = ic->inputPanel();
    fcitx::Text indicator_text;
    indicator_text.append(indicator);
    inputPanel.setAuxUp(indicator_text);
    inputPanel.setAuxDown(fcitx::Text());
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);

    auto ic_ref =
        ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    mode_indicator_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        fcitx::now(CLOCK_MONOTONIC) + 800000,
        0,
        [this, ic_ref](fcitx::EventSourceTime*, uint64_t) {
            mode_indicator_timer_.reset();
            auto* ic_ptr = ic_ref.get();
            if (!ic_ptr) {
                return false;
            }
            auto& panel = ic_ptr->inputPanel();
            panel.setAuxUp(fcitx::Text());
            panel.setAuxDown(fcitx::Text());
            ic_ptr->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            return false;
        });
    mode_indicator_timer_->setOneShot();
}

bool VoCoTypeAddon::handleUnhandledSpace(fcitx::InputContext* ic,
                                         const char* reason) {
    if (usesXdotoolSpaceInjection(ic) &&
        injectUnhandledSpaceWithXdotool(ic, reason)) {
        return true;
    }

    commitText(ic, " ");
    return true;
}

bool VoCoTypeAddon::injectUnhandledSpaceWithXdotool(fcitx::InputContext* ic,
                                                    const char* reason) {
    if (!std::getenv("DISPLAY")) {
        FCITX_WARN() << "Cannot inject space without DISPLAY: program="
                     << ic->program()
                     << ", frontend=" << std::string(ic->frontendName());
        return false;
    }

    space_injection_passthrough_until_us_ =
        fcitx::now(CLOCK_MONOTONIC) + XDOTOOL_SPACE_INJECT_GUARD_US;
    std::thread([program = ic->program(), frontend = std::string(ic->frontendName()),
                 reason = std::string(reason)]() {
        if (!sendSpaceWithXdotool()) {
            FCITX_WARN() << "xdotool space injection failed: reason=" << reason
                         << ", program=" << program
                         << ", frontend=" << frontend;
        }
    }).detach();

    FCITX_INFO() << "Injected space via xdotool: reason=" << reason
                 << ", program=" << ic->program()
                 << ", frontend=" << std::string(ic->frontendName());
    return true;
}

void VoCoTypeAddon::replayShortTapAsRegularKey(fcitx::InputContext* ic) {
    const auto states = pending_ptt_states_;
    cancelPendingRecordingStart();

    if (forwardKeyToRime(ic, ptt_key_sym_, states)) {
        return;
    }

    if ((states & fcitx::KeyState::Ctrl) || (states & fcitx::KeyState::Alt)) {
        return;
    }

    if (ptt_key_sym_ == FcitxKey_space) {
        handleUnhandledSpace(ic, "short_tap");
        return;
    }

    std::string text = fcitx::Key::keySymToUTF8(ptt_key_sym_);
    if (text.empty()) {
        if (ptt_key_sym_ == FcitxKey_Return || ptt_key_sym_ == FcitxKey_KP_Enter) {
            text = "\n";
        } else {
            const int time = 0;
            // Non-character key: replay raw key event so short taps are not swallowed.
            ic->forwardKey(fcitx::Key(ptt_key_sym_, states), false, time);
            ic->forwardKey(fcitx::Key(ptt_key_sym_, states), true, time);
            return;
        }
    }

    commitText(ic, text);
}

void VoCoTypeAddon::showPanelMessage(fcitx::InputContext* ic, const std::string& message) {
    pending_fallback_text_.clear();
    auto& inputPanel = ic->inputPanel();
    fcitx::Text panel_text;
    panel_text.append(message);

    inputPanel.setClientPreedit(fcitx::Text());
    inputPanel.setPreedit(fcitx::Text());
    inputPanel.setAuxUp(panel_text);
    inputPanel.setAuxDown(fcitx::Text());
    inputPanel.setCandidateList(nullptr);
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VoCoTypeAddon::showAnimationFrame(fcitx::InputContext* ic) {
    const auto *frames = &RECORDING_ANIMATION_FRAMES;
    if (panel_animation_kind_ == PanelAnimationKind::RecordingLong) {
        frames = &LONG_RECORDING_ANIMATION_FRAMES;
    } else if (panel_animation_kind_ == PanelAnimationKind::Polishing) {
        frames = &POLISHING_ANIMATION_FRAMES;
    }

    const auto &frame =
        (*frames)[recording_animation_frame_index_ % frames->size()];
    showPanelMessage(ic, frame);
    recording_animation_frame_index_ =
        (recording_animation_frame_index_ + 1) % frames->size();
}

void VoCoTypeAddon::stopRecordingAnimation() {
    recording_animation_timer_.reset();
    recording_animation_frame_index_ = 0;
    panel_animation_kind_ = PanelAnimationKind::None;
}

void VoCoTypeAddon::startPanelAnimation(fcitx::InputContext* ic,
                                        PanelAnimationKind kind) {
    stopRecordingAnimation();
    panel_animation_kind_ = kind;
    showAnimationFrame(ic);

    auto ic_ref =
        ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    auto schedule_next = std::make_shared<std::function<void()>>();
    *schedule_next = [this, ic_ref, schedule_next]() {
        recording_animation_timer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC,
            fcitx::now(CLOCK_MONOTONIC) + RECORDING_ANIMATION_INTERVAL_US,
            0,
            [this, ic_ref, schedule_next](fcitx::EventSourceTime*, uint64_t) {
                recording_animation_timer_.reset();
                if (panel_animation_kind_ == PanelAnimationKind::None) {
                    recording_animation_frame_index_ = 0;
                    return false;
                }

                auto* ic_ptr = ic_ref.get();
                if (!ic_ptr) {
                    panel_animation_kind_ = PanelAnimationKind::None;
                    recording_animation_frame_index_ = 0;
                    return false;
                }

                showAnimationFrame(ic_ptr);
                (*schedule_next)();
                return false;
            });
        recording_animation_timer_->setOneShot();
    };
    (*schedule_next)();
}

void VoCoTypeAddon::startRecordingAnimation(fcitx::InputContext* ic) {
    startPanelAnimation(ic, PanelAnimationKind::Recording);
}

void VoCoTypeAddon::startLongRecordingAnimation(fcitx::InputContext* ic) {
    startPanelAnimation(ic, PanelAnimationKind::RecordingLong);
}

void VoCoTypeAddon::startPolishingAnimation(fcitx::InputContext* ic) {
    startPanelAnimation(ic, PanelAnimationKind::Polishing);
}

void VoCoTypeAddon::startPolishPolling(fcitx::InputContext* ic,
                                       const std::string& task_id) {
    stopRecordingAnimation();
    active_polish_task_id_ = task_id;
    active_polish_preview_.clear();
    active_polish_original_.clear();
    active_polish_after_seq_ = 0;
    polish_poll_in_flight_ = false;
    polish_poll_timer_.reset();
    showPanelMessage(ic, "⏳ 识别中...");
    auto ic_ref =
        ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    schedulePolishPoll(ic_ref);
}

void VoCoTypeAddon::schedulePolishPoll(
    fcitx::TrackableObjectReference<fcitx::InputContext> ic_ref) {
    if (active_polish_task_id_.empty() || polish_poll_timer_ ||
        polish_poll_in_flight_) {
        return;
    }

    polish_poll_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        fcitx::now(CLOCK_MONOTONIC) + POLISH_POLL_INTERVAL_US,
        0,
        [this, ic_ref](fcitx::EventSourceTime*, uint64_t) {
            polish_poll_timer_.reset();
            if (active_polish_task_id_.empty() || polish_poll_in_flight_) {
                return false;
            }

            std::string task_id = active_polish_task_id_;
            int after_seq = active_polish_after_seq_;
            polish_poll_in_flight_ = true;

            std::thread([this, ic_ref, task_id, after_seq]() {
                PolishPollResult result =
                    ipc_client_->pollPolishTask(task_id, after_seq);
                instance_->eventDispatcher().scheduleWithContext(
                    ic_ref, [this, ic_ref, task_id, result]() {
                        auto* ic_ptr = ic_ref.get();
                        polish_poll_in_flight_ = false;
                        if (!ic_ptr || task_id != active_polish_task_id_) {
                            return;
                        }
                        handlePolishPollResult(ic_ptr, result);
                    });
            }).detach();
            return false;
        });
    polish_poll_timer_->setOneShot();
}

void VoCoTypeAddon::handlePolishPollResult(fcitx::InputContext* ic,
                                           const PolishPollResult& result) {
    if (!result.success) {
        active_polish_task_id_.clear();
        polish_poll_timer_.reset();
        showError(ic, result.error.empty() ? "润色任务失败" : result.error,
                  active_polish_original_);
        return;
    }

    active_polish_after_seq_ = result.last_seq;
    if (!result.original_text.empty()) {
        active_polish_original_ = result.original_text;
    }
    if (!result.preview.empty()) {
        active_polish_preview_ = result.preview;
    }

    std::string status_text =
        result.phase == "asr" ? "⏳ 识别中..." : "✨ 正在润色...";
    for (const auto& event : result.events) {
        if (event.kind == "status" && !event.text.empty()) {
            status_text = event.text;
        } else if (event.kind == "delta" && !event.preview.empty()) {
            active_polish_preview_ = event.preview;
        }
    }

    if (result.status == "final") {
        std::string final_text = result.final_text.empty()
                                     ? active_polish_preview_
                                     : result.final_text;
        active_polish_task_id_.clear();
        polish_poll_timer_.reset();
        if (!final_text.empty()) {
            commitText(ic, final_text, strip_trailing_period_on_commit_);
        } else {
            clearUI(ic);
        }
        return;
    }

    if (result.status == "error" || result.status == "cancelled") {
        std::string error = result.error.empty() ? "润色失败" : result.error;
        std::string fallback = result.original_text.empty()
                                   ? active_polish_original_
                                   : result.original_text;
        active_polish_task_id_.clear();
        polish_poll_timer_.reset();
        showError(ic, error, fallback);
        return;
    }

    showPolishProgress(ic, status_text, active_polish_preview_,
                       active_polish_original_);
    auto ic_ref =
        ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
    schedulePolishPoll(ic_ref);
}

void VoCoTypeAddon::showPolishProgress(fcitx::InputContext* ic,
                                       const std::string& status,
                                       const std::string& preview,
                                       const std::string& original_text) {
    stopRecordingAnimation();
    pending_fallback_text_.clear();

    auto& inputPanel = ic->inputPanel();
    fcitx::Text panel_text;
    panel_text.append(status.empty() ? "✨ 正在润色..." : status);
    inputPanel.setClientPreedit(fcitx::Text());
    inputPanel.setPreedit(fcitx::Text());
    inputPanel.setAuxUp(panel_text);
    inputPanel.setAuxDown(fcitx::Text());

    auto candidateList = std::make_unique<fcitx::CommonCandidateList>();
    candidateList->setPageSize(2);
    candidateList->setCursorPositionAfterPaging(
        fcitx::CursorPositionAfterPaging::ResetToFirst);
    candidateList->setSelectionKey({fcitx::Key(FcitxKey_1),
                                    fcitx::Key(FcitxKey_2)});

    fcitx::Text preview_text;
    preview_text.append(preview.empty() ? "等待模型输出..." : preview);
    candidateList->append<fcitx::DisplayOnlyCandidateWord>(preview_text);

    if (!original_text.empty()) {
        fcitx::Text original_candidate;
        original_candidate.append(original_text);
        candidateList->append<fcitx::DisplayOnlyCandidateWord>(original_candidate);
    }

    candidateList->setGlobalCursorIndex(0);
    inputPanel.setCandidateList(std::move(candidateList));
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VoCoTypeAddon::cancelActivePolishTask() {
    polish_poll_timer_.reset();
    polish_poll_in_flight_ = false;
    if (!active_polish_task_id_.empty()) {
        (void)ipc_client_->cancelPolishTask(active_polish_task_id_);
    }
    active_polish_task_id_.clear();
    active_polish_preview_.clear();
    active_polish_original_.clear();
    active_polish_after_seq_ = 0;
}

std::vector<fcitx::InputMethodEntry> VoCoTypeAddon::listInputMethods() {
    std::vector<fcitx::InputMethodEntry> result;

    auto entry = fcitx::InputMethodEntry("vocotype", "VoCoType", "zh_CN", "vocotype");
    entry.setNativeName("语音输入");
    entry.setIcon("microphone");
    entry.setLabel("🎤");

    result.push_back(std::move(entry));
    return result;
}

void VoCoTypeAddon::keyEvent(const fcitx::InputMethodEntry& entry,
                              fcitx::KeyEvent& keyEvent) {
    auto ic = keyEvent.inputContext();

    // 获取按键信息
    auto key = keyEvent.key();
    auto keyval = key.sym();
    bool is_release = keyEvent.isRelease();
    const bool is_plain_space =
        keyval == FcitxKey_space && !hasBlockingModifiers(key.states());
    const uint64_t now_us = fcitx::now(CLOCK_MONOTONIC);

    if (is_plain_space && usesXdotoolSpaceInjection(ic) &&
        now_us < space_injection_passthrough_until_us_) {
        FCITX_INFO() << "Passing through injected space: program="
                     << ic->program()
                     << ", frontend=" << std::string(ic->frontendName());
        return;
    }

    FCITX_DEBUG() << "Key event: keyval=" << keyval
                  << ", release=" << is_release
                  << ", ptt_key=" << ptt_key_name_;

    const auto modifier_state = fcitx::Key::keySymToStates(keyval);
    const bool is_long_mode_modifier_key =
        modifier_state != fcitx::KeyState::NoState &&
        modifier_state == long_mode_modifier_;

    if (is_long_mode_modifier_key && (is_recording_ || ptt_pressed_)) {
        cancelShiftToggle();

        if (!is_release) {
            if (is_recording_) {
                recording_long_mode_ = !recording_long_mode_;
                if (recording_long_mode_) {
                    startLongRecordingAnimation(ic);
                    FCITX_INFO() << "Recording toggled to long mode via modifier key";
                } else {
                    startRecordingAnimation(ic);
                    FCITX_INFO() << "Recording toggled to normal mode via modifier key";
                }
            } else {
                pending_long_mode_ = !pending_long_mode_;
                FCITX_INFO() << "Pending recording toggled to "
                             << (pending_long_mode_ ? "long" : "normal")
                             << " mode via modifier key";
            }
        }

        keyEvent.filterAndAccept();
        return;
    }

    if (!is_release && shift_toggle_armed_ && keyval != pending_shift_toggle_key_) {
        cancelShiftToggle();
    }

    if (shift_toggle_armed_ && is_release && keyval == pending_shift_toggle_key_) {
        cancelShiftToggle();

        if (!ascii_mode_ && !raw_composition_buffer_.empty()) {
            commitText(ic, raw_composition_buffer_);
        }

        ipc_client_->reset();
        ascii_mode_ = !ascii_mode_;
        clearUI(ic);
        showModeIndicator(ic, ascii_mode_ ? "A" : "中");
        keyEvent.filterAndAccept();
        return;
    }

    if (isBareShiftToggleKey(keyval, key.states())) {
        if (!is_release) {
            shift_toggle_armed_ = true;
            pending_shift_toggle_key_ = keyval;
            keyEvent.filterAndAccept();
            return;
        }
    }

    if (!active_polish_task_id_.empty()) {
        if (is_release) {
            if (keyval == FcitxKey_Escape || keyval == FcitxKey_1 ||
                keyval == FcitxKey_2) {
                keyEvent.filterAndAccept();
                return;
            }
        } else if (keyval == FcitxKey_Escape) {
            cancelActivePolishTask();
            clearUI(ic);
            keyEvent.filterAndAccept();
            return;
        } else if (keyval == FcitxKey_2 && !active_polish_original_.empty()) {
            std::string original_text = active_polish_original_;
            cancelActivePolishTask();
            commitText(ic, original_text);
            keyEvent.filterAndAccept();
            return;
        } else {
            cancelActivePolishTask();
            clearUI(ic);
        }
    }

    if (handlePendingFallbackKey(ic, keyval, key.states(), is_release)) {
        keyEvent.filterAndAccept();
        return;
    }

    // 处理 PTT 键
    if (keyval == ptt_key_sym_) {
        if (!is_release && is_recording_ && ptt_release_timer_) {
            cancelPendingRecordingStop();
            keyEvent.filterAndAccept();
            return;
        }

        if (is_release) {
            if (is_recording_) {
                armPendingRecordingStop(ic);
            } else if (ptt_pressed_) {
                replayShortTapAsRegularKey(ic);
            } else {
                cancelPendingRecordingStart();
            }
        } else {
            const bool long_mode = bool(key.states() & long_mode_modifier_);
            if (!is_recording_ && !ptt_pressed_) {
                pending_ptt_states_ = key.states();
                armPendingRecordingStart(ic, long_mode);
            }
        }
        keyEvent.filterAndAccept();
        return;
    }

    if (ascii_mode_) {
        return;
    }

    updateRawCompositionBuffer(keyval, key.states(), is_release);

    // 其他键：转发给 Rime
    if (!is_release) {
        if (forwardKeyToRime(ic, keyval, key.states())) {
            keyEvent.filterAndAccept();
            return;
        }
        if (is_plain_space) {
            handleUnhandledSpace(ic, "unhandled_space");
            keyEvent.filterAndAccept();
            return;
        }
    }
}

void VoCoTypeAddon::reset(const fcitx::InputMethodEntry& entry,
                           fcitx::InputContextEvent& event) {
    auto ic = event.inputContext();
    cancelPendingRecordingStart();
    cancelPendingRecordingStop();
    cancelShiftToggle();
    cancelActivePolishTask();
    clearUI(ic);
    ipc_client_->reset();
}

void VoCoTypeAddon::activate(const fcitx::InputMethodEntry& entry,
                              fcitx::InputContextEvent& event) {
    FCITX_DEBUG() << "VoCoType activated";
}

void VoCoTypeAddon::deactivate(const fcitx::InputMethodEntry& entry,
                                fcitx::InputContextEvent& event) {
    auto ic = event.inputContext();
    cancelPendingRecordingStart();
    cancelPendingRecordingStop();
    cancelShiftToggle();
    cancelActivePolishTask();
    clearUI(ic);

    // 如果正在录音，停止录音但不转录
    if (is_recording_) {
        stopRecording(ic, false);
    }

    FCITX_DEBUG() << "VoCoType deactivated";
}

void VoCoTypeAddon::startRecording(fcitx::InputContext* ic, bool long_mode) {
    if (is_recording_) {
        return;
    }

    cancelActivePolishTask();
    ptt_hold_timer_.reset();
    cancelPendingRecordingStop();

    if (recorder_launcher_path_.empty()) {
        showError(ic, "录音配置无效");
        return;
    }

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        showError(ic, "启动录音失败");
        return;
    }
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        showError(ic, "启动录音失败");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        showError(ic, "启动录音失败");
        return;
    }

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

                execl(recorder_launcher_path_.c_str(),
              recorder_launcher_path_.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    FILE* stdout_file = fdopen(stdout_pipe[0], "r");
    if (!stdout_file) {
        close(stdout_pipe[0]);
        close(stdin_pipe[1]);
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        showError(ic, "启动录音失败");
        return;
    }

    recorder_pid_ = pid;
    recorder_stdin_fd_ = stdin_pipe[1];
    recorder_stdout_ = stdout_file;
    is_recording_ = true;
    ptt_pressed_ = true;
    pending_long_mode_ = false;
    recording_long_mode_ = long_mode;

    if (long_mode) {
        startLongRecordingAnimation(ic);
    } else {
        startRecordingAnimation(ic);
    }

    FCITX_INFO() << "Recording started, mode=" << (long_mode ? "long" : "normal");
}

void VoCoTypeAddon::stopAndTranscribe(fcitx::InputContext* ic) {
    stopRecording(ic, true);
}

void VoCoTypeAddon::stopRecording(fcitx::InputContext* ic, bool transcribe) {
    if (!is_recording_) {
        return;
    }

    ptt_hold_timer_.reset();
    cancelPendingRecordingStop();
    stopRecordingAnimation();
    ptt_pressed_ = false;
    pending_long_mode_ = false;
    is_recording_ = false;
    const bool long_mode = recording_long_mode_;
    recording_long_mode_ = false;

    if (ic) {
        if (transcribe) {
            if (long_mode) {
                startPolishingAnimation(ic);
            } else {
                showPanelMessage(ic, "⏳ 识别中...");
            }
        } else {
            clearUI(ic);
        }
    }

    pid_t pid = recorder_pid_;
    int stdin_fd = recorder_stdin_fd_;
    FILE* stdout_file = recorder_stdout_;
    recorder_pid_ = -1;
    recorder_stdin_fd_ = -1;
    recorder_stdout_ = nullptr;

    auto ic_ref =
        ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();

    std::thread([this, pid, stdin_fd, stdout_file, transcribe, long_mode, ic_ref]() mutable {
        std::string audio_path = stopRecorderProcess(pid, stdin_fd, stdout_file);
        if (audio_path.empty()) {
            if (transcribe) {
                instance_->eventDispatcher().scheduleWithContext(
                    ic_ref, [this, ic_ref]() {
                        auto* ic_ptr = ic_ref.get();
                        if (ic_ptr) {
                            showError(ic_ptr, "录音失败");
                        }
                    });
            }
            return;
        }

        if (!transcribe) {
            std::remove(audio_path.c_str());
            return;
        }

        if (long_mode) {
            TranscribeStartResult result =
                ipc_client_->startTranscription(audio_path, long_mode);
            if (!result.success || result.task_id.empty()) {
                std::remove(audio_path.c_str());
            }

            instance_->eventDispatcher().scheduleWithContext(
                ic_ref, [this, ic_ref, result]() {
                    auto* ic_ptr = ic_ref.get();
                    if (!ic_ptr) {
                        return;
                    }
                    if (result.success && !result.task_id.empty()) {
                        startPolishPolling(ic_ptr, result.task_id);
                    } else {
                        showError(ic_ptr,
                                  result.error.empty() ? "转录失败" : result.error);
                    }
                });
            return;
        }

        TranscribeResult result = ipc_client_->transcribeAudio(audio_path, long_mode);
        std::remove(audio_path.c_str());

        instance_->eventDispatcher().scheduleWithContext(
            ic_ref, [this, ic_ref, result]() {
                auto* ic_ptr = ic_ref.get();
                if (!ic_ptr) {
                    return;
                }
                if (result.success && !result.text.empty()) {
                    commitText(ic_ptr, result.text, strip_trailing_period_on_commit_);
                } else if (!result.success) {
                    showError(ic_ptr,
                              result.error.empty() ? "转录失败" : result.error,
                              result.original_text);
                } else {
                    clearUI(ic_ptr);
                }
            });
    }).detach();

    FCITX_INFO() << "Recording stopped, mode=" << (long_mode ? "long" : "normal");
}

void VoCoTypeAddon::updateUI(fcitx::InputContext* ic, const RimeUIState& state) {
    stopRecordingAnimation();
    pending_fallback_text_.clear();
    mode_indicator_timer_.reset();
    auto& inputPanel = ic->inputPanel();
    inputPanel.setClientPreedit(fcitx::Text());
    inputPanel.setAuxUp(fcitx::Text());
    inputPanel.setAuxDown(fcitx::Text());

    // 更新预编辑
    if (!state.preedit_text.empty()) {
        fcitx::Text preedit;
        preedit.append(state.preedit_text, fcitx::TextFormatFlag::Underline);
        inputPanel.setClientPreedit(preedit);
        inputPanel.setPreedit(fcitx::Text());
    } else {
        inputPanel.setClientPreedit(fcitx::Text());
        inputPanel.setPreedit(fcitx::Text());
    }

    // 更新候选词
    if (!state.candidates.empty()) {
        auto candidateList = std::make_unique<fcitx::CommonCandidateList>();
        candidateList->setPageSize(state.page_size);
        candidateList->setCursorPositionAfterPaging(
            fcitx::CursorPositionAfterPaging::ResetToFirst);

        // 设置候选词选择键（数字 1-0）
        candidateList->setSelectionKey({
            fcitx::Key(FcitxKey_1), fcitx::Key(FcitxKey_2), fcitx::Key(FcitxKey_3),
            fcitx::Key(FcitxKey_4), fcitx::Key(FcitxKey_5), fcitx::Key(FcitxKey_6),
            fcitx::Key(FcitxKey_7), fcitx::Key(FcitxKey_8), fcitx::Key(FcitxKey_9),
            fcitx::Key(FcitxKey_0)
        });

        for (size_t i = 0; i < state.candidates.size(); ++i) {
            const auto& [text, comment] = state.candidates[i];
            FCITX_UNUSED(comment);
            fcitx::Text candidate_text;
            candidate_text.append(text);
            candidateList->append<fcitx::DisplayOnlyCandidateWord>(candidate_text);
        }

        int cursor_index = state.highlighted_index;
        if (cursor_index < 0 ||
            cursor_index >= static_cast<int>(state.candidates.size())) {
            cursor_index = 0;
        }
        candidateList->setGlobalCursorIndex(cursor_index);
        inputPanel.setCandidateList(std::move(candidateList));
    } else {
        inputPanel.setCandidateList(nullptr);
    }

    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VoCoTypeAddon::clearUI(fcitx::InputContext* ic) {
    stopRecordingAnimation();
    pending_fallback_text_.clear();
    mode_indicator_timer_.reset();
    clearRawCompositionBuffer();
    auto& inputPanel = ic->inputPanel();
    inputPanel.reset();
    inputPanel.setClientPreedit(fcitx::Text());
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

bool VoCoTypeAddon::pasteTextForClient(fcitx::InputContext* ic, const std::string& text) {
    const std::string program = toLower(ic->program());
    if (program.find("wechat") == std::string::npos) {
        return false;
    }

    const std::string session_type = toLower(std::getenv("XDG_SESSION_TYPE")
                                                 ? std::getenv("XDG_SESSION_TYPE")
                                                 : "");

    clearUI(ic);

    if (session_type == "x11") {
        auto ic_ref =
            ic ? ic->watch() : fcitx::TrackableObjectReference<fcitx::InputContext>();
        std::thread([this, ic_ref, program, text]() {
            if (!pasteTextToX11Client(text)) {
                FCITX_WARN() << "Failed to paste text through X11 clipboard bridge for program="
                             << program;
                instance_->eventDispatcher().scheduleWithContext(
                    ic_ref, [this, ic_ref, text]() {
                        auto* ic_ptr = ic_ref.get();
                        if (!ic_ptr) {
                            return;
                        }

                        // Clipboard bridge failed: fallback to direct commit to avoid text loss.
                        clearUI(ic_ptr);
                        if (ic_ptr->capabilityFlags() &
                            fcitx::CapabilityFlag::CommitStringWithCursor) {
                            ic_ptr->commitStringWithCursor(text, fcitx::utf8::length(text));
                        } else {
                            ic_ptr->commitString(text);
                        }

                        const uint64_t now = fcitx::now(CLOCK_MONOTONIC);
                        last_committed_ic_ = ic_ptr;
                        last_committed_program_ = ic_ptr->program();
                        last_committed_frontend_ = std::string(ic_ptr->frontendName());
                        last_committed_text_ = text;
                        last_commit_time_us_ = now;
                        FCITX_INFO() << "Fallback committed text after X11 bridge failure: program="
                                     << last_committed_program_
                                     << ", frontend=" << last_committed_frontend_
                                     << ", text=" << text;
                    });
                return;
            }
            FCITX_INFO() << "Pasted text through X11 clipboard bridge for program=" << program
                         << ", text=" << text;
        }).detach();
        return true;
    }

    if (!copyTextToWaylandClipboard(text)) {
        FCITX_WARN() << "Failed to copy text to clipboard for program=" << program;
        return false;
    }

    const int time = 0;
    ic->forwardKey(fcitx::Key(FcitxKey_v, fcitx::KeyState::Ctrl), false, time);
    ic->forwardKey(fcitx::Key(FcitxKey_v, fcitx::KeyState::Ctrl), true, time);
    FCITX_INFO() << "Pasted text through clipboard for program=" << program
                 << ", text=" << text;
    return true;
}

void VoCoTypeAddon::commitText(fcitx::InputContext* ic, const std::string& text,
                               bool strip_trailing_period) {
    const std::string commit_text = strip_trailing_period
                                        ? stripTrailingCommitPeriod(text)
                                        : text;
    clearRawCompositionBuffer();
    const uint64_t now = fcitx::now(CLOCK_MONOTONIC);
    const std::string current_program = ic->program();
    const std::string current_frontend(ic->frontendName());
    if (last_committed_ic_ == ic &&
        last_committed_text_ == commit_text &&
        last_committed_program_ == current_program &&
        last_committed_frontend_ == current_frontend &&
        now >= last_commit_time_us_ &&
        now - last_commit_time_us_ < DUPLICATE_COMMIT_SUPPRESS_US) {
        FCITX_WARN() << "Suppressed duplicate commit: program=" << current_program
                     << ", frontend=" << current_frontend
                     << ", text=" << commit_text;
        return;
    }

    if (pasteTextForClient(ic, commit_text)) {
        last_committed_ic_ = ic;
        last_committed_program_ = current_program;
        last_committed_frontend_ = current_frontend;
        last_committed_text_ = commit_text;
        last_commit_time_us_ = now;
        return;
    }

    clearUI(ic);
    if (!isAsciiWhitespaceOnly(commit_text) &&
        (ic->capabilityFlags() & fcitx::CapabilityFlag::CommitStringWithCursor)) {
        ic->commitStringWithCursor(commit_text, fcitx::utf8::length(commit_text));
    } else {
        ic->commitString(commit_text);
    }
    last_committed_ic_ = ic;
    last_committed_program_ = current_program;
    last_committed_frontend_ = current_frontend;
    last_committed_text_ = commit_text;
    last_commit_time_us_ = now;
    FCITX_INFO() << "Committed text: program=" << current_program
                 << ", frontend=" << current_frontend
                 << ", text=" << commit_text;
}

void VoCoTypeAddon::showError(fcitx::InputContext* ic, const std::string& error,
                              const std::string& original_text) {
    stopRecordingAnimation();

    if (!original_text.empty()) {
    pending_fallback_text_ = original_text;
        auto& inputPanel = ic->inputPanel();
        fcitx::Text panel_text;
        panel_text.append("❌ " + error);
        fcitx::Text hint_text;

        inputPanel.setClientPreedit(fcitx::Text());
        inputPanel.setPreedit(fcitx::Text());
        inputPanel.setAuxUp(panel_text);
        inputPanel.setAuxDown(hint_text);

        auto candidateList = std::make_unique<fcitx::CommonCandidateList>();
        candidateList->setPageSize(1);
        candidateList->setCursorPositionAfterPaging(
            fcitx::CursorPositionAfterPaging::ResetToFirst);
        candidateList->setSelectionKey({fcitx::Key(FcitxKey_1)});
        candidateList->append<CommitCandidateWord>(original_text);
        candidateList->setGlobalCursorIndex(0);
        inputPanel.setCandidateList(std::move(candidateList));

        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    pending_fallback_text_.clear();
    showPanelMessage(ic, "❌ " + error);

    // 简化：不自动清除，等待用户下次按键
    // 2 秒自动清除在 Fcitx5 中需要更复杂的实现
}

bool VoCoTypeAddon::isIMSwitchHotkey(const fcitx::Key& key) const {
    // 只拦截 Super+Space (输入法切换)，不拦截 Ctrl+Space (中英切换)
    if (key.sym() == FcitxKey_space) {
        if (key.states() & fcitx::KeyState::Super) {
            return true;
        }
    }

    // Ctrl+Shift 或 Alt+Shift
    if (key.sym() == FcitxKey_Shift_L || key.sym() == FcitxKey_Shift_R) {
        if (key.states() & fcitx::KeyState::Ctrl) {
            return true;
        }
        if (key.states() & fcitx::KeyState::Alt) {
            return true;
        }
    }

    return false;
}

} // namespace vocotype

// Fcitx5 插件注册
class VoCoTypeAddonFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new vocotype::VoCoTypeAddon(manager->instance());
    }
};

FCITX_ADDON_FACTORY(VoCoTypeAddonFactory);
