#include "Crypto/unpkg.h"
#include "Crypto/unself.h"
#include "yaml-cpp/yaml.h"
#include "Emu/Audio/Cubeb/CubebBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/SPURecompiler.h"
#include "util/bin_patch.h"
#include <set>
#include "Emu/IdManager.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/NullPadHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/pad_config_types.h"
#include "Emu/NP/rpcn_client.h"
#include "Emu/NP/rpcn_config.h"
#include "Emu/NP/rpcn_types.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_save_dialog.h"
#include "Emu/RSX/Overlays/overlay_trophy_notification.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/localized_string_id.h"
#include "Emu/system_config.h"
#include "Emu/system_config_types.h"
#include "Emu/system_progress.hpp"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"
#include "Input/ds3_pad_handler.h"
#include "Input/ds4_pad_handler.h"
#include "Input/dualsense_pad_handler.h"
#include "Input/hid_pad_handler.h"
#include "Input/pad_thread.h"
#include "Input/virtual_pad_handler.h"
#include "Loader/ISO.h"
#include "Loader/iso_cache.h"
#include "Loader/PSF.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "cellos/sys_sync.h"
#include "hidapi_libusb.h"
#include "libusb.h"
#include "rpcs3_version.h"
#include "rpcsx/fw/ps3/cellMsgDialog.h"
#include "rpcsx/fw/ps3/cellSysutil.h"
#include "rx/asm.hpp"
#include "rx/debug.hpp"
#include "util/File.h"
#include "util/JIT.h"
#include "util/StrFmt.h"
#include "util/StrUtil.h"
#include "util/Thread.h"
#include "util/console.h"
#include "util/fixed_typemap.hpp"
#include "util/logs.hpp"
#include "util/serialization.hpp"
#include "util/sysinfo.hpp"
#include <Emu/Io/pad_config.h>
#include <Emu/RSX/GSFrameBase.h>
#include <Emu/System.h>
#include <nlohmann/json.hpp>
#include <rpcsx/fw/ps3/cellSaveData.h>
#include <rpcsx/fw/ps3/sceNpTrophy.h>
#include <rx/Version.hpp>

#include <algorithm>
#include <android/log.h>
#include <cctype>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iterator>
#include <jni.h>
#include <optional>
#include <span>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"

struct AtExit {
  std::function<void()> cb;
  ~AtExit() { cb(); }
};

static bool g_initialized;
static std::atomic<ANativeWindow *> g_native_window;

extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;

// App-internal filesDir (/data/data/<pkg>/files). NOT exposed over MTP/USB and
// not readable by other apps. Used only for the RPCN secret file (rpcn.yml).
std::string g_android_internal_config_dir;

static std::mutex g_virtual_pad_mutex;
static std::shared_ptr<Pad> g_virtual_pad;

std::string g_input_config_override;
cfg_input_configurations g_cfg_input_configs;

LOG_CHANNEL(rpcsx_android, "ANDROID");

struct LogListener : logs::listener {
  LogListener() { logs::listener::add(this); }

  void log(u64 stamp, const logs::message &msg, const std::string &prefix,
           const std::string &text) override {
    int prio = 0;
    switch (static_cast<logs::level>(msg)) {
    case logs::level::always:
      prio = ANDROID_LOG_INFO;
      break;
    case logs::level::fatal:
      prio = ANDROID_LOG_FATAL;
      break;
    case logs::level::error:
      prio = ANDROID_LOG_ERROR;
      break;
    case logs::level::todo:
      prio = ANDROID_LOG_WARN;
      break;
    case logs::level::success:
      prio = ANDROID_LOG_INFO;
      break;
    case logs::level::warning:
      prio = ANDROID_LOG_WARN;
      break;
    case logs::level::notice:
      prio = ANDROID_LOG_DEBUG;
      break;
    case logs::level::trace:
      prio = ANDROID_LOG_VERBOSE;
      break;
    }

    __android_log_write(prio, "RPCS3", text.c_str());
  }
} static g_androidLogListener;

struct GraphicsFrame : GSFrameBase {
  mutable ANativeWindow *activeNativeWindow = nullptr;
  mutable int width = 0;
  mutable int height = 0;

  ~GraphicsFrame() {
    if (activeNativeWindow != nullptr) {
      ANativeWindow_release(activeNativeWindow);
    }
  }

  ANativeWindow *getNativeWindow() const {
    ANativeWindow *result;
    while ((result = g_native_window.load()) == nullptr) [[unlikely]] {
      if (Emu.IsStopped()) {
        return activeNativeWindow;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (result != activeNativeWindow) [[unlikely]] {
      ANativeWindow_acquire(result);

      if (activeNativeWindow != nullptr) {
        ANativeWindow_release(activeNativeWindow);
      }

      activeNativeWindow = result;

      width = ANativeWindow_getWidth(result);
      height = ANativeWindow_getHeight(result);
    }

    return result;
  }

  void close() override {}
  void reset() override {}
  bool shown() override { return true; }
  void hide() override {}
  void show() override {}
  void toggle_fullscreen() override {}

  void delete_context(draw_context_t ctx) override {}
  draw_context_t make_context() override { return nullptr; }
  void set_current(draw_context_t ctx) override {}
  void flip(draw_context_t ctx, bool skip_frame = false) override {}
  int client_width() override { return width; }
  int client_height() override { return height; }
  f64 client_display_rate() override { return 30.f; }
  bool has_alpha() override {
    return ANativeWindow_getFormat(getNativeWindow()) ==
           WINDOW_FORMAT_RGBA_8888;
  }

  display_handle_t handle() const override { return getNativeWindow(); }

  bool can_consume_frame() const override { return false; }

  void present_frame(std::vector<u8> &data, u32 pitch, u32 width, u32 height,
                     bool is_bgra) const override {}
  void take_screenshot(std::vector<u8> &&sshot_data, u32 sshot_width,
                       u32 sshot_height, bool is_bgra) override {}
};

void jit_announce(uptr, usz, std::string_view);

[[noreturn]] void report_fatal_error(std::string_view _text,
                                     bool is_html = false,
                                     bool include_help_text = true) {
  std::string buf;

  buf = std::string(_text);

  // Check if thread id is in string
  if (_text.find("\nThread id = "sv) == umax && !thread_ctrl::is_main()) {
    // Append thread id if it isn't already, except on main thread
    fmt::append(buf, "\n\nThread id = %u.", thread_ctrl::get_tid());
  }

  if (!g_tls_serialize_name.empty()) {
    fmt::append(buf, "\nSerialized Object: %s", g_tls_serialize_name);
  }

  const system_state state = Emu.GetStatus(false);

  if (state == system_state::stopped) {
    fmt::append(buf, "\nEmulation is stopped");
  } else {
    const std::string &name = Emu.GetTitleAndTitleID();
    fmt::append(buf, "\nTitle: \"%s\" (emulation is %s)",
                name.empty() ? "N/A" : name.data(),
                state == system_state::stopping ? "stopping" : "running");
  }

  fmt::append(buf, "\nBuild: \"%s\"", rpcs3::get_verbose_version());
  fmt::append(buf, "\nDate: \"%s\"", std::chrono::system_clock::now());

  __android_log_write(ANDROID_LOG_FATAL, "RPCS3", buf.c_str());

  jit_announce(0, 0, "");
  rx::breakpoint();
  std::abort();
  std::terminate();
}

void qt_events_aware_op(int repeat_duration_ms,
                        std::function<bool()> wrapped_op) {
  // The core uses this as its synchronous "wait until done" primitive during
  // Emu stop/kill (the predicate checks e.g. m_state == stopped). On desktop it
  // pumps the Qt event loop while polling; on Android there is no such loop on
  // the calling thread, so just poll the predicate.
  //
  // This was previously an empty stub ("/// ?????"), so every synchronous stop
  // wait returned immediately without actually waiting, letting callers proceed
  // while emulation teardown was still in flight (shutdown races / ordering bugs).
  //
  // These waits run on emulation/background threads (game-side process_exit ->
  // GracefulShutdown), never the UI thread, so blocking here is safe. A generous
  // total cap keeps a genuinely stalled teardown from blocking the caller forever
  // (it then just returns, no worse than the old stub for that pathological case).
  if (!wrapped_op) {
    return;
  }

  constexpr int max_wait_ms = 30'000;
  const int step_ms = repeat_duration_ms > 0 ? repeat_duration_ms : 1;

  for (int waited_ms = 0; !wrapped_op(); waited_ms += step_ms) {
    if (waited_ms >= max_wait_ms) {
      rpcsx_android.error(
          "qt_events_aware_op: operation did not complete within %dms", max_wait_ms);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
  }
}

static std::string unwrap(JNIEnv *env, jstring string) {
  auto resultBuffer = env->GetStringUTFChars(string, nullptr);
  std::string result(resultBuffer);
  env->ReleaseStringUTFChars(string, resultBuffer);
  return result;
}
static jstring wrap(JNIEnv *env, const std::string &string) {
  return env->NewStringUTF(string.c_str());
}
static jstring wrap(JNIEnv *env, const char *string) {
  return env->NewStringUTF(string);
}

static std::string fix_dir_path(std::string string) {
  if (!string.empty() && !string.ends_with('/')) {
    string += '/';
  }

  return string;
}

enum class FileType {
  Unknown,
  Pup,
  Pkg,
  Edat,
  Rap,
  Iso,
};

static FileType getFileType(const fs::file &file) {
  file.seek(0);
  if (PUPHeader pupHeader; file.read(pupHeader)) {
    if (pupHeader.magic == "SCEUF\0\0\0"_u64) {
      return FileType::Pup;
    }
  }

  file.seek(0);
  if (PKGHeader pkgHeader; file.read(pkgHeader)) {
    if (pkgHeader.pkg_magic == std::bit_cast<le_t<u32>>("\x7FPKG"_u32)) {
      return FileType::Pkg;
    }
  }

  file.seek(0);
  if (NPD_HEADER npdHeader; file.read(npdHeader)) {
    if (npdHeader.magic == "NPD\0"_u32) {
      return FileType::Edat;
    }
  }

  if (file.size() == 16) {
    return FileType::Rap;
  }

  {
    // ISO9660 volume-descriptor probe (sector 16). Works for plain AND
    // redump-encrypted ISOs (region 0 is unencrypted), unlike the old
    // full-directory-parse sniff which also walked the whole tree.
    char vd[6]{};
    if (file.read_at(0x8000, vd, sizeof(vd)) == sizeof(vd) &&
        std::memcmp(vd + 1, "CD001", 5) == 0) {
      return FileType::Iso;
    }
  }

  return FileType::Unknown;
}

#define MAKE_STRING(id, x) [int(localized_string_id::id)] = {x, U##x}

static std::pair<std::string, std::u32string> g_strings[] = {
    MAKE_STRING(RSX_OVERLAYS_COMPILING_SHADERS, "Compiling shaders"),
    MAKE_STRING(RSX_OVERLAYS_COMPILING_PPU_MODULES, "Compiling PPU Modules"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_YES, "Yes"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_NO, "No"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_OK, "OK"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_TITLE, "Save Dialog"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_DELETE, "Delete Save"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_LOAD, "Load Save"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_SAVE, "Save"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ACCEPT, "Enter"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_SPACE, "Space"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_BACKSPACE, "Backspace"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_SHIFT, "Shift"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ENTER_TEXT, "[Enter Text]"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ENTER_PASSWORD, "[Enter Password]"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_TITLE, "Select media"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_TITLE_PHOTO_IMPORT,
                "Select photo to import"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_EMPTY, "No media found."),
    MAKE_STRING(RSX_OVERLAYS_LIST_SELECT, "Enter"),
    MAKE_STRING(RSX_OVERLAYS_LIST_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_LIST_DENY, "Deny"),
    MAKE_STRING(CELL_OSK_DIALOG_TITLE, "On Screen Keyboard"),
    MAKE_STRING(
        CELL_OSK_DIALOG_BUSY,
        "The Home Menu can't be opened while the On Screen Keyboard is busy!"),
    MAKE_STRING(CELL_SAVEDATA_CB_BROKEN, "Error - Save data corrupted"),
    MAKE_STRING(CELL_SAVEDATA_CB_FAILURE, "Error - Failed to save or load"),
    MAKE_STRING(CELL_SAVEDATA_CB_NO_DATA, "Error - Save data cannot be found"),
    MAKE_STRING(CELL_SAVEDATA_NO_DATA, "There is no saved data."),
    MAKE_STRING(CELL_SAVEDATA_NEW_SAVED_DATA_TITLE, "New Saved Data"),
    MAKE_STRING(CELL_SAVEDATA_NEW_SAVED_DATA_SUB_TITLE,
                "Select to create a new entry"),
    MAKE_STRING(CELL_SAVEDATA_SAVE_CONFIRMATION,
                "Do you want to save this data?"),
    MAKE_STRING(CELL_SAVEDATA_AUTOSAVE, "Saving..."),
    MAKE_STRING(CELL_SAVEDATA_AUTOLOAD, "Loading..."),
    MAKE_STRING(
        CELL_CROSS_CONTROLLER_FW_MSG,
        "If your system software version on the PS Vita system is earlier than "
        "1.80, you must update the system software to the latest version."),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE, "Select Message"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE_INVITE, "Select Invite"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE_ADD_FRIEND, "Add Friend"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_FROM, "From:"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_SUBJECT, "Subject:"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE, "Select Message To Send"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE_INVITE, "Send Invite"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE_ADD_FRIEND, "Add Friend"),
    MAKE_STRING(CELL_NP_MESSAGE_INVITE_RECEIVED, "Received an invite from %0"),
    MAKE_STRING(CELL_NP_MESSAGE_OTHER_RECEIVED, "Received a message from %0"),
    MAKE_STRING(RECORDING_ABORTED, "Recording aborted!"),
    MAKE_STRING(RPCN_NO_ERROR, "RPCN: No Error"),
    MAKE_STRING(RPCN_ERROR_INVALID_INPUT,
                "RPCN: Invalid Input (Wrong Host/Port)"),
    MAKE_STRING(RPCN_ERROR_WOLFSSL, "RPCN Connection Error: WolfSSL Error"),
    MAKE_STRING(RPCN_ERROR_RESOLVE, "RPCN Connection Error: Resolve Error"),
    MAKE_STRING(RPCN_ERROR_BINDING, "RPCN Connection Error: Failed to bind to given binding IP"),
    MAKE_STRING(RPCN_ERROR_CONNECT, "RPCN Connection Error"),
    MAKE_STRING(RPCN_ERROR_LOGIN_ERROR,
                "RPCN Login Error: Identification Error"),
    MAKE_STRING(RPCN_ERROR_ALREADY_LOGGED,
                "RPCN Login Error: User Already Logged In"),
    MAKE_STRING(RPCN_ERROR_INVALID_LOGIN, "RPCN Login Error: Invalid Username"),
    MAKE_STRING(RPCN_ERROR_INVALID_PASSWORD,
                "RPCN Login Error: Invalid Password"),
    MAKE_STRING(RPCN_ERROR_INVALID_TOKEN, "RPCN Login Error: Invalid Token"),
    MAKE_STRING(RPCN_ERROR_INVALID_PROTOCOL_VERSION,
                "RPCN Misc Error: Protocol Version Error (outdated RPCS3?)"),
    MAKE_STRING(RPCN_ERROR_UNKNOWN, "RPCN: Unknown Error"),
    MAKE_STRING(RPCN_SUCCESS_LOGGED_ON, "Successfully logged on RPCN!"),
    MAKE_STRING(HOME_MENU_TITLE, "Home Menu"),
    MAKE_STRING(HOME_MENU_EXIT_GAME, "Exit Game"),
    MAKE_STRING(HOME_MENU_RESUME, "Resume Game"),
    MAKE_STRING(HOME_MENU_FRIENDS, "Friends"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUESTS, "Pending Friend Requests"),
    MAKE_STRING(HOME_MENU_FRIENDS_BLOCKED, "Blocked Users"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_ONLINE, "Online"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_OFFLINE, "Offline"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_BLOCKED, "Blocked"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUEST_SENT, "You sent a friend request"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUEST_RECEIVED,
                "Sent you a friend request"),
    MAKE_STRING(HOME_MENU_FRIENDS_REJECT_REQUEST, "Reject Request"),
    MAKE_STRING(HOME_MENU_FRIENDS_NEXT_LIST, "Next list"),
    MAKE_STRING(HOME_MENU_RESTART, "Restart Game"),
    MAKE_STRING(HOME_MENU_SETTINGS, "Settings"),
    MAKE_STRING(HOME_MENU_SETTINGS_SAVE, "Save custom configuration?"),
    MAKE_STRING(HOME_MENU_SETTINGS_SAVE_BUTTON, "Save"),
    MAKE_STRING(HOME_MENU_SETTINGS_RESET_BUTTON, "To default"),
    MAKE_STRING(HOME_MENU_TOGGLE_FULLSCREEN, "Toggle Fullscreen"),
    MAKE_STRING(HOME_MENU_SETTINGS_DISCARD,
                "Discard the current settings' changes?"),
    MAKE_STRING(HOME_MENU_SETTINGS_DISCARD_BUTTON, "Discard"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO, "Audio"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_MASTER_VOLUME, "Master Volume"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BACKEND, "Audio Backend"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BUFFERING, "Enable Buffering"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BUFFER_DURATION,
                "Desired Audio Buffer Duration"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_TIME_STRETCHING,
                "Enable Time Stretching"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_TIME_STRETCHING_THRESHOLD,
                "Time Stretching Threshold"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO, "Video"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_FRAME_LIMIT, "Frame Limit"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_ANISOTROPIC_OVERRIDE,
                "Anisotropic Filter Override"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_OUTPUT_SCALING, "Output Scaling"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_RCAS_SHARPENING,
                "FidelityFX CAS Sharpening Intensity"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_STRETCH_TO_DISPLAY,
                "Stretch To Display Area"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT, "Input"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_BACKGROUND_INPUT,
                "Background Input Enabled"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_KEEP_PADS_CONNECTED,
                "Keep Pads Connected"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_SHOW_PS_MOVE_CURSOR,
                "Show PS Move Cursor"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_CAMERA_FLIP, "Camera Flip"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_PAD_MODE, "Pad Handler Mode"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_PAD_SLEEP, "Pad Handler Sleep"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_FAKE_MOVE_ROTATION_CONE_H,
                "Fake PS Move Rotation Cone (Horizontal)"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_FAKE_MOVE_ROTATION_CONE_V,
                "Fake PS Move Rotation Cone (Vertical)"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED, "Advanced"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_PREFERRED_SPU_THREADS,
                "Preferred SPU Threads"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_MAX_CPU_PREEMPTIONS,
                "Max Power Saving CPU-Preemptions"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_ACCURATE_RSX_RESERVATION_ACCESS,
                "Accurate RSX reservation access"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_SLEEP_TIMERS_ACCURACY,
                "Sleep Timers Accuracy"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_MAX_SPURS_THREADS,
                "Max SPURS Threads"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_DRIVER_WAKE_UP_DELAY,
                "Driver Wake-Up Delay"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_VBLANK_FREQUENCY,
                "VBlank Frequency"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_VBLANK_NTSC, "VBlank NTSC Fixup"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS, "Overlays"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_TROPHY_POPUPS,
                "Show Trophy Popups"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_RPCN_POPUPS,
                "Show RPCN Popups"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_SHADER_COMPILATION_HINT,
                "Show Shader Compilation Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_PPU_COMPILATION_HINT,
                "Show PPU Compilation Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_AUTO_SAVE_LOAD_HINT,
                "Show Autosave/Autoload Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_PRESSURE_INTENSITY_TOGGLE_HINT,
                "Show Pressure Intensity Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_ANALOG_LIMITER_TOGGLE_HINT,
                "Show Analog Limiter Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_MOUSE_AND_KB_TOGGLE_HINT,
                "Show Mouse And Keyboard Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY, "Performance Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE,
                "Enable Performance Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE_FRAMERATE_GRAPH,
                "Enable Framerate Graph"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE_FRAMETIME_GRAPH,
                "Enable Frametime Graph"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_DETAIL_LEVEL,
                "Detail level"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMERATE_DETAIL_LEVEL,
                "Framerate Graph Detail Level"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMETIME_DETAIL_LEVEL,
                "Frametime Graph Detail Level"),
    MAKE_STRING(
        HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMERATE_DATAPOINT_COUNT,
        "Framerate Datapoints"),
    MAKE_STRING(
        HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMETIME_DATAPOINT_COUNT,
        "Frametime Datapoints"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_UPDATE_INTERVAL,
                "Metrics Update Interval"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_POSITION, "Position"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_CENTER_X,
                "Center Horizontally"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_CENTER_Y,
                "Center Vertically"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_MARGIN_X,
                "Horizontal Margin"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_MARGIN_Y,
                "Vertical Margin"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FONT_SIZE, "Font Size"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_OPACITY, "Opacity"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG, "Debug"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_OVERLAY, "Debug Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_INPUT_OVERLAY, "Input Debug Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_DISABLE_VIDEO_OUTPUT,
                "Disable Video Output"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_TEXTURE_LOD_BIAS,
                "Texture LOD Bias Addend"),
    MAKE_STRING(HOME_MENU_SCREENSHOT, "Take Screenshot"),
    MAKE_STRING(HOME_MENU_SAVESTATE, "SaveState"),
    MAKE_STRING(HOME_MENU_SAVESTATE_SAVE, "Save Emulation State"),
    MAKE_STRING(HOME_MENU_SAVESTATE_AND_EXIT, "Save Emulation State And Exit"),
    MAKE_STRING(HOME_MENU_RELOAD_SAVESTATE, "Reload Last Emulation State"),
    MAKE_STRING(HOME_MENU_RECORDING, "Start/Stop Recording"),
    MAKE_STRING(HOME_MENU_TROPHIES, "Trophies"),
    MAKE_STRING(HOME_MENU_TROPHY_HIDDEN_TITLE, "Hidden trophy"),
    MAKE_STRING(HOME_MENU_TROPHY_HIDDEN_DESCRIPTION, "This trophy is hidden"),
    MAKE_STRING(HOME_MENU_TROPHY_PLATINUM_RELEVANT, "Platinum relevant"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_BRONZE, "Bronze"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_SILVER, "Silver"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_GOLD, "Gold"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_PLATINUM, "Platinum"),
    MAKE_STRING(AUDIO_MUTED, "Audio muted"),
    MAKE_STRING(AUDIO_UNMUTED, "Audio unmuted"),
    MAKE_STRING(PROGRESS_DIALOG_PROGRESS, "Progress:"),
    MAKE_STRING(PROGRESS_DIALOG_PROGRESS_ANALYZING, "Progress: analyzing..."),
    MAKE_STRING(PROGRESS_DIALOG_REMAINING, "remaining"),
    MAKE_STRING(PROGRESS_DIALOG_DONE, "done"),
    MAKE_STRING(PROGRESS_DIALOG_FILE, "file"),
    MAKE_STRING(PROGRESS_DIALOG_MODULE, "module"),
    MAKE_STRING(PROGRESS_DIALOG_OF, "of"),
    MAKE_STRING(PROGRESS_DIALOG_PLEASE_WAIT, "Please wait"),
    MAKE_STRING(PROGRESS_DIALOG_STOPPING_PLEASE_WAIT,
                "Stopping. Please wait..."),
    MAKE_STRING(PROGRESS_DIALOG_SAVESTATE_PLEASE_WAIT,
                "Creating savestate. Please wait..."),
    MAKE_STRING(PROGRESS_DIALOG_SCANNING_PPU_EXECUTABLE,
                "Scanning PPU Executable..."),
    MAKE_STRING(PROGRESS_DIALOG_ANALYZING_PPU_EXECUTABLE,
                "Analyzing PPU Executable..."),
    MAKE_STRING(PROGRESS_DIALOG_SCANNING_PPU_MODULES,
                "Scanning PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_LOADING_PPU_MODULES, "Loading PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_COMPILING_PPU_MODULES,
                "Compiling PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_LINKING_PPU_MODULES, "Linking PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_APPLYING_PPU_CODE, "Applying PPU Code..."),
    MAKE_STRING(PROGRESS_DIALOG_BUILDING_SPU_CACHE, "Building SPU Cache..."),
    MAKE_STRING(EMULATION_PAUSED_RESUME_WITH_START,
                "Press and hold the START button to resume"),
    MAKE_STRING(EMULATION_RESUMING, "Resuming...!"),
    MAKE_STRING(EMULATION_FROZEN,
                "The PS3 application has likely crashed, you can close it."),
    MAKE_STRING(
        SAVESTATE_FAILED_DUE_TO_SAVEDATA,
        "SaveState failed: Game saving is in progress, wait until finished."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_VDEC,
                "SaveState failed: VDEC-base video/cutscenes are in order, "
                "wait for them to end or enable libvdec.sprx."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_MISSING_SPU_SETTING,
                "SaveState failed: Failed to lock SPU state, enabling "
                "SPU-Compatible mode may fix it."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_SPU,
                "SaveState failed: Failed to lock SPU state, using SPU ASMJIT "
                "will fix it."),
    MAKE_STRING(INVALID, "Invalid"),
};

enum GameFlags {
  kGameFlagLocked = 1 << 0,
  kGameFlagTrial = 1 << 1,
};

struct GameInfo {
  std::string path;
  std::string name;
  std::string iconPath;
  int flags = 0;
  std::string version;
  std::string titleId;
};

class Progress {
  JNIEnv *env;
  jlong progressId;
  jclass progressRepositoryClass;
  jmethodID onProgressEventMethodId;

public:
  Progress(JNIEnv *env, jlong progressId) : env(env), progressId(progressId) {
    progressRepositoryClass =
        ensure(env->FindClass("net/rpcsx/ProgressRepository"));
    onProgressEventMethodId = env->GetStaticMethodID(
        progressRepositoryClass, "onProgressEvent", "(JJJLjava/lang/String;)Z");
  }

  bool report(jlong value, jlong max, const std::string &message = {}) {
    return env->CallStaticBooleanMethod(
        progressRepositoryClass, onProgressEventMethodId, progressId, value,
        max, message.empty() ? nullptr : wrap(env, message));
  }

  void failure(const std::string &message = {}) { report(-1, 0, message); }

  void success(jlong value, const std::string &message = {}) {
    value = std::max<jlong>(value, 1);
    report(value, value, message);
  }

  jlong getProgressId() const { return progressId; }
};

static void sendFirmwareInstalled(JNIEnv *env, const std::string &version) {
  auto fwRepositoryClass =
      ensure(env->FindClass("net/rpcsx/FirmwareRepository"));
  auto methodId = ensure(env->GetStaticMethodID(
      fwRepositoryClass, "onFirmwareInstalled", "(Ljava/lang/String;)V"));

  env->CallStaticVoidMethod(fwRepositoryClass, methodId, wrap(env, version));
}

static void sendFirmwareCompiled(JNIEnv *env, const std::string &version) {
  auto fwRepositoryClass =
      ensure(env->FindClass("net/rpcsx/FirmwareRepository"));
  auto methodId = ensure(env->GetStaticMethodID(
      fwRepositoryClass, "onFirmwareCompiled", "(Ljava/lang/String;)V"));

  env->CallStaticVoidMethod(fwRepositoryClass, methodId, wrap(env, version));
}

static void sendGameInfo(JNIEnv *env, jlong progressId,
                         std::span<const GameInfo> infos) {
  auto gameRepositoryClass = ensure(env->FindClass("net/rpcsx/GameRepository"));
  auto addMethodId = ensure(env->GetStaticMethodID(
      gameRepositoryClass, "add", "([Lnet/rpcsx/GameInfo;J)V"));
  auto gameClass = ensure(env->FindClass("net/rpcsx/GameInfo"));

  // Prefer the 6-arg constructor (game version + title id); fall back to the
  // legacy 4-arg one so a new core keeps working with an older app.
  jmethodID gameConstructorV2 = env->GetMethodID(
      gameClass, "<init>",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)V");
  if (gameConstructorV2 == nullptr) {
    env->ExceptionClear();
  }

  jmethodID gameConstructor =
      gameConstructorV2 ? nullptr
                        : ensure(env->GetMethodID(
                              gameClass, "<init>",
                              "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V"));

  std::vector<jobject> objects;
  objects.reserve(infos.size());

  for (const auto &info : infos) {
    auto path = Emu.GetCallbacks().resolve_path(info.path);
    if (path.ends_with('/')) {
      path.resize(path.size() - 1);
    }

    if (gameConstructorV2) {
      objects.push_back(env->NewObject(
          gameClass, gameConstructorV2, wrap(env, path), wrap(env, info.name),
          wrap(env, Emu.GetCallbacks().resolve_path(info.iconPath)),
          jint(info.flags), wrap(env, info.version), wrap(env, info.titleId)));
    } else {
      objects.push_back(env->NewObject(
          gameClass, gameConstructor, wrap(env, path), wrap(env, info.name),
          wrap(env, Emu.GetCallbacks().resolve_path(info.iconPath)),
          jint(info.flags)));
    }
  }

  auto result = env->NewObjectArray(objects.size(), gameClass, nullptr);

  for (std::size_t i = 0; i < objects.size(); ++i) {
    env->SetObjectArrayElement(result, i, objects[i]);
  }

  env->CallStaticVoidMethod(gameRepositoryClass, addMethodId, result,
                            progressId);
}

static void sendVshBootable(JNIEnv *env, jlong progressId) {
  auto dev_flash = g_cfg_vfs.get_dev_flash();

  sendGameInfo(
      env, progressId,
      {{GameInfo{
          .path = dev_flash + "/vsh/module/vsh.self",
          .name = "VSH",
          .iconPath = dev_flash + "vsh/resource/explore/icon/icon_home.png",
      }}});
}

static bool tryUnlockGame(const psf::registry &psf) {
  auto contentId = psf::get_string(psf, "CONTENT_ID");

  if (contentId.empty()) {
    return true;
  }

  const auto licenseDir = fmt::format(
      "%shome/%s/exdata/", rpcs3::utils::get_hdd0_dir(), Emu.GetUsr());

  const auto licenseFile = fmt::format("%s%s", licenseDir, contentId);
  if (std::filesystem::is_regular_file(licenseFile + ".rap")) {
    return true;
  }

  if (std::filesystem::is_regular_file(licenseFile + ".edat")) {
    return true;
  }

  return false;
}

static bool hasIsoExtension(const std::filesystem::path &p) {
  auto ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return ext == ".iso";
}

static void collectGamePaths(std::vector<std::string> &paths,
                             const std::string &rootDir) {
  std::error_code ec;
  std::vector<std::filesystem::path> workList;
  workList.reserve(32);
  if (!std::filesystem::is_directory(rootDir)) {
    // A dropped/added .iso is a game entry by itself (played in place)
    if (hasIsoExtension(rootDir)) {
      paths.push_back(rootDir);
      return;
    }

    auto rootPath = std::filesystem::path(rootDir).parent_path();
    if (rootPath.filename() == "USRDIR") {
      rootPath = rootPath.parent_path();
    }
    if (rootPath.filename() == "PS3_GAME") {
      rootPath = rootPath.parent_path();
    }

    workList.push_back(rootPath);
  } else {
    workList.push_back(rootDir);
  }

  while (!workList.empty()) {
    auto dir = std::move(workList.back());
    workList.pop_back();

    for (auto &entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.is_directory()) {
        if (entry.path().filename() != "C00") {
          workList.push_back(entry.path());
        }

        continue;
      }

      if (entry.is_regular_file() && entry.path().filename() == "PARAM.SFO") {
        paths.push_back(entry.path().parent_path().string());
        continue;
      }

      // Direct-play ISO entries (no extraction)
      if (entry.is_regular_file() && hasIsoExtension(entry.path())) {
        paths.push_back(entry.path().string());
        continue;
      }
    }
  }
}

static std::string locateEbootPath(std::string_view root) {
  if (std::filesystem::is_regular_file(root)) {
    return std::string(root);
  }

  for (auto suffix : {
           "/EBOOT.BIN",
           "/USRDIR/EBOOT.BIN",
           "/USRDIR/ISO.BIN.EDAT",
           "/PS3_GAME/USRDIR/EBOOT.BIN",
       }) {
    auto tryPath = std::string(root);
    tryPath += suffix;

    if (std::filesystem::is_regular_file(tryPath)) {
      return tryPath;
    }
  }

  return {};
}

static std::string locateParamSfoPath(std::string_view root) {
  if (std::filesystem::is_regular_file(root)) {
    return std::string(root);
  }

  for (auto suffix : {
           "/PARAM.SFO",
           "/PS3_GAME/PARAM.SFO",
       }) {
    auto tryPath = std::string(root);
    tryPath += suffix;

    if (std::filesystem::is_regular_file(tryPath)) {
      return tryPath;
    }
  }

  return {};
}

static std::optional<GameInfo>
fetchGameInfo(const psf::registry &psf,
              std::filesystem::path psfRootPath = {}) {
  auto titleId = std::string(psf::get_string(psf, "TITLE_ID"));
  auto name = std::string(psf::get_string(psf, "TITLE"));
  auto bootable = psf::get_integer(psf, "BOOTABLE", 0);
  auto category = psf::get_string(psf, "CATEGORY");
  // Game version for display: APP_VER (patched version) over disc VERSION
  auto version = std::string(
      psf::get_string(psf, "APP_VER", psf::get_string(psf, "VERSION", "")));

  if (!bootable || titleId.empty()) {
    return {};
  }

  bool isDiscGame = category == "DG";

  std::string path;

  if (!isDiscGame) {
    path = rpcs3::utils::get_hdd0_dir() + "game/" + titleId + "/";
  } else {
    if (psfRootPath.empty()) {
      path = fs::get_config_dir() + "games/" + titleId + "/";
    } else {
      // Locate game root path
      if (psfRootPath.filename() == "USRDIR") {
        psfRootPath = psfRootPath.parent_path();
      }

      if (psfRootPath.filename() == "PS3_GAME") {
        psfRootPath = psfRootPath.parent_path();
      }

      path = psfRootPath;
      if (!path.ends_with('/')) {
        path += '/';
      }
    }
  }

  auto dataPath = isDiscGame ? path + "PS3_GAME/" : path;
  auto iconPath = dataPath + "ICON0.PNG";
  auto moviePath = dataPath + "ICON1.PAM";

  int flags = 0;

  if (!isDiscGame) {
    auto ebootPath = locateEbootPath(path);

    bool isLocked = false;

    if (!ebootPath.empty()) {
      if (fs::file eboot{ebootPath};
          eboot && eboot.size() >= 4 && eboot.read<u32>() == "SCE\0"_u32) {
        isLocked = !decrypt_self(eboot);
      }
    }

    if (isLocked) {
      flags |= kGameFlagLocked;
      rpcsx_android.warning("game %s is locked", path);
    }

    auto c00Path = path + "/C00";

    bool isTrial = std::filesystem::is_directory(c00Path);

    if (isTrial) {
      if (!tryUnlockGame(psf)) {
        flags |= kGameFlagTrial;
        rpcsx_android.warning("game %s is trial", path);
      } else {
        auto c00IconPath = c00Path + "/ICON0.PNG";
        if (std::filesystem::is_regular_file(c00IconPath)) {
          iconPath = c00IconPath;
        }

        auto c00SfoPath = c00Path + "/PARAM.SFO";

        if (std::filesystem::is_regular_file(c00IconPath)) {
          auto c00Sfo = psf::load_object(c00SfoPath);
          titleId = psf::get_string(c00Sfo, "TITLE_ID", titleId);
          name = psf::get_string(c00Sfo, "TITLE", name);
        }
      }
    }
  }

  // Prefer an installed game update's version (dev_hdd0/game/<id>/PARAM.SFO)
  // over the disc/base version - that's the effective version the game runs at
  // once an update is installed. For HDD games this is the same file (no-op).
  if (!titleId.empty()) {
    const auto updateSfo =
        rpcs3::utils::get_hdd0_dir() + "game/" + titleId + "/PARAM.SFO";
    if (fs::is_file(updateSfo)) {
      auto upd = psf::load_object(updateSfo);
      auto updVer = std::string(psf::get_string(upd, "APP_VER", ""));
      if (!updVer.empty()) {
        version = std::move(updVer);
      }
    }
  }

  return GameInfo{
      .path = std::move(path),
      .name = std::move(name),
      .iconPath = std::move(iconPath),
      .flags = flags,
      .version = std::move(version),
      .titleId = std::move(titleId),
  };
}

static void collectGameInfo(JNIEnv *env, jlong progressId,
                            const std::vector<std::string> &rootDirs) {
  std::vector<std::string> paths;
  for (auto &&rootDir : rootDirs) {
    collectGamePaths(paths, rootDir);

    rpcsx_android.notice("collectGameInfo: processed %s", rootDir);
  }

  rpcsx_android.notice("collectGameInfo: found %d paths", paths.size());

  Progress progress(env, progressId);
  progress.report(0, paths.size());

  std::vector<GameInfo> gameInfos;
  gameInfos.reserve(10);
  std::size_t processed = 0;

  auto submit = [&] {
    if (gameInfos.empty()) {
      return;
    }

    sendGameInfo(env, progressId, gameInfos);
    progress.report(processed, paths.size());
    gameInfos.clear();
  };

  for (auto &&path : paths) {
    processed++;

    // Direct-play ISO entry: metadata comes from inside the archive, served
    // through the iso_cache so repeated list scans never re-walk the ISO tree
    // (prohibitive on FUSE/SAF storage).
    if (!std::filesystem::is_directory(path) && hasIsoExtension(path)) {
      if (!is_iso_file(path)) {
        rpcsx_android.warning("collectGameInfo: '%s' is not a readable ISO "
                              "(encrypted without key, or not ISO9660)",
                              path);
        continue;
      }

      iso_metadata_cache_entry entry;

      if (!iso_cache::load(path, path, entry)) {
        iso_archive archive(path);

        if (!archive) {
          continue;
        }

        const psf::registry iso_psf = archive.open_psf("PS3_GAME/PARAM.SFO");

        if (iso_psf.empty()) {
          rpcsx_android.warning("collectGameInfo: no PS3_GAME/PARAM.SFO in '%s'", path);
          continue;
        }

        entry.psf_data = psf::save_object(iso_psf);
        entry.icon_path = "PS3_GAME/ICON0.PNG";

        if (auto icon = archive.open(entry.icon_path)) {
          std::vector<u8> data(icon->size());
          if (icon->read(data.data(), data.size()) == data.size()) {
            entry.icon_data = std::move(data);
          }
        }

        if (fs::stat_t st{}; fs::get_stat(path, st)) {
          entry.mtime = st.mtime;
        }

        iso_cache::save(path, path, entry);
      }

      const psf::registry psf =
          psf::load_object(fs::make_stream(std::move(entry.psf_data)), path);

      rpcsx_android.notice("collectGameInfo: iso sfo from %s", path);

      if (auto gameInfo = fetchGameInfo(psf, path)) {
        // Boot target and icon live at host paths for ISO entries
        gameInfo->path = path;
        gameInfo->iconPath =
            entry.icon_data.empty() ? std::string()
                                    : iso_cache::get_icon_file_path(path);

        gameInfos.push_back(std::move(*gameInfo));

        if (gameInfos.size() >= 10) {
          submit();
        }
      }

      continue;
    }

    if (!std::filesystem::is_regular_file(path + "/PARAM.SFO")) {
      continue;
    }

    const auto psf = psf::load_object(path + "/PARAM.SFO");

    rpcsx_android.notice("collectGameInfo: sfo at %s", path);

    if (auto gameInfo = fetchGameInfo(psf, path)) {
      gameInfos.push_back(std::move(*gameInfo));

      if (gameInfos.size() >= 10) {
        submit();
      }
    }
  }

  submit();

  progress.success(processed);
}

class MainThreadProcessor {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::pair<std::function<void(JNIEnv *)>, atomic_t<u32> *>> queue;
  std::atomic<std::thread::id> processorThreadId{};

public:
  // True when called from the thread that runs process() (the dedicated main
  // thread). Used to invoke a queued callback inline instead of deadlocking on a
  // re-entrant/blocking dispatch back to ourselves.
  bool onProcessorThread() const {
    return processorThreadId.load() == std::this_thread::get_id();
  }

  void push(std::function<void(JNIEnv *)> cb, atomic_t<u32> *wakeUp = nullptr) {
    std::lock_guard lock(mutex);
    queue.push_back({std::move(cb), wakeUp});
    cv.notify_one();
  }

  void push(std::function<void()> cb, atomic_t<u32> *wakeUp = nullptr) {
    push([cb = std::move(cb)](JNIEnv *) { cb(); }, wakeUp);
  }

  void process(JNIEnv *env) {
    processorThreadId = std::this_thread::get_id();
    while (true) {
      std::function<void(JNIEnv *)> cb;
      atomic_t<u32> *wakeUp = nullptr;

      {
        std::unique_lock lock(mutex);
        if (queue.empty()) {
          cv.wait(lock);
          continue;
        }

        auto item = std::move(queue.front());
        queue.pop_front();

        cb = std::move(item.first);
        wakeUp = item.second;
      }

      cb(env);
      if (wakeUp) {
        *wakeUp = true;
        wakeUp->notify_all();
      }
    }
  }
} static g_mainThreadProcessor;

static void invokeAsync(std::function<void(JNIEnv *)> cb) {
  g_mainThreadProcessor.push(std::move(cb));
}

static void invokeSync(std::function<void(JNIEnv *)> cb) {
  atomic_t<u32> wakeup{false};
  g_mainThreadProcessor.push(std::move(cb), &wakeup);

  while (wakeup.load() == false) {
    wakeup.wait(false);
  }
}

struct ProgressMessageDialog : MsgDialogBase {
  jlong progressId;
  jlong value = 0;
  jlong max = 0;

  ProgressMessageDialog(jlong progressId) : progressId(progressId) {}

  void Create(const std::string &msg, const std::string &title) override {
    rpcsx_android.warning("ProgressMessageDialog::Create(%s, %s)", msg, title);
    max = 100;
    invokeSync([this, &msg](JNIEnv *env) {
      Progress progress(env, progressId);
      // Determinate from the start (max != 0) so the long compile shows a real
      // percentage instead of an indeterminate spinner. The progress server
      // then drives 0..100 via ProgressBarSetValue.
      progress.report(0, max, msg);
    });
  }

  jlong getValue() const {
    return value == max && max != 0 ? value - 1 : value;
  }

  void Close(bool success) override {
    rpcsx_android.warning("ProgressMessageDialog::Close(%s)", success);
    invokeSync([this](JNIEnv *env) {
      Progress progress(env, progressId);
      // Report complete instead of resetting to an indeterminate (0,0).
      progress.report(max, max);
    });

    //   Progress progress(env, progressId);
    //   if (success) {
    //     progress.success(0);
    //   } else {
    //     progress.failure();
    //   }
    // });
  }

  void SetMsg(const std::string &msg) override {
    rpcsx_android.warning("ProgressMessageDialog::SetMsg(%s)", msg);
    invokeSync([this, msg](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max, msg);
    });
  }

  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarSetMsg(%d, %s)",
                          progressBarIndex, msg);
    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    invokeSync([this, msg](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max, msg);
    });
  }

  void ProgressBarReset(u32 progressBarIndex) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarReset(%d)",
                          progressBarIndex);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    value = 0;
    invokeSync(
        [this](JNIEnv *env) { Progress(env, progressId).report(value, max); });
  }

  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarInc(%d, %d)",
                          progressBarIndex, delta);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    value += delta;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }

  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarSetValue(%d, %d)",
                          progressBarIndex, value);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    this->value = value;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }
  void ProgressBarSetLimit(u32 index, u32 limit) override {
    rpcsx_android.warning("ProgressMessageDialog::ProgressBarSetLimit(%d, %d)",
                          index, limit);

    if (index != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    max = limit;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }
};

struct UiMessageDialog : MsgDialogBase {
  // FIXME: implement

  void Create(const std::string &msg, const std::string &title) override {}
  void Close(bool success) override {}
  void SetMsg(const std::string &msg) override {}
  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {}
  void ProgressBarReset(u32 progressBarIndex) override {}
  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {}
  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {}
  void ProgressBarSetLimit(u32 index, u32 limit) override {}
};

struct MessageDialog : MsgDialogBase {
  std::unique_ptr<MsgDialogBase> impl = nullptr;

  void Create(const std::string &msg, const std::string &title) override {
    auto progressId = s_pendingProgressId.load();

    rpcsx_android.warning("MessageDialog::Create(%s, %s): source %s, id %d",
                          msg, title, source, progressId);

    if (progressId != -1) {
      impl = std::make_unique<ProgressMessageDialog>(progressId);
    } else {
      impl = std::make_unique<UiMessageDialog>();
    }

    impl->type = type;
    impl->source = source;
    impl->Create(msg, title);
  }

  void Close(bool success) override { impl->Close(success); }

  void SetMsg(const std::string &msg) override { impl->SetMsg(msg); }

  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {
    impl->ProgressBarSetMsg(progressBarIndex, msg);
  }

  void ProgressBarReset(u32 progressBarIndex) override {
    impl->ProgressBarReset(progressBarIndex);
  }

  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {
    impl->ProgressBarInc(progressBarIndex, delta);
  }

  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {
    impl->ProgressBarSetValue(progressBarIndex, value);
  }

  void ProgressBarSetLimit(u32 index, u32 limit) override {
    impl->ProgressBarSetLimit(index, limit);
  }

  static void pushPendingProgressId(jlong id) {
    jlong value = -1;

    while (!s_pendingProgressId.compare_exchange_weak(value, id)) {
      s_pendingProgressId.wait(value);
      value = -1;
    }
  }

  static bool popPendingProgressId(jlong id) {
    return s_pendingProgressId.compare_exchange_strong(id, -1);
  }

private:
  static std::atomic<jlong> s_pendingProgressId;
};

struct OverlaySaveDialog : SaveDialogBase {
  s32 ShowSaveDataList(const std::string &base_dir,
                       std::vector<SaveDataEntry> &save_entries, s32 focused,
                       u32 op, vm::ptr<CellSaveDataListSet> listSet,
                       bool enable_overlay) override {
    rpcsx_android.notice("ShowSaveDataList(save_entries=%d, focused=%d, "
                         "op=0x%x, listSet=*0x%x, enable_overlay=%d)",
                         save_entries.size(), focused, op, listSet,
                         enable_overlay);

    bool use_end = sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_BEGIN, 0) >= 0;

    auto atExit = AtExit([&] {
      if (use_end) {
        sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_END, 0);
      }
    });

    if (!use_end) {
      rpcsx_android.error(
          "ShowSaveDataList(): Not able to notify DRAWING_BEGIN callback "
          "because one has already been sent!");
    }

    if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>()) {
      rpcsx_android.notice("ShowSaveDataList: Showing native UI dialog");

      s32 result = manager->create<rsx::overlays::save_dialog>()->show(
          base_dir, save_entries, focused, op, listSet, enable_overlay);

      if (result != rsx::overlays::user_interface::selection_code::error) {
        rpcsx_android.notice(
            "ShowSaveDataList: Native UI dialog returned with selection %d",
            result);

        return result;
      }

      rpcsx_android.error("ShowSaveDataList: Native UI dialog returned error");
    }

    return -2;
  }
};

class OverlayTrophyNotification : public TrophyNotificationBase {
public:
  s32 ShowTrophyNotification(
      const SceNpTrophyDetails &trophy,
      const std::vector<uchar> &trophy_icon_buffer) override {
    if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>()) {
      auto popup = std::make_shared<rsx::overlays::trophy_notification>();
      return manager->add(popup, false)->show(trophy, trophy_icon_buffer);
    }

    return 0;
  }
};

std::atomic<jlong> MessageDialog::s_pendingProgressId = -1;

struct CompilationWorkload {
  jlong progressId;
  std::string path;
};

extern bool ppu_load_exec(const ppu_exec_object &, bool virtual_load,
                          const std::string &, utils::serial * = nullptr);
extern void spu_load_exec(const spu_exec_object &);
extern void spu_load_rel_exec(const spu_rel_object &);
extern void ppu_precompile(std::vector<std::string> &dir_queue,
                           std::vector<ppu_module<lv2_obj> *> *loaded_prx);
extern bool ppu_initialize(const ppu_module<lv2_obj> &, bool check_only = false,
                           u64 file_size = 0);
extern void ppu_finalize(const ppu_module<lv2_obj> &);
extern bool ppu_load_rel_exec(const ppu_rel_object &);

class CompilationQueue {
  std::atomic<std::uint64_t> nextWorkTag{0};
  std::uint64_t lastProcessedTag = 0;
  std::mutex queueMutex;
  std::deque<CompilationWorkload> queue;

public:
  void push(CompilationWorkload workload) {
    {
      std::lock_guard lock(queueMutex);
      queue.push_back(std::move(workload));
    }

    nextWorkTag.fetch_add(1);
  }

  void push(Progress &progress, std::string path) {
    // Keep the bar determinate (0%, queued) until the compile dialog drives it,
    // rather than flipping it to an indeterminate spinner while it waits.
    progress.report(0, 100);

    push({
        .progressId = progress.getProgressId(),
        .path = std::move(path),
    });
  }

  void process(JNIEnv *env) {
    while (true) {
      auto nextWorkTagValue = nextWorkTag.load();

      if (nextWorkTagValue == lastProcessedTag) {
        nextWorkTag.wait(lastProcessedTag);
      }

      if (nextWorkTagValue == lastProcessedTag || queue.empty()) {
        continue;
      }

      CompilationWorkload workload;

      {
        std::lock_guard lock(queueMutex);

        if (queue.empty()) {
          continue;
        }

        workload = std::move(queue.front());
        queue.pop_front();
      }

      impl(env, std::move(workload));
      lastProcessedTag++;
    }
  }

private:
  void impl(JNIEnv *env, CompilationWorkload workload) {
    if (workload.path.empty()) {
      Progress(env, workload.progressId).success(0);
      return;
    }

    rpcsx_android.error("Creating cache initiated, state %d",
                        (int)Emu.GetStatus(false));

    while (true) {
      auto state = Emu.GetStatus(false);

      if (state == system_state::stopped || state == system_state::ready) {
        break;
      }

      rpcsx_android.error("Creating cache wait, state %d", (int)state);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    bool is_vsh = workload.path.ends_with("/vsh.self");

    // SetTestMode (not SetState(running)): this is an install-time precompile, NOT a real
    // game. It forces m_state=running for the PPU/memory managers, but also raises
    // IsTestMode() so the persistent RPCN thread stays off g_fxo while the g_fxo reset()
    // and init<>() below tear down / rebuild the p2p_context. SetState(stopped) at the end
    // of this function clears test mode again. (Fixes the install-finished native crash:
    // RPCN thread locking a torn-down shared_mutex - mutex.cpp:89 imp_lock underflow.)
    Emu.SetTestMode();

    MessageDialog::pushPendingProgressId(workload.progressId);

    // If a previous game boot was killed, Emu.Kill() -> g_fxo->clear() leaves the
    // fixed-object map torn down (m_order/m_info nulled). The single-type init<>()
    // calls below do `*m_order++ = obj`, which segfaults on a null m_order
    // (write to 0x0). reset() reallocates the bookkeeping arrays so the precompile
    // gets a clean fxo - this mirrors upstream's precompile path (Emulator::Load),
    // where Emu.Init()/reset() always runs before these inits. Guarded so we don't
    // tear down an fxo that is already live (e.g. a game sitting in the ready state).
    if (!g_fxo->is_init()) {
      g_fxo->reset();
    }

    g_fxo->init<named_thread<progress_dialog_server>>();
    g_fxo->init<main_ppu_module<lv2_obj>>();
    g_fxo->init(false, nullptr);
    auto rootPath = std::filesystem::path(workload.path);

    if (is_vsh) {
      rootPath = g_cfg_vfs.get_dev_flash() + "sys/external/";
    } else {
      if (!std::filesystem::is_directory(rootPath)) {
        rootPath = rootPath.parent_path();
        if (rootPath.filename() == "USRDIR") {
          rootPath = rootPath.parent_path();
        }
      }
    }

    auto &_main = *ensure(g_fxo->try_get<main_ppu_module<lv2_obj>>());

    if (fs::is_file(workload.path)) {
      if (!is_vsh) {
        auto sfoPath = locateParamSfoPath(std::string(rootPath));

        if (!sfoPath.empty()) {
          const auto psf = psf::load_object(sfoPath);
          rpcsx_android.warning("title id is %s",
                                psf::get_string(psf, "TITLE_ID"));

          Emu.SetTitleID(std::string(psf::get_string(psf, "TITLE_ID")));
        } else {
          rpcsx_android.warning("param.sfo not found");
        }
      }

      // Compile binary first
      rpcsx_android.notice("Trying to load binary: %s", workload.path);

      fs::file src{workload.path};
      src = decrypt_self(src);

      const ppu_exec_object obj = src;

      if (obj == elf_error::ok && ppu_load_exec(obj, true, workload.path)) {
        _main.path = workload.path;
      } else {
        rpcsx_android.error("Failed to load binary '%s' (%s)", workload.path,
                            obj.get_error());
      }
    }

    std::vector<std::string> dir_queue;
    dir_queue.push_back(rootPath.string());

    for (auto &entry :
         std::filesystem::recursive_directory_iterator(rootPath)) {
      if (entry.is_directory()) {
        dir_queue.push_back(entry.path().string());
      }
    }

    // Honor the "LLVM Precompilation" toggle for install-time precompile too
    // (it already gates boot-time precompile). With it off, installing - incl.
    // batch folder installs - skips the long up-front compile; code is then
    // compiled lazily on first boot instead. Nothing is lost, only deferred.
    if (g_cfg.core.llvm_precompilation) {
      std::vector<ppu_module<lv2_obj> *> mod_list;
      rpcsx_android.error("Going to analyze executable");

      // FIXME: split states
      if (!is_vsh) {
        if (_main.analyse(0, _main.elf_entry, _main.seg0_code_end,
                          _main.applied_patches, std::vector<u32>{})) {
          Emu.ConfigurePPUCache();
          Emu.SetTestMode();
          rpcsx_android.error("Going to precompile main PPU module");
          ppu_initialize(_main);
          mod_list.emplace_back(&_main);
        }
      }

      ppu_precompile(dir_queue, mod_list.empty() ? nullptr : &mod_list);
    } else {
      rpcsx_android.error("Skipping install-time precompile (LLVM Precompilation disabled)");
    }

    rpcsx_android.error("Finalization");
    g_fxo->reset();
    Emu.SetState(system_state::stopped);

    MessageDialog::popPendingProgressId(workload.progressId);

    Progress(env, workload.progressId).success(0);
  }
} static g_compilationQueue;

static void setupCallbacks() {
  Emu.SetCallbacks({
      .call_from_main_thread =
          [](std::function<void()> cb, atomic_t<u32> *wake_up) {
            // Run deferred Emu callbacks on the dedicated main-thread processor,
            // never inline on the calling (often emulation/worker) thread. The
            // Emu stop "join thread" hands its final teardown to
            // CallFromMainThread, and that teardown drops the last reference to
            // the join thread itself; running it inline made the thread join
            // itself in its named_thread destructor and deadlock - this was the
            // shutdown hang (log: "Thread [Emulation Join Thread] is too sleepy"
            // emitted by the Emulation Join Thread). Dispatching to the processor
            // makes that destructor run on a different thread, joining the now
            // finished join thread cleanly. If we are already on the processor
            // thread, run inline to avoid a self-deadlock on re-entrant blocking
            // calls (matches the desktop "already on main thread" fast path).
            if (g_mainThreadProcessor.onProcessorThread()) {
              cb();
              if (wake_up) {
                *wake_up = true;
                wake_up->notify_all();
              }
            } else {
              g_mainThreadProcessor.push(std::move(cb), wake_up);
            }
          },
      .on_run = [](auto...) {},
      .on_pause = [](auto...) {},
      .on_resume = [](auto...) {},
      .on_stop = [](auto...) {},
      .on_ready = [](auto...) {},
      .on_missing_fw = [](auto...) {},
      .on_emulation_stop_no_response = [](auto...) {},
      .on_save_state_progress = [](auto...) {},
      .enable_disc_eject = [](auto...) {},
      .enable_disc_insert = [](auto...) {},
      .handle_taskbar_progress = [](auto...) {},
      .init_kb_handler =
          [](auto...) {
            ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(
                Emu.DeserialManager()));
          },
      .init_mouse_handler =
          [](auto...) {
            ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(
                Emu.DeserialManager()));
          },
      .init_pad_handler =
          [](auto...) {
            ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, ""));
          },
      .update_emu_settings = [](auto...) {},
      .save_emu_settings =
          [](auto...) {
            Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
          },
      .close_gs_frame = [](auto...) {},
      .get_gs_frame = [] { return std::make_unique<GraphicsFrame>(); },
      .get_camera_handler =
          [](auto...) { return std::make_shared<null_camera_handler>(); },
      .get_music_handler =
          [](auto...) { return std::make_shared<null_music_handler>(); },
      .create_pad_handler = [](pad_handler type, void *thread, void *window)
          -> std::shared_ptr<PadHandlerBase> {
        switch (type) {
        case pad_handler::keyboard:
        case pad_handler::null:
          break;
        case pad_handler::ds3:
          return std::make_shared<ds3_pad_handler>();
        case pad_handler::ds4:
          return std::make_shared<ds4_pad_handler>();
        case pad_handler::dualsense:
          return std::make_shared<dualsense_pad_handler>();
        case pad_handler::skateboard:
          //   return std::make_shared<skateboard_pad_handler>();
          break;
        case pad_handler::move:
          // return std::make_shared<ps_move_handler>();
          break;
#ifdef _WIN32
        case pad_handler::xinput:
          return std::make_shared<xinput_pad_handler>();
        case pad_handler::mm:
          return std::make_shared<mm_joystick_handler>();
#endif
#ifdef HAVE_SDL3
        case pad_handler::sdl:
          return std::make_shared<sdl_pad_handler>();
#endif
#ifdef HAVE_LIBEVDEV
        case pad_handler::evdev:
          return std::make_shared<evdev_joystick_handler>();
#endif
        case pad_handler::virtual_pad:
          return std::make_shared<virtual_pad_handler>();
        }

        return std::make_shared<NullPadHandler>();
      },
      .init_gs_render =
          [](utils::serial *ar) {
            switch (g_cfg.video.renderer.get()) {
            case video_renderer::null:
              g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
              break;
            case video_renderer::vulkan:
              g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
              break;

            default:
              break;
            }
          },
      .get_audio =
          [](auto...) {
            std::shared_ptr<AudioBackend> result;

            switch (g_cfg.audio.renderer.get()) {
            case audio_renderer::null:
              result = std::make_shared<NullAudioBackend>();
              break;

            case audio_renderer::cubeb:
            default:
              result = std::make_shared<CubebBackend>();
              break;
            }

            if (!result->Initialized()) {
              rpcsx_android.error(
                  "Audio renderer %s could not be initialized, using a Null "
                  "renderer instead. Make sure that no other application is "
                  "running that might block audio access (e.g. Netflix).",
                  result->GetName());
              result = std::make_shared<NullAudioBackend>();
            }
            return result;
          },
      .get_audio_enumerator = [](auto...) { return nullptr; },
      .get_msg_dialog = [] { return std::make_shared<MessageDialog>(); },
      .get_osk_dialog = [](auto...) { return nullptr; },
      .get_save_dialog =
          [](auto...) { return std::make_unique<OverlaySaveDialog>(); },
      .get_sendmessage_dialog = [](auto...) { return nullptr; },
      .get_recvmessage_dialog = [](auto...) { return nullptr; },
      .get_trophy_notification_dialog =
          [](auto...) { return std::make_unique<OverlayTrophyNotification>(); },
      .get_localized_string = [](localized_string_id id,
                                 const char *) -> std::string {
        if (std::size_t(id) < std::size(g_strings)) {
          return g_strings[int(id)].first;
        }
        return "";
      },
      .get_localized_u32string = [](localized_string_id id,
                                    const char *) -> std::u32string {
        if (std::size_t(id) < std::size(g_strings)) {
          return g_strings[int(id)].second;
        }
        return U"";
      },
      .get_localized_setting =
          [](const cfg::_base *node, u32 enum_index) -> std::string {
        // The home-menu dropdowns build their option list by calling this once
        // per enum index. Returning "" (the old stub) left every dropdown blank.
        // We have no translation table on Android, so surface the config enum's
        // own token (e.g. "Vulkan", "Mega", "Automatic") via to_list(), which is
        // already human-readable. Falls back to "" only for out-of-range/non-enum.
        if (!node) {
          return "";
        }
        const std::vector<std::string> list = node->to_list();
        if (enum_index < list.size()) {
          return list[enum_index];
        }
        return "";
      },
      .play_sound = [](auto...) {},
      .get_image_info = [](auto...) { return false; },
      .get_scaled_image = [](auto...) { return false; },
      .resolve_path =
          [](std::string_view arg) {
            std::error_code ec;
            auto result =
                std::filesystem::weakly_canonical(
                    std::filesystem::path(fmt::replace_all(arg, "\\", "/")), ec)
                    .string();
            return ec ? std::string(arg) : result;
          },
      .get_font_dirs = [](auto...) { return std::vector<std::string>(); },
      .on_install_pkgs =
          [](const std::vector<std::string> &pkgs) {
            for (const std::string &pkg : pkgs) {
              if (!rpcs3::utils::install_pkg(pkg)) {
                rpcsx_android.error("cd install pkgs: failed to install %s",
                                    pkg);
                return false;
              }
            }
            return true;
          },
      .add_breakpoint = [](auto...) {},
      .display_sleep_control_supported = [](auto...) { return false; },
      .enable_display_sleep = [](auto...) {},
      .check_microphone_permissions = [](auto...) {},
  });
}

static bool initVirtualPad(const std::shared_ptr<Pad> &pad) {
  u32 pclass_profile = 0;
  pad->Init(CELL_PAD_STATUS_CONNECTED,
            CELL_PAD_CAPABILITY_PS3_CONFORMITY |
                CELL_PAD_CAPABILITY_PRESS_MODE |
                CELL_PAD_CAPABILITY_HP_ANALOG_STICK |
                CELL_PAD_CAPABILITY_ACTUATOR //| CELL_PAD_CAPABILITY_SENSOR_MODE
            ,
            CELL_PAD_DEV_TYPE_STANDARD, CELL_PAD_PCLASS_TYPE_STANDARD,
            pclass_profile, 0, 0, 50);

  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_UP);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_DOWN);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_LEFT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_RIGHT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_CROSS);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_SQUARE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_CIRCLE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_TRIANGLE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_L1);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_L2);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_L3);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_R1);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_R2);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_R3);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_START);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_SELECT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_PS);

  pad->m_sticks[0] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, {}, {});
  pad->m_sticks[1] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, {}, {});
  pad->m_sticks[2] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, {}, {});
  pad->m_sticks[3] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, {}, {});

  pad->m_sensors[0] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_X, 0, 0, 0, DEFAULT_MOTION_X);
  pad->m_sensors[1] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Y, 0, 0, 0, DEFAULT_MOTION_Y);
  pad->m_sensors[2] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Z, 0, 0, 0, DEFAULT_MOTION_Z);
  pad->m_sensors[3] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_G, 0, 0, 0, DEFAULT_MOTION_G);

  pad->m_vibrateMotors[0] = VibrateMotor(true, 0);
  pad->m_vibrateMotors[1] = VibrateMotor(false, 0);

  if (pad->m_player_id == 0) {
    std::lock_guard lock(g_virtual_pad_mutex);
    g_virtual_pad = pad;
  }
  return true;
}

extern "C" bool _rpcsx_overlayPadData(int digital1, int digital2,
                                      int leftStickX, int leftStickY,
                                      int rightStickX, int rightStickY) {

  auto pad = [] {
    std::shared_ptr<Pad> result;
    std::lock_guard lock(g_virtual_pad_mutex);
    result = g_virtual_pad;
    return result;
  }();

  if (pad == nullptr) {
    return false;
  }

  for (auto &btn : pad->m_buttons) {
    if (btn.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1) {
      btn.m_pressed = (digital1 & btn.m_outKeyCode) != 0;

      if (btn.m_outKeyCode == CELL_PAD_CTRL_PS && btn.m_pressed) {
        if (auto padThread = pad::get_pad_thread(true)) {
          padThread->open_home_menu();
        }
      }

    } else if (btn.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2) {
      btn.m_pressed = (digital2 & btn.m_outKeyCode) != 0;
    }

    btn.m_value = btn.m_pressed ? 255 : 0;
  }

  pad->m_sticks[0].m_value = leftStickX;
  pad->m_sticks[1].m_value = leftStickY;
  pad->m_sticks[2].m_value = rightStickX;
  pad->m_sticks[3].m_value = rightStickY;
  return true;
}

// Optional, additive entry point: hand the core the app-private (internal)
// storage dir so secrets like rpcn.yml can live off the MTP/USB-visible and
// cloud-backed-up external storage. Kept SEPARATE from _rpcsx_initialize so the
// app<->core ABI of the critical init path never changes: an old app simply
// never calls this (the core falls back to external), and an old core simply
// lacks the symbol (the app's dlsym yields null and skips the call). Either way
// nothing crashes on version skew.
extern "C" void _rpcsx_setRpcnConfigDir(std::string_view internalDir) {
  if (internalDir.empty()) {
    return;
  }

  auto internalDirStr = fix_dir_path(std::string(internalDir));
  g_android_internal_config_dir = internalDirStr + "config/";
  std::filesystem::create_directories(g_android_internal_config_dir);
}

extern "C" bool _rpcsx_initialize(std::string_view rootDir,
                                  std::string_view user) {
  auto rootDirStr = fix_dir_path(std::string(rootDir));

  if (g_android_executable_dir != rootDirStr) {
    g_android_executable_dir = rootDirStr;
    g_android_config_dir = rootDirStr + "config/";
    g_android_cache_dir = rootDirStr + "cache/";

    std::filesystem::create_directories(g_android_config_dir);
    std::error_code ec;
    // std::filesystem::remove_all(g_android_cache_dir, ec);
    std::filesystem::create_directories(g_android_cache_dir);
  }

  if (g_initialized) {
    return true;
  }

  g_initialized = true;

#if defined(ARCH_ARM64)
  // Calibrate busy_wait() to this device's hardware timer frequency before any
  // emulation spins. Without this, busy waits sized in x86-equivalent cycles
  // ran ~100x too long on phone timers (~19MHz vs the assumed ~3GHz).
  rx::init_arm_timer_scale();
#endif

  if (int r = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY,
                                nullptr);
      r != 0) {
    rpcsx_android.warning(
        "libusb_set_option(LIBUSB_OPTION_NO_DEVICE_DISCOVERY) -> %d", r);
  }

  // Initialize thread pool finalizer // ???
  static_cast<void>(named_thread("", [](int) {}));

  static std::unique_ptr<logs::listener> log_file;
  {
    // Check free space
    fs::device_stat stats{};
    if (!fs::statfs(fs::get_cache_dir(), stats) ||
        stats.avail_free < 128 * 1024 * 1024) {
      std::fprintf(stderr, "Not enough free space for logs (%f KB)",
                   stats.avail_free / 1000000.);
    }

    // preserve old log file
    if (std::filesystem::exists(fs::get_log_dir() + "RPCSX.log")) {
      std::error_code ec;
      std::filesystem::remove(fs::get_log_dir() + "RPCSX.old.log", ec);
      std::filesystem::rename(fs::get_log_dir() + "RPCSX.log",
                              fs::get_log_dir() + "RPCSX.old.log", ec);
    }

    // Limit log size to ~25% of free space
    log_file = logs::make_file_listener(fs::get_log_dir() + "RPCSX.log",
                                        stats.avail_free / 4);
  }

  // Capture native stderr (fd 2) into a side file. LLVM's unhandled-error
  // paths write their last words to fd 2 before abort() (e.g. "LLVM ERROR:
  // out of memory" from report_bad_alloc_error) - an Android app otherwise
  // discards fd 2 entirely, which made the Demon's Souls compile-OOM death
  // completely silent. Kept separate from RPCSX.log so raw writes cannot
  // interleave with the buffered file listener. O_APPEND keeps each write
  // atomic. Normally this file stays empty.
  {
    std::error_code ec;
    std::filesystem::remove(fs::get_log_dir() + "RPCSX.stderr.old.log", ec);
    std::filesystem::rename(fs::get_log_dir() + "RPCSX.stderr.log",
                            fs::get_log_dir() + "RPCSX.stderr.old.log", ec);

    if (const int fd =
            ::open((fs::get_log_dir() + "RPCSX.stderr.log").c_str(),
                   O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        fd >= 0) {
      ::dup2(fd, 2);
      if (fd != 2) {
        ::close(fd);
      }
    }
  }

  // Release logging budget: a handful of high-volume HLE / recompiler channels emit
  // thousands of per-call / per-block trace lines (SPU block dumps + loop analysis,
  // sys_* syscall traces, module export/import dumps, unavailable-perf-counter spam)
  // that bloat the on-device log enormously with no end-user value - one session was
  // ~29 MB / 194k lines. Raise those channels so only genuine warnings/errors survive.
  // g_cfg.log is empty by default, so Emu boot's set_channel_levels() is a no-op and
  // will not undo these. RSX is kept at warning to preserve the texture cache-miss
  // perf signal.
  logs::set_level("SPU", logs::level::error);
  logs::set_level("sys_fs", logs::level::error);
  logs::set_level("sys_event", logs::level::error);
  logs::set_level("sys_spu", logs::level::error);
  logs::set_level("sys_process", logs::level::error);
  logs::set_level("sys_net", logs::level::error);
  logs::set_level("ppu_loader", logs::level::warning);
  logs::set_level("PERF", logs::level::warning);
  logs::set_level("RSX", logs::level::warning);

  logs::stored_message ver{rpcsx_android.always()};
  // The user-facing name is the ordered date-time (see Version::toString); keep
  // the exact git revision here in the log so a report still pins the commit.
  const auto rxVer = rx::getVersion();
  const auto rxRev = rxVer.gitRev();
  ver.text = rxRev.empty()
                 ? fmt::format("RPCSX-ps3-android v%s", rxVer.toString())
                 : fmt::format("RPCSX-ps3-android v%s (%s)", rxVer.toString(), rxRev);

  // Write System information
  logs::stored_message sys{rpcsx_android.always()};
  sys.text = utils::get_system_info();

  // Write OS version
  logs::stored_message os{rpcsx_android.always()};
  os.text = utils::get_OS_version_string();

  // Write current time
  logs::stored_message time{rpcsx_android.always()};
  time.text = fmt::format("Current Time: %s", std::chrono::system_clock::now());

  logs::set_init(
      {std::move(ver), std::move(sys), std::move(os), std::move(time)});

  auto set_rlim = [](int resource, std::uint64_t limit) {
    rlimit64 rlim{};
    if (getrlimit64(resource, &rlim) != 0) {
      rpcsx_android.error("failed to get rlimit for %d", resource);
      return;
    }

    // Try to raise the hard limit too, not just the soft limit. Android's
    // default RLIMIT_MEMLOCK hard cap is small, so only bumping the soft limit
    // up to it (the previous behaviour) still left vm::lock_sudo failing and the
    // RSX/main/stack guest memory unpinned ("Failed to lock sudo memory" in the
    // log). Never lower an already-higher hard limit; fall back to the soft-only
    // bump if the kernel refuses to raise the hard limit.
    // (Raising RLIMIT_MEMLOCK to RLIM_INFINITY follows aps3e, by aenu.)
    rlimit64 want{};
    want.rlim_max = std::max<std::uint64_t>(rlim.rlim_max, limit);
    want.rlim_cur = std::min<std::uint64_t>(want.rlim_max, limit);

    if (setrlimit64(resource, &want) == 0) {
      rpcsx_android.notice("rlimit[%d] = %u (max %u)", resource, want.rlim_cur,
                           want.rlim_max);
      return;
    }

    rlim.rlim_cur = std::min<std::size_t>(rlim.rlim_max, limit);
    rpcsx_android.error("rlimit[%d] = %u (requested %u, max %u)", resource,
                        rlim.rlim_cur, limit, rlim.rlim_max);

    if (setrlimit64(resource, &rlim) != 0) {
      rpcsx_android.error("failed to set rlimit for %d", resource);
      return;
    }
  };

  set_rlim(RLIMIT_MEMLOCK, RLIM_INFINITY);
  set_rlim(RLIMIT_NOFILE, RLIM_INFINITY);
  set_rlim(RLIMIT_STACK, 128 * 1024 * 1024);
  set_rlim(RLIMIT_AS, RLIM_INFINITY);

  virtual_pad_handler::set_on_connect_cb(initVirtualPad);
  setupCallbacks();
  Emu.SetHasGui(false);
  Emu.SetUsr(std::string(user));
  Emu.Init();

  g_cfg_input.player1.handler.set(pad_handler::virtual_pad);
  g_cfg_input.player1.device.from_string("Virtual");
  g_cfg_input.save("", g_cfg_input_configs.default_config);

  // Leave the LLVM target CPU empty so the JIT auto-detects the real chip via
  // our MIDR table (jit_compiler::cpu -> aarch64::get_cpu_name), which picks the
  // prime/big core (e.g. cortex-a76) and tunes PPU/SPU codegen for it. The old
  // port hard-coded "cortex-a34" - a tiny in-order ARMv8.0 core - which defeated
  // that detection and scheduled all generated code for the weakest possible
  // microarchitecture. Force-clear it (not just default) so devices that already
  // persisted "cortex-a34" in config.yml get re-detected on next launch.
  g_cfg.core.llvm_cpu.from_string("");

  // Log the resolved target so every report shows which core codegen was tuned
  // for (e.g. "cortex-a720"), instead of the empty auto-detect setting.
  rpcsx_android.notice("LLVM target CPU resolved to: %s",
                       jit_compiler::cpu(g_cfg.core.llvm_cpu));

  Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
  return true;
}

extern "C" bool _rpcsx_processCompilationQueue(JNIEnv *env) {
  g_compilationQueue.process(env);
  return true;
}

extern "C" bool _rpcsx_startMainThreadProcessor(JNIEnv *env) {
  g_mainThreadProcessor.process(env);
  return true;
}

extern "C" bool _rpcsx_collectGameInfo(JNIEnv *env, std::string_view rootDir,
                                       long progressId) {

  if (std::filesystem::is_regular_file(g_cfg_vfs.get_dev_flash() +
                                       "/vsh/module/vsh.self")) {
    sendVshBootable(env, progressId);
  }

  collectGameInfo(env, progressId, {std::string(rootDir)});
  return true;
}

extern "C" void _rpcsx_shutdown() { Emu.Kill(); }

extern "C" int _rpcsx_boot(std::string_view path_) {
  Emu.SetForceBoot(true);
  std::string path = std::string(path_);
  while (path.ends_with('/')) {
    path.pop_back();
  }

  // cfg_mode::custom makes the emulator load a per-game custom config
  // (config/custom_configs/config_<title_id>.yml) when one exists, falling back
  // to the global config otherwise - mirroring desktop RPCS3.
  return static_cast<int>(Emu.BootGame(path, "", false, cfg_mode::custom));
}

// ADD-ONLY ABI (JNI skew rule): boot an ISO from an Android SAF fd without any
// real filesystem path. The app owns the URI permission and must pass a FRESH
// detached fd on every boot (provider fds do not survive provider restarts).
// display_path (the content:// URI) is what games.yml/savestates record.
// Decrypted ISOs only (sector-key lookup is path-based).
extern "C" int _rpcsx_bootIsoFd(int fd, std::string_view display_path) {
  auto file = fs::file::from_native_handle(fd);

  if (!file || !file.size()) {
    return static_cast<int>(game_boot_result::invalid_file_or_folder);
  }

  {
    // Validate before mounting so a bad fd never leaves a half-registered device
    iso_archive probe(fs::file::from_native_handle(::dup(fd)));
    if (!probe) {
      return static_cast<int>(game_boot_result::invalid_file_or_folder);
    }
  }

  load_iso(std::move(file), std::string(display_path));

  std::string path = iso_device::virtual_device_name + "/";

  // Install discs have no USRDIR/EBOOT.BIN - boot the archive root then
  if (fs::is_file(path + "PS3_GAME/USRDIR/EBOOT.BIN")) {
    path += "PS3_GAME/USRDIR/EBOOT.BIN";
  }

  Emu.SetForceBoot(true);
  return static_cast<int>(Emu.BootGame(path, "", false, cfg_mode::custom));
}

// ADD-ONLY ABI: game metadata (title id, name, version) for an ISO given a SAF
// fd, so the app can list content:// ISOs without extraction. Returns a
// "titleId|name|version" packed string or null.
extern "C" jstring _rpcsx_getIsoGameInfoFd(JNIEnv *env, int fd) {
  iso_archive archive(fs::file::from_native_handle(fd));

  if (!archive) {
    return nullptr;
  }

  const psf::registry sfo = archive.open_psf("PS3_GAME/PARAM.SFO");
  const auto title_id = psf::get_string(sfo, "TITLE_ID");

  if (title_id.empty()) {
    return nullptr;
  }

  const auto name = psf::get_string(sfo, "TITLE");
  const auto version = psf::get_string(sfo, "APP_VER", psf::get_string(sfo, "VERSION", ""));

  return wrap(env, fmt::format("%s|%s|%s", title_id, name, version));
}

extern "C" int _rpcsx_getState() {
  return static_cast<int>(Emu.GetStatus(false));
}
extern "C" void _rpcsx_kill() { Emu.Kill(); }
extern "C" void _rpcsx_resume() { Emu.Resume(); }

extern "C" void _rpcsx_openHomeMenu() {
  if (auto padThread = pad::get_pad_thread(true)) {
    padThread->open_home_menu();
  }
}

extern "C" std::string _rpcsx_getTitleId() { return Emu.GetTitleID(); }

extern "C" bool _rpcsx_surfaceEvent(JNIEnv *env, jobject surface, jint event) {
  rpcsx_android.warning("surface event %p, %d", surface, event);

  if (event == 2) {
    auto prevWindow = g_native_window.exchange(nullptr);
    if (prevWindow != nullptr) {
      ANativeWindow_release(prevWindow);
    }

    // surfaceDestroyed runs on the Android UI/main thread. Both open_home_menu() and
    // Emu.Pause() can block for many seconds during a first-boot bulk PPU precompile
    // (the guest main thread is parked waiting for modules to finish compiling). That
    // froze the UI -> ANR -> force close whenever the user backgrounded a still-
    // compiling first boot (RPCSX.old(17).log: surface lost at 0:01:08, the core kept
    // running un-paused to 0:01:22, the UI hung). The native window is already released
    // above - the only step the SurfaceHolder contract requires - so hand the pause and
    // home-menu off to a detached worker and return immediately.
    std::thread([] {
      // relaxed: the surface can be lost while no pad thread exists (e.g. during
      // emulation shutdown or before boot); a non-relaxed get_pad_thread() would
      // ensure()-abort on the null handle and crash the process.
      if (auto padThread = pad::get_pad_thread(true)) {
        padThread->open_home_menu();
      }

      // Only pause if the surface is still gone. A quick destroy->recreate (e.g. a
      // rotation or a transient focus loss) fires event 0 -> Emu.Resume(); that resume
      // must win the race, not this deferred pause.
      if (g_native_window.load() == nullptr) {
        Emu.Pause();
      }
    }).detach();
  } else {
    auto newWindow = ANativeWindow_fromSurface(env, surface);

    if (newWindow == nullptr) {
      rpcsx_android.fatal("returned native window is null, surface %p",
                          surface);
      return false;
    }

    auto prevWindow = g_native_window.exchange(newWindow);

    if (newWindow != prevWindow) {
      ANativeWindow_acquire(newWindow);

      if (prevWindow != nullptr) {
        ANativeWindow_release(prevWindow);
      }
    }

    if (event == 0 && Emu.IsPaused()) {
      Emu.Resume();
    }
  }

  return true;
}

extern "C" bool _rpcsx_usbDeviceEvent(int fd, int vendorId, int productId,
                                      int event) {
  rpcsx_android.warning(
      "usb device event %d fd: %d, vendorId: %d, productId: %d", event, fd,
      vendorId, productId);

  {
    std::lock_guard lock(g_android_usb_devices_mutex);

    if (event == 0) {
      g_android_usb_devices.push_back({
          .fd = int(fd),
          .vendorId = u16(vendorId),
          .productId = u16(productId),
      });
    } else {
      auto filter = [fd](auto device) { return device.fd == fd; };
      if (auto it = std::ranges::find_if(g_android_usb_devices, filter);
          it != g_android_usb_devices.end()) {
        g_android_usb_devices.erase(it);
      }
    }
  }

  {
    auto selectedHandler = g_cfg_input.player1.handler.get();
    std::string selectedDevice;

    std::map<pad_handler, std::pair<std::unique_ptr<PadHandlerBase>,
                                    std::vector<std::string>>>
        handlerToDevices;

    auto collectDevices = [&]<typename T>(T handler) {
      handler->Init();

      std::vector<std::string> devices;
      for (const auto &device : handler->list_connected_devices()) {
        devices.push_back(device.name);
      }

      auto type = handler->m_type;

      handlerToDevices[type] = std::pair{
          std::move(handler),
          std::move(devices),
      };
    };

    collectDevices(std::make_unique<dualsense_pad_handler>());
    collectDevices(std::make_unique<ds4_pad_handler>());
    collectDevices(std::make_unique<ds3_pad_handler>());

    if (handlerToDevices[selectedHandler].second.empty()) {
      selectedHandler = pad_handler::null;
    }

    if (!handlerToDevices[pad_handler::dualsense].second.empty()) {
      selectedHandler = pad_handler::dualsense;
    } else if (!handlerToDevices[pad_handler::ds4].second.empty()) {
      selectedHandler = pad_handler::ds4;
    } else if (!handlerToDevices[pad_handler::ds3].second.empty()) {
      selectedHandler = pad_handler::ds3;
    }

    if (selectedHandler == pad_handler::null) {
      selectedHandler = pad_handler::virtual_pad;
    }

    if (selectedHandler != g_cfg_input.player1.handler.get()) {
      rpcsx_android.warning("install %s pad handler", selectedHandler);

      g_cfg_input.player1.handler.set(selectedHandler);

      if (selectedHandler == pad_handler::null) {
        g_cfg_input.player1.device.from_default();
      } else if (selectedHandler == pad_handler::virtual_pad) {
        g_cfg_input.player1.handler.set(pad_handler::virtual_pad);
        g_cfg_input.player1.device.from_string("Virtual");
      } else {
        g_cfg_input.player1.device.from_string(
            handlerToDevices[selectedHandler].second.front());
        handlerToDevices[selectedHandler].first->init_config(
            &g_cfg_input.player1.config);
        if (selectedHandler != pad_handler::virtual_pad) {
          std::lock_guard lock(g_virtual_pad_mutex);
          g_virtual_pad = nullptr;
        }
      }

      g_cfg_input.save("", g_cfg_input_configs.default_config);

      if (!Emu.IsStopped()) {
        pad::reset(Emu.GetTitleID());
      }
    }
  }

  return true;
}

static bool installPup(JNIEnv *env, fs::file &&pup_f, jlong progressId) {
  Progress progress(env, progressId);

  pup_object pup(std::move(pup_f));
  AtExit atExit{[&] { pup.file().release_handle(); }};

  if (static_cast<pup_error>(pup) == pup_error::hash_mismatch) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Selected file is not firmware update file");
    return false;
  }

  if (static_cast<pup_error>(pup) != pup_error::ok) {
    const std::string &pup_detail = pup.get_formatted_error();
    rpcsx_android.fatal("installFw: invalid PUP (%s)",
                        pup_detail.empty() ? "unknown error" : pup_detail.c_str());
    progress.failure(pup_detail.empty()
                         ? std::string("Firmware update file is broken")
                         : fmt::format("Firmware update file is broken: %s",
                                       pup_detail));
    return false;
  }

  fs::file update_files_f = pup.get_file(0x300);

  const usz update_files_size = update_files_f ? update_files_f.size() : 0;

  if (!update_files_size) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  tar_object update_files(update_files_f);

  auto update_filenames = update_files.get_filenames();
  update_filenames.erase(std::remove_if(update_filenames.begin(),
                                        update_filenames.end(),
                                        [](const std::string &s) {
                                          return !s.starts_with("dev_flash_");
                                        }),
                         update_filenames.end());

  if (update_filenames.empty()) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  std::string version_string;

  if (fs::file version = pup.get_file(0x100)) {
    version_string = version.to_string();
  }

  if (const usz version_pos = version_string.find('\n');
      version_pos != std::string::npos) {
    version_string.erase(version_pos);
  }

  if (version_string.empty()) {
    rpcsx_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  sendVshBootable(env, progressId);

  jlong processed = 0;
  for (const auto &update_filename : update_filenames) {
    auto update_file_stream = update_files.get_file(update_filename);

    if (update_file_stream->m_file_handler) {
      // Forcefully read all the data
      update_file_stream->m_file_handler->handle_file_op(
          *update_file_stream, 0, update_file_stream->get_size(umax), nullptr);
    }

    fs::file update_file = fs::make_stream(std::move(update_file_stream->data));

    SCEDecrypter self_dec(update_file);
    self_dec.LoadHeaders();
    self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
    self_dec.DecryptData();

    auto dev_flash_tar_f = self_dec.MakeFile();

    if (dev_flash_tar_f.size() < 3) {
      rpcsx_android.error(
          "Firmware installation failed: Firmware could not be decompressed");

      progress.failure("Firmware update file could not be decompressed");
      return false;
    }

    tar_object dev_flash_tar(dev_flash_tar_f[2]);

    if (!dev_flash_tar.extract()) {

      rpcsx_android.error("Error while installing firmware: TAR contents are "
                          "invalid. (package=%s)",
                          update_filename);

      progress.failure(fmt::format("TAR contents are invalid (package=%s)",
                                   update_filename));
      return false;
    }

    if (!progress.report(processed++, update_filenames.size())) {
      // Installation was cancelled
      return false;
    }
  }

  sendFirmwareInstalled(env, utils::get_firmware_version());

  g_compilationQueue.push(progress,
                          g_cfg_vfs.get_dev_flash() + "/vsh/module/vsh.self");
  return true;
}

static bool installPkg(JNIEnv *env, fs::file &&file, jlong progressId) {
  Progress progress(env, progressId);

  std::deque<package_reader> readers;
  std::deque<std::string> bootable_paths;
  readers.emplace_back("dummy.pkg", std::move(file));

  AtExit atExit{[&] {
    for (auto &reader : readers) {
      reader.file().release_handle();
    }
  }};

  package_install_result result = {};
  named_thread worker("PKG Installer", [&readers, &result, &bootable_paths] {
    result = package_reader::extract_data(readers, bootable_paths);
    return result.error == package_install_result::error_type::no_error;
  });

  for (auto &reader : readers) {
    if (auto gameInfo = fetchGameInfo(reader.get_psf())) {
      sendGameInfo(env, progressId, {{*gameInfo}});
    }
  }

  const jlong maxProgress = 10000;

  while (true) {
    std::uint64_t totalProgress = 0;
    for (auto &reader : readers) {
      if (result.error != package_install_result::error_type::no_error) {
        progress.failure("Installation failed");
        for (package_reader &reader : readers) {
          reader.abort_extract();
        }
        return false;
      }

      totalProgress += reader.get_progress(maxProgress);
    }

    if (totalProgress == maxProgress * readers.size()) {
      break;
    }

    totalProgress /= readers.size();

    if (!progress.report(totalProgress, maxProgress)) {
      for (package_reader &reader : readers) {
        reader.abort_extract();
      }

      return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  if (worker()) {
    auto paths = std::vector(bootable_paths.begin(), bootable_paths.end());
    collectGameInfo(env, -1, paths);

    for (auto &path : paths) {
      g_compilationQueue.push(progress, std::move(path));
    }
  }

  return true;
}

static bool installEdat(JNIEnv *env, fs::file &&file, jlong progressId,
                        std::string_view rootPath = {}) {
  Progress progress(env, progressId);

  NPD_HEADER npdHeader;
  if (!file.read(npdHeader)) {
    progress.failure("Invalid EDAT file");
    return false;
  }

  if (!rootPath.empty()) {
    auto ebootPath = locateEbootPath(rootPath);
    auto sfoPath = locateParamSfoPath(rootPath);

    if (sfoPath.empty()) {
      progress.failure("Game is broken: PARAM.SFO not found");
      return false;
    }

    auto psf = psf::load_object(sfoPath);
    auto contentId = psf::get_string(psf, "CONTENT_ID");

    if (contentId != npdHeader.content_id) {
      progress.failure(fmt::format("File cannot be used for this game. EDAT "
                                   "content ID missmatch %s vs %s",
                                   contentId, npdHeader.content_id));
      return false;
    }
  }

  const auto licenseFile =
      fmt::format("%shome/%s/exdata/%s.edat", rpcs3::utils::get_hdd0_dir(),
                  Emu.GetUsr(), npdHeader.content_id);

  file.seek(0);

  std::vector<std::uint8_t> bytes(file.size());
  if (!file.read(bytes)) {
    progress.failure("Failed to read key");
    return false;
  }

  if (!fs::write_file(licenseFile, fs::open_mode::create + fs::open_mode::trunc,
                      bytes)) {
    progress.failure(fmt::format("Failed to write EDAT to %s", licenseFile));
    return false;
  }

  auto root = std::string(rootPath);

  if (root.empty()) {
    root = rpcs3::utils::get_hdd0_dir() + "game";
  }

  collectGameInfo(env, progressId, {std::move(root)});
  return true;
}

static bool installRap(JNIEnv *env, fs::file &&file, jlong progressId,
                       std::string_view rootPath) {
  Progress progress(env, progressId);

  auto ebootPath = locateEbootPath(rootPath);

  std::vector<std::uint8_t> bytes;
  if (!file.read(bytes, 16)) {
    progress.failure("Failed to read key");
    return false;
  }

  SelfAdditionalInfo info;
  decrypt_self(fs::file(ebootPath), nullptr, &info);

  auto npd = [&]() -> NPD_HEADER * {
    for (auto &supplemental : info.supplemental_hdr) {
      if (supplemental.type == 3) {
        return &supplemental.PS3_npdrm_header.npd;
      }
    }

    return nullptr;
  }();

  if (npd == nullptr) {
    progress.failure("Failed to fetch NPDRM of SELF");
    return false;
  }

  const auto licenseFile =
      fmt::format("%shome/%s/exdata/%s.rap", rpcs3::utils::get_hdd0_dir(),
                  Emu.GetUsr(), npd->content_id);

  if (!fs::write_file(licenseFile, fs::open_mode::create + fs::open_mode::trunc,
                      bytes)) {
    progress.failure(fmt::format("Failed to write key to %s", licenseFile));
    return false;
  }

  if (!decrypt_self(fs::file(ebootPath))) {
    progress.failure("Provided key is invalid for selected game");
    fs::remove_file(licenseFile);
    return false;
  }

  collectGameInfo(env, -1, {std::string(rootPath)});
  g_compilationQueue.push(progress, std::move(ebootPath));
  return true;
}

static bool installIso(JNIEnv *env, fs::file &&file, jlong progressId) {
  // Streaming extraction over the ported Loader/ISO reader: every copy runs in
  // bounded chunks (the old dev/iso path materialized each file TWICE - whole
  // extent + to_vector - which OOM-killed installs of ISOs with large inner
  // files). Direct play needs no extraction at all; this is the fallback.
  iso_archive archive(std::move(file));
  Progress progress(env, progressId);

  if (!archive) {
    progress.failure("Failed to read ISO (not ISO9660, or encrypted)");
    return false;
  }

  const psf::registry sfo = archive.open_psf("PS3_GAME/PARAM.SFO");
  const auto title_id = psf::get_string(sfo, "TITLE_ID");

  if (title_id.empty()) {
    progress.failure("Failed to fetch TITLE_ID from PARAM.SFO in ISO");
    return false;
  }

  if (auto gameInfo = fetchGameInfo(sfo)) {
    sendGameInfo(env, progressId, {{*gameInfo}});
  }

  const std::filesystem::path destinationPath =
      fs::get_config_dir() + "games/" + std::string(title_id);

  // Collect all files (relative iso paths) up front for progress accounting
  struct PendingFile {
    std::string isoPath;
    const iso_fs_node *node;
  };

  std::vector<PendingFile> files;
  std::vector<std::string> dirs;

  {
    struct WorkItem {
      const iso_fs_node *node;
      std::string path;
    };

    std::vector<WorkItem> workList;
    workList.push_back({&archive.root(), {}});

    while (!workList.empty()) {
      auto [node, path] = std::move(workList.back());
      workList.pop_back();

      for (const auto &child : node->children) {
        const auto &name = child->metadata.name;

        if (name == "." || name == "..") {
          continue;
        }
        if (path.empty() && name == "PS3_UPDATE") {
          continue;
        }

        std::string childPath = path.empty() ? name : path + "/" + name;

        if (child->metadata.is_directory) {
          dirs.push_back(childPath);
          workList.push_back({child.get(), std::move(childPath)});
        } else {
          files.push_back({std::move(childPath), child.get()});
        }
      }
    }
  }

  progress.report(0, files.size());

  std::error_code ec;
  std::filesystem::create_directories(destinationPath, ec);

  for (const auto &dir : dirs) {
    std::filesystem::create_directories(destinationPath / dir, ec);
    if (ec) {
      progress.failure(fmt::format("Failed to create dir %s: %s",
                                   (destinationPath / dir).string(),
                                   ec.message()));
      return false;
    }
  }

  std::size_t processedFiles = 0;
  std::vector<u8> buffer(1u << 20);

  for (const auto &pending : files) {
    auto src = archive.open(pending.isoPath);

    if (!src) {
      progress.failure(
          fmt::format("Failed to open file in ISO: %s", pending.isoPath));
      return false;
    }

    const auto destPath = destinationPath / pending.isoPath;
    fs::file out(destPath.string(), fs::rewrite);

    if (!out) {
      progress.failure(fmt::format("Failed to create file: %s, dest %s",
                                   destPath.string(),
                                   destinationPath.string()));
      return false;
    }

    u64 remaining = src->size();

    while (remaining) {
      const u64 chunk = std::min<u64>(buffer.size(), remaining);

      if (src->read(buffer.data(), chunk) != chunk ||
          out.write(buffer.data(), chunk) != chunk) {
        progress.failure(fmt::format("Failed to copy file: %s (%s left)",
                                     pending.isoPath, remaining));
        return false;
      }

      remaining -= chunk;
    }

    progress.report(processedFiles++, files.size());
  }

  collectGameInfo(env, -1, {destinationPath.string()});
  auto ebootPath = locateEbootPath(destinationPath.string());
  g_compilationQueue.push(progress, std::move(ebootPath));
  return true;
}

extern "C" bool _rpcsx_installFw(JNIEnv *env, int fd, long progressId) {
  return installPup(env, fs::file::from_native_handle(fd), progressId);
}

extern "C" bool _rpcsx_isInstallableFile(jint fd) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto type = getFileType(file);
  file.seek(0);
  return type != FileType::Unknown &&
         type != FileType::Rap; // FIXME: implement rap preinstallation
}

extern "C" jstring _rpcsx_getDirInstallPath(JNIEnv *env, jint fd) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto psf = psf::load_object(file, "");
  if (auto gameInfo = fetchGameInfo(psf)) {
    return wrap(env, gameInfo->path);
  }

  return nullptr;
}

extern "C" bool _rpcsx_install(JNIEnv *env, int fd, long progressId) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto type = getFileType(file);
  file.seek(0);

  switch (type) {
  case FileType::Unknown:
    Progress(env, progressId).failure("Unsupported file type");
    return false;

  case FileType::Pup:
    return installPup(env, std::move(file), progressId);

  case FileType::Pkg:
    return installPkg(env, std::move(file), progressId);

  case FileType::Edat:
    return installEdat(env, std::move(file), progressId);

  case FileType::Iso:
    return installIso(env, std::move(file), progressId);

  case FileType::Rap:
    Progress(env, progressId)
        .failure("RAP file cannot be preinstalled. Use lock button on "
                 "installed game instead");
    return false;
  }

  return true;
}

extern "C" bool _rpcsx_installKey(JNIEnv *env, int fd, long progressId,
                                  std::string_view gamePath) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto type = getFileType(file);
  file.seek(0);

  if (type == FileType::Rap) {
    return installRap(env, std::move(file), progressId, gamePath);
  }

  if (type == FileType::Edat) {
    return installEdat(env, std::move(file), progressId, gamePath);
  }

  Progress(env, progressId).failure("Unsupported key type");
  return false;
}

// ---- Patch manager bridge ----------------------------------------------
// The core patch_engine (util/bin_patch) already loads patches/patch.yml and
// applies enabled patches per game-hash on boot. patch_engine::load() also
// merges the user's patch_config.yml enabled state into the map, so after a
// load() the map reflects the effective enabled state. We expose enough to let
// the app list patches and flip their enabled flag (persisted via save_config).

static void patch_json_escape(std::string &out, std::string_view s) {
  for (char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        fmt::append(out, "\\u%04x",
                    static_cast<unsigned>(static_cast<unsigned char>(c)));
      } else {
        out += c;
      }
    }
  }
}

extern "C" std::string _rpcsx_patchEngineVersion() {
  return patch_engine_version;
}

// Returns a JSON array: [{hash,name,author,version,notes,serials:[...],enabled}]
extern "C" std::string _rpcsx_patchesList() {
  patch_engine::patch_map patches;
  patch_engine::load(patches, patch_engine::get_patches_path() + "patch.yml");
  patch_engine::load(patches, patch_engine::get_imported_patch_path());

  std::string out = "[";
  bool first = true;

  for (const auto &[hash, container] : patches) {
    for (const auto &[description, info] : container.patch_info_map) {
      bool enabled = false;
      std::set<std::string> serials;
      std::set<std::string> titles;

      for (const auto &[title, serial_map] : info.titles) {
        if (!title.empty()) {
          titles.insert(title);
        }
        for (const auto &[serial, version_map] : serial_map) {
          serials.insert(serial);
          for (const auto &[version, cfg] : version_map) {
            if (cfg.enabled) {
              enabled = true;
            }
          }
        }
      }

      if (!first) {
        out += ",";
      }
      first = false;

      out += "{\"hash\":\"";
      patch_json_escape(out, hash);
      out += "\",\"name\":\"";
      patch_json_escape(out, description);
      out += "\",\"author\":\"";
      patch_json_escape(out, info.author);
      out += "\",\"version\":\"";
      patch_json_escape(out, info.patch_version);
      out += "\",\"notes\":\"";
      patch_json_escape(out, info.notes);
      out += "\",\"serials\":[";
      bool sfirst = true;
      for (const auto &s : serials) {
        if (!sfirst) {
          out += ",";
        }
        sfirst = false;
        out += "\"";
        patch_json_escape(out, s);
        out += "\"";
      }
      out += "],\"titles\":[";
      bool tfirst = true;
      for (const auto &t : titles) {
        if (!tfirst) {
          out += ",";
        }
        tfirst = false;
        out += "\"";
        patch_json_escape(out, t);
        out += "\"";
      }
      out += "],\"enabled\":";
      out += enabled ? "true" : "false";
      out += "}";
    }
  }

  out += "]";
  return out;
}

// Enable/disable a patch (all of its serials/versions) and persist to
// patch_config.yml. Returns false if the patch was not found.
extern "C" bool _rpcsx_patchSetEnabled(std::string_view hash,
                                       std::string_view description,
                                       bool enabled) {
  patch_engine::patch_map patches;
  patch_engine::load(patches, patch_engine::get_patches_path() + "patch.yml");
  patch_engine::load(patches, patch_engine::get_imported_patch_path());

  const auto hash_it = patches.find(std::string(hash));
  if (hash_it == patches.end()) {
    return false;
  }

  const auto desc_it =
      hash_it->second.patch_info_map.find(std::string(description));
  if (desc_it == hash_it->second.patch_info_map.end()) {
    return false;
  }

  for (auto &[title, serial_map] : desc_it->second.titles) {
    for (auto &[serial, version_map] : serial_map) {
      for (auto &[version, cfg] : version_map) {
        cfg.enabled = enabled;
      }
    }
  }

  patch_engine::save_config(patches);
  return true;
}

static cfg::_base *find_cfg_node(cfg::_base *root, std::string_view path);

// --- Per-game custom configuration -----------------------------------------
// Mirrors desktop RPCS3's per-title configs (config/custom_configs/
// config_<serial>.yml). _rpcsx_boot uses cfg_mode::custom, so a config created
// here is applied automatically when that game boots, with the global config as
// fallback. The serial is the game's title id (the install-folder name).

// True if a custom config file exists for this game.
extern "C" bool _rpcsx_customConfigExists(std::string_view serial) {
  if (serial.empty()) {
    return false;
  }
  return fs::is_file(rpcs3::utils::get_custom_config_path(std::string(serial)));
}

// Create a custom config by snapshotting the current global settings, giving
// the user a sensible starting point to tweak (RPCS3's "Create Custom
// Configuration from current settings").
// fs::pending_file (used by Emulator::SaveSettings) creates its temporary file
// inside the target directory and fails with "Not found" if that directory does
// not exist yet. Ensure custom_configs/ is present before saving a per-game
// config, otherwise the very first per-game/community config write silently
// fails and nothing is persisted.
static void ensure_custom_config_dir() {
  fs::create_path(rpcs3::utils::get_custom_config_dir());
}

extern "C" bool _rpcsx_customConfigCreate(std::string_view serial) {
  if (serial.empty()) {
    return false;
  }
  ensure_custom_config_dir();
  // Write an EMPTY config, not a snapshot of the globals. Boot overlays the
  // custom file over the live global config (System.cpp), so an empty file
  // means "inherit everything" - only fields the user later edits get pinned.
  // A full snapshot froze ALL settings at creation time, silently masking any
  // later global tuning ("same settings run slower with a custom config").
  Emulator::SaveSettings("{}\n", std::string(serial));
  return _rpcsx_customConfigExists(serial);
}

// Remove a game's custom config; subsequent boots fall back to the global one.
extern "C" bool _rpcsx_customConfigDelete(std::string_view serial) {
  if (serial.empty()) {
    return false;
  }
  return fs::remove_file(
      rpcs3::utils::get_custom_config_path(std::string(serial)));
}

// Loads the effective per-game config view into cfg: the global config overlaid
// with the game's custom file (if any) - exactly what boot will apply. Filled by
// reference because cfg_root is non-copyable/non-movable.
static void load_effective_custom_cfg(cfg_root &cfg, std::string_view serial) {
  cfg.from_string(g_cfg.to_string());
  if (!serial.empty()) {
    if (fs::file f{rpcs3::utils::get_custom_config_path(std::string(serial))}) {
      cfg.from_string(f.to_string());
    }
  }
}

// Read a settings node for a game (same JSON shape as _rpcsx_settingsGet).
extern "C" std::string _rpcsx_customConfigGet(std::string_view serial,
                                              std::string_view path) {
  cfg_root cfg;
  load_effective_custom_cfg(cfg, serial);
  auto node = find_cfg_node(&cfg, path);
  if (node == nullptr) {
    return nullptr;
  }
  return node->to_json().dump(4);
}

// Set a settings node for a game and persist it to the custom config file.
// Creates the file on first edit (seeded from the global config).
extern "C" bool _rpcsx_customConfigSet(std::string_view serial,
                                       std::string_view path,
                                       std::string_view valueString) {
  if (serial.empty()) {
    return false;
  }

  nlohmann::json value;
  try {
    value = nlohmann::json::parse(valueString);
  } catch (...) {
    rpcsx_android.error(
        "customConfigSet: node %s passed with invalid json '%s'", path,
        valueString);
    return false;
  }

  cfg_root cfg;
  load_effective_custom_cfg(cfg, serial);
  auto node = find_cfg_node(&cfg, path);
  if (node == nullptr) {
    rpcsx_android.error("customConfigSet: node %s not found", path);
    return false;
  }

  if (!node->from_json(value, false)) {
    rpcsx_android.error("customConfigSet: node %s not accepts value '%s'", path,
                        value.dump());
    return false;
  }

  // Persist ONLY the edited node. Untouched settings stay absent from the
  // custom file and keep inheriting the live global config at boot. Saving the
  // full effective snapshot here would pin every setting at its current value.
  YAML::Node yaml_root;

  if (fs::file f{rpcs3::utils::get_custom_config_path(std::string(serial))}) {
    try {
      yaml_root = YAML::Load(f.to_string());
    } catch (...) {
      rpcsx_android.error(
          "customConfigSet: existing custom config unreadable, recreating");
    }
  }

  if (!yaml_root.IsMap()) {
    yaml_root = YAML::Node(YAML::NodeType::Map);
  }

  {
    const auto pathList = fmt::split(path, {"@@"});
    YAML::Node cur;
    cur.reset(yaml_root);

    for (usz i = 0; i < pathList.size(); i++) {
      if (i + 1 == pathList.size()) {
        // Leaf: store the validated scalar (enum/string as-is; bool/number via
        // their JSON spelling, which matches the YAML scalar form).
        if (value.is_string()) {
          cur[pathList[i]] = value.get<std::string>();
        } else {
          cur[pathList[i]] = value.dump();
        }
        break;
      }

      YAML::Node next = cur[pathList[i]];
      if (!next.IsMap()) {
        cur[pathList[i]] = YAML::Node(YAML::NodeType::Map);
        next.reset(cur[pathList[i]]);
      }
      cur.reset(next);
    }
  }

  ensure_custom_config_dir();
  Emulator::SaveSettings(YAML::Dump(yaml_root) + "\n", std::string(serial));
  return true;
}

// Import a community/recommended config (a sparse YAML string) as a game's
// custom config. The preset specifies only the keys it changes; we persist
// ONLY those keys (each validated against the schema) so every unmentioned
// setting keeps inheriting the user's live globals at boot - exactly like
// manual per-game edits. A full snapshot would pin every setting and make
// community configs perform worse than setting the same options by hand.
extern "C" bool _rpcsx_customConfigImport(std::string_view serial,
                                          std::string_view yaml) {
  if (serial.empty()) {
    return false;
  }

  YAML::Node incoming;
  try {
    incoming = YAML::Load(std::string(yaml));
  } catch (...) {
    rpcsx_android.error("customConfigImport: unparseable YAML for %s", serial);
    return false;
  }
  if (!incoming.IsMap()) {
    rpcsx_android.error("customConfigImport: top-level YAML is not a map for %s",
                        serial);
    return false;
  }

  // Merge into any existing custom file so re-importing doesn't drop prior
  // sparse edits (matches customConfigSet).
  YAML::Node yaml_root;
  if (fs::file f{rpcs3::utils::get_custom_config_path(std::string(serial))}) {
    try {
      yaml_root = YAML::Load(f.to_string());
    } catch (...) {
      rpcsx_android.error(
          "customConfigImport: existing custom config unreadable, recreating");
    }
  }
  if (!yaml_root.IsMap()) {
    yaml_root = YAML::Node(YAML::NodeType::Map);
  }

  // Validate each leaf against the live schema (seeded from globals so enum/
  // range checks match what boot uses). Only validated keys get written.
  cfg_root validator;
  validator.from_string(g_cfg.to_string());

  bool wrote_any = false;

  std::function<void(const YAML::Node &, const std::string &)> walk =
      [&](const YAML::Node &node, const std::string &prefix) {
        if (!node.IsMap()) {
          return;
        }
        for (const auto &kv : node) {
          if (!kv.first.IsScalar()) {
            continue;
          }
          const std::string key = kv.first.Scalar();
          const std::string path = prefix.empty() ? key : prefix + "@@" + key;

          if (kv.second.IsMap()) {
            walk(kv.second, path);
            continue;
          }
          if (!kv.second.IsScalar()) {
            continue;
          }

          auto schema = find_cfg_node(&validator, path);
          if (schema == nullptr) {
            rpcsx_android.error("customConfigImport: unknown key '%s' skipped",
                                path);
            continue;
          }

          const std::string scalar = kv.second.Scalar();
          if (!schema->from_string(scalar, false)) {
            rpcsx_android.error(
                "customConfigImport: value '%s' rejected for key '%s'", scalar,
                path);
            continue;
          }

          // Write the validated scalar into the sparse output tree.
          const auto pathList = fmt::split(path, {"@@"});
          YAML::Node cur;
          cur.reset(yaml_root);
          for (usz i = 0; i < pathList.size(); i++) {
            if (i + 1 == pathList.size()) {
              cur[pathList[i]] = scalar;
              break;
            }
            YAML::Node next = cur[pathList[i]];
            if (!next.IsMap()) {
              cur[pathList[i]] = YAML::Node(YAML::NodeType::Map);
              next.reset(cur[pathList[i]]);
            }
            cur.reset(next);
          }
          wrote_any = true;
        }
      };

  walk(incoming, "");

  if (!wrote_any) {
    rpcsx_android.error("customConfigImport: no valid keys in preset for %s",
                        serial);
    return false;
  }

  ensure_custom_config_dir();
  Emulator::SaveSettings(YAML::Dump(yaml_root) + "\n", std::string(serial));
  return _rpcsx_customConfigExists(serial);
}

extern "C" std::string _rpcsx_systemInfo() {
  std::string result;

  // Show the CPU the JIT actually targets (resolved from the empty config via the
  // MIDR detection), not the raw empty setting, so it's clear which core codegen
  // is tuned for.
  fmt::append(result, "%s\n\nLLVM CPU: %s\n\n", utils::get_system_info(),
              jit_compiler::cpu(g_cfg.core.llvm_cpu));

  {
    vk::instance device_enum_context;
    if (device_enum_context.create("RPCS3")) {
      device_enum_context.bind();
      const std::vector<vk::physical_device> &gpus =
          device_enum_context.enumerate_devices();

      for (const auto &gpu : gpus) {
        fmt::append(result, "GPU: %s\n\nDriver: %s (v%s)\n\nVulkan: %s",
                    gpu.get_name(), gpu.get_driver_name(),
                    gpu.get_driver_version(), gpu.get_driver_vk_version());
      }
    }
  }

  return result;
}

static cfg::_base *find_cfg_node(cfg::_base *root, std::string_view path) {
  auto pathList = fmt::split(path, {"@@"});
  std::ranges::reverse(pathList);

  while (!pathList.empty()) {
    auto elem = pathList.back();
    pathList.pop_back();
    if (elem.empty()) {
      continue;
    }

    auto root_node = dynamic_cast<cfg::node *>(root);
    if (root_node == nullptr) {
      return nullptr;
    }

    cfg::_base *child_node = nullptr;

    for (auto node : root_node->get_nodes()) {
      if (node->get_name() == elem) {
        child_node = node;
        break;
      }
    }

    if (child_node == nullptr) {
      return nullptr;
    }

    root = child_node;
  }

  return root;
}

extern "C" void _rpcsx_loginUser(std::string_view userId) {
  Emu.SetUsr(std::string(userId));
}

extern "C" std::string _rpcsx_getUser() { return Emu.GetUsr(); }

extern "C" std::string _rpcsx_settingsGet(std::string_view path) {
  auto root = find_cfg_node(&g_cfg, path);

  if (root == nullptr) {
    return nullptr;
  }

  return root->to_json().dump(4);
}

extern "C" bool _rpcsx_settingsSet(std::string_view path,
                                   std::string_view valueString) {
  nlohmann::json value;
  try {
    value = nlohmann::json::parse(valueString);
  } catch (...) {
    rpcsx_android.error("settingsSet: node %s passed with invalid json '%s'",
                        path, valueString);
    return false;
  }

  auto root = find_cfg_node(&g_cfg, path);

  if (root == nullptr) {
    rpcsx_android.error("settingsSet: node %s not found", path);
    return false;
  }

  if (!root->from_json(value, !Emu.IsStopped())) {
    rpcsx_android.error("settingsSet: node %s not accepts value '%s'", path,
                        value.dump());
    return false;
  }

  Emulator::SaveSettings(g_cfg.to_string(), "");
  return true;
}

// Android low-RAM guard. Caps concurrent LLVM compile threads at the effective
// thread-pool level (see rpcs3::utils::get_compile_thread_cap), so it survives
// per-game custom configs that the global "Max LLVM Compile Threads" cannot. The
// app derives the value from device RAM. 0 (or <=0) disables the cap.
extern "C" void _rpcsx_setMaxCompileThreads(int count) {
  rpcs3::utils::set_compile_thread_cap(count > 0 ? static_cast<u32>(count) : 0u);
}

// Android LLVM compile MEMORY budget in bytes (the concurrent-compile RAM ceiling
// the PPU compiler uses instead of the unreliable get_total_memory()/3). The app
// derives a device-scaled, usable figure from ActivityManager. <=0 = unset (core
// falls back to its corrected internal cap).
extern "C" void _rpcsx_setCompileMemoryBudget(long long bytes) {
  rpcs3::utils::set_compile_memory_budget(bytes > 0 ? static_cast<u64>(bytes) : 0ull);
  rpcsx_android.notice("Compile memory budget set to %lld bytes", bytes);
}

// Android battery-saver toggle (see rpcs3::utils::get_power_save_mode).
extern "C" void _rpcsx_setPowerSaveMode(int on) {
  rpcs3::utils::set_power_save_mode(on != 0);
  rpcsx_android.notice("Power: battery-saver mode %s", on ? "ON" : "off");
}

// Android thermal throttle: frame-rate cap (fps) when the SoC is hot; 0 = none.
extern "C" void _rpcsx_setThermalFrameCap(float fps) {
  rpcs3::utils::set_thermal_frame_cap(fps);
  rpcsx_android.notice("Power: thermal frame cap %.0f fps", fps);
}

// Android ADPF feed (read side): the presenting RSX thread's OS tid (0 until the
// RSX thread has flipped once) and this frame's actual CPU work in nanoseconds.
// The app polls these to drive a PerformanceHintManager session. Advisory only.
extern "C" int _rpcsx_getRsxThreadTid() {
  return rpcs3::utils::get_rsx_thread_tid();
}

extern "C" long long _rpcsx_getFrameWorkNanos() {
  return static_cast<long long>(rpcs3::utils::get_frame_work_ns());
}

extern "C" long long _rpcsx_getFramePeriodNanos() {
  return static_cast<long long>(rpcs3::utils::get_frame_period_ns());
}

// Android experimental: bias PPU/SPU/RSX onto the big CPU cluster.
extern "C" void _rpcsx_setCpuAffinityMode(int on) {
  thread_ctrl::set_android_affinity(on != 0);
  rpcsx_android.notice("Power: big-cluster CPU affinity %s", on ? "ON" : "off");
}

// Android experimental: low-power WFE waiting (RSX semaphore park).
extern "C" void _rpcsx_setWfeMode(int on) {
  rx::set_wfe_mode(on != 0);
  rpcsx_android.notice("Power: low-power WFE waiting %s", on ? "ON" : "off");
}

extern "C" void _rpcsx_setSmoothShaders(int on) {
  rpcs3::utils::set_smooth_shaders(on != 0);
  rpcsx_android.notice("Video: smooth shaders (async interpreter) %s",
                       on ? "ON" : "off");
}

// Android GPU turbo (max Adreno clocks). The KGSL ioctl is app-only (adrenotools), so the core
// stores the flag and calls back into an app-registered handler. This lets the in-game home-menu
// toggle drive the exact same path as the app's startup apply.
extern "C" void _rpcsx_setGpuTurbo(int on) {
  rpcs3::utils::set_gpu_turbo(on != 0);
  rpcsx_android.notice("GPU: turbo (max clocks) %s", on ? "ON" : "off");
}

extern "C" void _rpcsx_registerGpuTurboHandler(void (*handler)(bool)) {
  rpcs3::utils::set_gpu_turbo_handler(handler);
}

extern "C" std::string _rpcsx_getVersion() {
  return rx::getVersion().toString();
}

// ---------------------------------------------------------------------------
// RPCN JNI bridge. The app drives the PSN/RPCN account + host configuration
// through these. Network-blocking calls (create account, resend token, test
// connection) are invoked off the UI thread by the app side.
// ---------------------------------------------------------------------------

namespace {
// Minimal JSON string escaper for the config getters. npid/host/token rarely
// contain quotes or backslashes, but escape them to keep the JSON well-formed.
static std::string rpcn_json_escape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 2);
  for (char c : in) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out += c; break;
    }
  }
  return out;
}

// Human-readable text for an RPCN account/login ErrorType. Mirrors the upstream
// error_to_explanation table (rpcn_client.cpp), which is not exported.
static std::string rpcn_error_to_string(rpcn::ErrorType error) {
  switch (error) {
  case rpcn::ErrorType::NoError: return "";
  case rpcn::ErrorType::Malformed: return "Sent packet was malformed.";
  case rpcn::ErrorType::Invalid: return "Sent command was invalid.";
  case rpcn::ErrorType::InvalidInput: return "Sent data was invalid.";
  case rpcn::ErrorType::TooSoon:
    return "Request happened too soon, please wait before retrying.";
  case rpcn::ErrorType::LoginError: return "Unknown login error.";
  case rpcn::ErrorType::LoginAlreadyLoggedIn: return "User is already logged in.";
  case rpcn::ErrorType::LoginInvalidUsername: return "Login error: invalid username.";
  case rpcn::ErrorType::LoginInvalidPassword: return "Login error: invalid password.";
  case rpcn::ErrorType::LoginInvalidToken: return "Login error: invalid token.";
  case rpcn::ErrorType::CreationError: return "Error creating an account.";
  case rpcn::ErrorType::CreationExistingUsername:
    return "An account with that username already exists.";
  case rpcn::ErrorType::CreationBannedEmailProvider:
    return "This email provider is banned.";
  case rpcn::ErrorType::CreationExistingEmail:
    return "An account with that email already exists.";
  case rpcn::ErrorType::DbFail: return "A database query failed on the server.";
  case rpcn::ErrorType::EmailFail: return "An email action failed on the server.";
  case rpcn::ErrorType::NotFound: return "Requested object was not found.";
  case rpcn::ErrorType::Unauthorized: return "Unauthorized operation.";
  default: return "RPCN error.";
  }
}
} // namespace

// Return current RPCN config as JSON {host, npid, password, token}.
extern "C" std::string _rpcsx_rpcnGetConfig() {
  g_cfg_rpcn.load();
  std::string out = "{\"host\":\"";
  out += rpcn_json_escape(g_cfg_rpcn.get_host());
  out += "\",\"npid\":\"";
  out += rpcn_json_escape(g_cfg_rpcn.get_npid());
  out += "\",\"password\":\"";
  // Intentionally empty: the stored password is a derived hash (PBKDF2-SHA3),
  // not the raw password. Returning it would let the UI re-submit and re-derive
  // it (double hash -> login fails), and needlessly echoes the secret hash back.
  out += "\",\"token\":\"";
  out += rpcn_json_escape(g_cfg_rpcn.get_token());
  out += "\"}";
  return out;
}

extern "C" void _rpcsx_rpcnSetCredentials(std::string_view npid,
                                          std::string_view password,
                                          std::string_view token) {
  g_cfg_rpcn.set_npid(npid);
  // Derive the password client-side (PBKDF2-SHA3) before storing/sending; the
  // RPCN server only accepts the derived form. An empty password means the user
  // did not retype it, so keep the existing stored (already-derived) value.
  if (!password.empty()) {
    g_cfg_rpcn.set_password(rpcn::derive_password(password));
  }
  g_cfg_rpcn.set_token(token);
  g_cfg_rpcn.save();
}

// Return host list as JSON [{description, host}].
extern "C" std::string _rpcsx_rpcnGetHosts() {
  g_cfg_rpcn.load();
  const auto hosts = g_cfg_rpcn.get_hosts();
  std::string out = "[";
  bool first = true;
  for (const auto &[description, host] : hosts) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "{\"description\":\"";
    out += rpcn_json_escape(description);
    out += "\",\"host\":\"";
    out += rpcn_json_escape(host);
    out += "\"}";
  }
  out += "]";
  return out;
}

extern "C" bool _rpcsx_rpcnAddHost(std::string_view description,
                                   std::string_view host) {
  const bool added = g_cfg_rpcn.add_host(description, host);
  if (added) {
    g_cfg_rpcn.save();
  }
  return added;
}

extern "C" bool _rpcsx_rpcnRemoveHost(std::string_view host) {
  auto hosts = g_cfg_rpcn.get_hosts();
  // Never remove the last/only entry: RPCN always needs a server to point at.
  if (hosts.size() <= 1) {
    return false;
  }

  std::vector<std::pair<std::string, std::string>> filtered;
  filtered.reserve(hosts.size());
  for (auto &entry : hosts) {
    if (entry.second != host) {
      filtered.push_back(std::move(entry));
    }
  }

  if (filtered.size() == hosts.size()) {
    return false; // nothing matched
  }

  // set_hosts is private; del_host removes by (description, host) pair.
  bool removed = false;
  for (const auto &[description, h] : hosts) {
    if (h == host) {
      if (g_cfg_rpcn.del_host(description, h)) {
        removed = true;
      }
    }
  }
  if (removed) {
    g_cfg_rpcn.save();
  }
  return removed;
}

extern "C" void _rpcsx_rpcnSetActiveHost(std::string_view host) {
  g_cfg_rpcn.set_host(host);
  g_cfg_rpcn.save();
  // Drop any live connection so the next test/sign-in reconnects to the NEW
  // host instead of silently reusing the cached connection to the old one.
  if (auto client = rpcn::rpcn_client::get_instance(0)) {
    client->server_infos_updated();
  }
}

extern "C" std::string _rpcsx_rpcnGetActiveHost() {
  g_cfg_rpcn.load();
  return g_cfg_rpcn.get_host();
}

// Create an RPCN account. Returns "" on success, else a human error string.
// The country arg is collected by the UI for future use; create_user has no
// country parameter so it is intentionally not forwarded.
extern "C" std::string _rpcsx_rpcnCreateAccount(std::string_view npid,
                                                std::string_view password,
                                                std::string_view online_name,
                                                std::string_view email,
                                                std::string_view country) {
  (void)country;
  g_cfg_rpcn.load();

  auto client = rpcn::rpcn_client::get_instance(0);
  if (!client) {
    return "Failed to obtain RPCN client instance.";
  }
  client->clear_failure_state(); // clear any stale failure so this attempt reconnects

  if (auto state = client->wait_for_connection();
      state != rpcn::rpcn_state::failure_no_failure) {
    return rpcn::rpcn_state_to_string(state);
  }

  // Derive once; create_user and the stored credential must use the same value.
  const std::string derived = rpcn::derive_password(password);
  // Match upstream rpcs3qt: send the default avatar URL, not an empty string,
  // so the create packet is identical to the validated desktop client.
  const auto error = client->create_user(npid, derived, online_name,
                                         "https://rpcs3.net/cdn/netplay/DefaultAvatar.png", email);
  if (error != rpcn::ErrorType::NoError) {
    return rpcn_error_to_string(error);
  }

  // Persist credentials (derived password) so the user can then verify the token.
  g_cfg_rpcn.set_npid(npid);
  g_cfg_rpcn.set_password(derived);
  g_cfg_rpcn.save();
  return "";
}

// Resend the account verification token. Returns "" on success, else an error.
extern "C" std::string _rpcsx_rpcnResendToken() {
  g_cfg_rpcn.load();
  const std::string npid = g_cfg_rpcn.get_npid();
  const std::string password = g_cfg_rpcn.get_password();

  auto client = rpcn::rpcn_client::get_instance(0);
  if (!client) {
    return "Failed to obtain RPCN client instance.";
  }
  client->clear_failure_state(); // clear any stale failure so this attempt reconnects

  if (auto state = client->wait_for_connection();
      state != rpcn::rpcn_state::failure_no_failure) {
    return rpcn::rpcn_state_to_string(state);
  }

  const auto error = client->resend_token(npid, password);
  if (error != rpcn::ErrorType::NoError) {
    return rpcn_error_to_string(error);
  }
  return "";
}

// Persistent strong ref to the RPCN client. The singleton is held via a weak_ptr, so without a
// strong ref it is destroyed the instant a JNI call returns - which is why the live status read
// "offline" even right after a successful connection (the connected client was already gone).
// Holding a ref while online is enabled keeps the authenticated connection alive for the status
// poll, and lets a subsequently-booted game's NP handler reuse the live connection.
static std::mutex g_rpcn_persistent_mutex;
static std::shared_ptr<rpcn::rpcn_client> g_rpcn_persistent;

static void rpcn_hold(std::shared_ptr<rpcn::rpcn_client> client) {
  std::lock_guard lock(g_rpcn_persistent_mutex);
  g_rpcn_persistent = std::move(client);
}

static void rpcn_release() {
  std::lock_guard lock(g_rpcn_persistent_mutex);
  g_rpcn_persistent.reset();
}

// Test the RPCN connection + authentication. Returns "" if both succeed,
// else the human string for the failing state. On success the connection is held alive so the
// status indicator reflects it (and a later game reuses it) instead of dropping immediately.
extern "C" std::string _rpcsx_rpcnTestConnection() {
  auto client = rpcn::rpcn_client::get_instance(0);
  if (!client) {
    return "Failed to obtain RPCN client instance.";
  }
  client->clear_failure_state(); // clear any stale failure so this attempt reconnects

  if (auto state = client->wait_for_connection();
      state != rpcn::rpcn_state::failure_no_failure) {
    return rpcn::rpcn_state_to_string(state);
  }

  if (auto state = client->wait_for_authentified();
      state != rpcn::rpcn_state::failure_no_failure) {
    return rpcn::rpcn_state_to_string(state);
  }

  rpcn_hold(client); // keep the now-connected client alive for the live status indicator
  return "";
}

// Enable/disable RPCN. Enable sets PSN=RPCN and Internet=enabled; disable sets
// PSN=disabled. Persisted via the same path the settings JNI uses.
extern "C" void _rpcsx_rpcnSetEnabled(int enabled) {
  if (enabled) {
    g_cfg.net.psn_status.set(np_psn_status::psn_rpcn);
    g_cfg.net.net_active.set(np_internet_status::enabled);
    rpcn_hold(rpcn::rpcn_client::get_instance(0)); // create + hold a live client (the app then connects it)
  } else {
    // Disable both PSN and Internet so the live config is fully offline (symmetric with the
    // enable path), and gracefully tear down any live session so disabling takes effect
    // immediately instead of only at the next game boot. NOTE: a per-game custom config that
    // pins net status still overrides the global config at game boot, so a game set online via
    // its per-game config keeps reconnecting until that per-game online setting is also cleared
    // (global-authoritative-over-per-game override is a separate, larger change).
    g_cfg.net.psn_status.set(np_psn_status::disabled);
    g_cfg.net.net_active.set(np_internet_status::disabled);
    rpcn::rpcn_client::terminate_active_session();
    rpcn_release(); // drop the persistent ref so the offline client is fully torn down
  }
  Emulator::SaveSettings(g_cfg.to_string(), "");
  rpcsx_android.notice("RPCN: %s", enabled ? "enabled" : "disabled");
}

extern "C" bool _rpcsx_rpcnIsEnabled() {
  return g_cfg.net.psn_status.get() == np_psn_status::psn_rpcn;
}

// Passive, non-blocking RPCN connection status for a live indicator. Reads the state of an
// existing session WITHOUT creating a client or reconnecting (unlike _rpcsx_rpcnTestConnection).
// Returns "online" (connected + authenticated), "connecting" (socket up, not yet authed), or
// "offline" (no live session).
extern "C" std::string _rpcsx_rpcnLiveStatus() {
  auto client = rpcn::rpcn_client::get_active_instance();
  if (!client) {
    return "offline";
  }
  if (client->is_authentified()) {
    return "online";
  }
  if (client->is_connected()) {
    return "connecting";
  }
  return "offline";
}

extern "C" void *_rpcsx_setCustomDriver(void *driverHandle) {
  auto prevLoader = vk::instance::g_vk_loader;
  if (prevLoader != nullptr) {
    vk::symbol_cache::cache_instance().clear();
  }

  vk::instance::g_vk_loader = driverHandle;

  if (driverHandle != nullptr) {
    vk::symbol_cache::cache_instance().initialize();
  }

  return prevLoader;
}

#pragma GCC diagnostic pop
