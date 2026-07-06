#pragma once

#include "util/types.hpp"
#include <string>

enum class game_content_type
{
	content_icon,         // ICON0.PNG
	content_video,        // ICON1.PAM
	content_sound,        // SND0.AT3
	overlay_picture,      // PIC0.PNG (16:9) or PIC2.PNG (4:3)
	background_picture,   // PIC1.PNG
	background_picture_2, // PIC3.PNG (should only exist for install or extra content discs...)
};

namespace rpcs3::utils
{
	u32 get_max_threads();

	// Android low-RAM guard: hard cap on how many LLVM modules compile
	// concurrently during first-boot recompilation, so the kernel low-memory
	// killer does not SIGKILL the process mid-compile. 0 = no cap (default;
	// desktop/high-RAM behaviour is byte-identical to upstream). The Android app
	// sets this from device RAM via JNI; because it is applied to the *effective*
	// thread count it cannot be undone by a global or per-game custom config.
	void set_compile_thread_cap(u32 cap);
	u32 get_compile_thread_cap();

	// Android compile MEMORY budget (bytes; 0 = unset). The PPU LLVM compiler uses this as
	// the concurrent-compile memory ceiling instead of get_total_memory()/3. get_total_memory()
	// is accurate physical RAM (no zRAM/swap inflation - sysconf counts totalram only), but a
	// single Android process cannot allocate near all of it before the per-process / Low Memory
	// Killer limit fires, so total/3 is too loose. The app pushes a device-scaled, per-process-
	// safe figure via JNI. Sized below the largest expected single module so big modules
	// serialize (one at a time) while small modules still compile concurrently - the OOM guard
	// is the budget, not the thread count.
	void set_compile_memory_budget(u64 bytes);
	u64 get_compile_memory_budget();

	// Android battery-saver. When on, the core makes low-power choices at the
	// EFFECTIVE level (so a saved/per-game config can't undo it, like the compile
	// cap): forces FIFO present (caps the GPU to the display) and collapses the
	// SPU GETLLAR busy-wait so idle SPU reservation polls park instead of pinning
	// a big core. Default false = off = byte-identical to stock. The app toggles it.
	void set_power_save_mode(bool on);
	bool get_power_save_mode();

	// Android thermal throttle: a frame-rate cap (fps) applied when the SoC gets
	// hot, so the pipeline does less work and the device can cool (less fan / less
	// hard-throttle stutter). 0 = no cap (default). The app sets it from
	// PowerManager thermal status; it's clamped against the configured frame limit.
	void set_thermal_frame_cap(float fps);
	float get_thermal_frame_cap();

	// Android ADPF (PerformanceHintManager) feed. The RSX flip loop publishes the
	// OS thread id of the presenting thread and this frame's actual CPU work time
	// (wall interval minus idle/limiter sleep). The app polls these to drive a
	// performance hint session so the scheduler can pick the lowest CPU clock that
	// still hits the frame target = same fps, less heat. Pure publish: the values
	// are advisory and read-only to the core, so rendering is unaffected whether or
	// not the app consumes them. tid 0 / work 0 = not yet known.
	void set_rsx_thread_tid(int tid);
	int get_rsx_thread_tid();
	void report_frame_work_ns(u64 ns);
	u64 get_frame_work_ns();
	void report_frame_period_ns(u64 ns);
	u64 get_frame_period_ns();

	// True when low-power waiting should be used at idle spin sites: either the
	// battery-saver override is on, or the experimental WFE toggle is on. Lets the
	// proven WFE/spin-trim wins reach every battery-saver user (the two toggles
	// were previously decoupled, so battery-saver users never got the WFE parks).
	bool low_power_wait_enabled();

	// Android "smooth shaders": when on (default), the VK backend bumps the
	// default shader mode (async_recompiler) to async_with_interpreter, so new
	// shaders render via the interpreter while compiling instead of popping in -
	// the textbook anti-shader-stutter mode. Session-only (does not touch the
	// saved config); an explicit non-default shader mode is left untouched.
	void set_smooth_shaders(bool on);
	bool get_smooth_shaders();

	// GPU turbo (max Adreno clocks). The actual KGSL ioctl lives in the app (adrenotools), so the
	// core only stores the flag and forwards to an app-registered handler - this lets the in-game
	// home-menu toggle drive the same path as the app's startup apply.
	void set_gpu_turbo(bool on);
	bool get_gpu_turbo();
	void set_gpu_turbo_handler(void (*handler)(bool on));

	void configure_logs(bool force_enable = false);

	u32 check_user(const std::string& user);

	bool install_pkg(const std::string& path);

	std::string get_emu_dir();
	std::string get_games_dir();
	std::string get_hdd0_dir();
	std::string get_hdd1_dir();
	std::string get_cache_dir();
	std::string get_cache_dir(std::string_view module_path);
	// Directory holding redump .dkey/.key files for encrypted ISOs.
	std::string get_redump_key_dir();

	std::string get_rap_file_path(const std::string_view& rap);
	bool verify_c00_unlock_edat(const std::string_view& content_id, bool fast = false);
	std::string get_sfo_dir_from_game_path(const std::string& game_path, const std::string& title_id = "");

	std::string get_custom_config_dir();
	std::string get_custom_config_path(const std::string& identifier);

	std::string get_input_config_root();
	std::string get_input_config_dir(const std::string& title_id = "");
	std::string get_custom_input_config_path(const std::string& title_id);

	std::string get_game_content_path(game_content_type type);

	bool version_is_bigger(std::string_view v0, std::string_view v1, std::string_view serial, bool is_fw);
} // namespace rpcs3::utils
