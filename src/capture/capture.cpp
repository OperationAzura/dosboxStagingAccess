// SPDX-FileCopyrightText:  2023-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "capture.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <vector>

#include "private/capture_audio.h"
#include "private/capture_midi.h"
#include "private/capture_video.h"

#include "config/config.h"
#include "config/setup.h"
#include "dosbox_config.h"
#include "gui/mapper.h"
#include "gui/titlebar.h"
#include "image/image_capturer.h"
#include "misc/support.h"
#include "utils/checks.h"
#include "utils/fs_utils.h"
#include "utils/string_utils.h"

// must be included after dosbox_config.h
#include <SDL.h>

CHECK_NARROWING();

static struct {
	std_fs::path path     = {};
	bool path_initialised = false;

	struct {
		std::atomic<CaptureState> audio = {};
		std::atomic<CaptureState> midi  = {};
		std::atomic<CaptureState> video = {};
	} state = {};

	struct {
		int32_t audio              = 1;
		int32_t midi               = 1;
		int32_t raw_opl_stream     = 1;
		int32_t rad_opl_instrument = 1;
		int32_t video              = 1;
		int32_t image              = 1;
		int32_t serial_log         = 1;
	} next_index = {};

	void reset()
	{
		path.clear();
		path_initialised = false;
		state.audio = CaptureState::Off;
		state.midi = CaptureState::Off;
		state.video = CaptureState::Off;
		next_index = {};
	}
} capture = {};

static std::unique_ptr<ImageCapturer> image_capturer = {};


// DARKTEXT_LIVE_FRAME -----------------------------------------------------
// Accessibility framebuffer tap.
//
// Store every frame handed to CAPTURE_AddFrame. DOSBox Staging can hand the
// capture path Indexed8, RGB555, RGB565, BGR24, or BGRX32 frames. Preserve the
// raw bytes and convert them to PPM only when HTTP requests the latest frame.

static std::atomic<bool> live_frame_enabled = true;
static std::mutex live_frame_mutex = {};

static struct {
    int width  = 0;
    int height = 0;
    int pitch  = 0;

    bool double_width  = false;
    bool double_height = false;
    bool is_flipped_vertically = false;

    PixelFormat pixel_format = PixelFormat::Indexed8;

    std::vector<uint8_t> pixels = {};
    std::array<Rgb888, NumVgaColors> palette = {};
} live_frame = {};

void CAPTURE_SetLiveFrameEnabled(const bool enabled)
{
    live_frame_enabled.store(enabled, std::memory_order_relaxed);

    if (!enabled) {
        std::lock_guard<std::mutex> lock(live_frame_mutex);
        live_frame.width  = 0;
        live_frame.height = 0;
        live_frame.pitch  = 0;
        live_frame.pixels.clear();
    }
}

bool CAPTURE_IsLiveFrameEnabled()
{
    return live_frame_enabled.load(std::memory_order_relaxed);
}

static void update_live_frame(const RenderedImage& image)
{
    if (!image.image_data || image.params.width <= 0 ||
        image.params.height <= 0 || image.pitch <= 0) {
        return;
    }

    const auto num_bytes = static_cast<size_t>(image.params.height) *
                           static_cast<size_t>(image.pitch);

    std::lock_guard<std::mutex> lock(live_frame_mutex);

    live_frame.width  = image.params.width;
    live_frame.height = image.params.height;
    live_frame.pitch  = image.pitch;

    live_frame.double_width  = image.params.double_width;
    live_frame.double_height = image.params.double_height;
    live_frame.is_flipped_vertically = image.is_flipped_vertically;
    live_frame.pixel_format = image.params.pixel_format;

    live_frame.palette = image.palette;
    live_frame.pixels.assign(image.image_data, image.image_data + num_bytes);
}

static uint8_t expand_5_to_8(const uint16_t value)
{
    const auto v = static_cast<uint8_t>(value & 0x1f);
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}

static uint8_t expand_6_to_8(const uint16_t value)
{
    const auto v = static_cast<uint8_t>(value & 0x3f);
    return static_cast<uint8_t>((v << 2) | (v >> 4));
}

std::string CAPTURE_GetLiveFramePpm()
{
    int width  = 0;
    int height = 0;
    int pitch  = 0;

    bool double_width  = false;
    bool double_height = false;
    bool is_flipped_vertically = false;

    PixelFormat pixel_format = PixelFormat::Indexed8;

    std::vector<uint8_t> pixels = {};
    std::array<Rgb888, NumVgaColors> palette = {};

    {
        std::lock_guard<std::mutex> lock(live_frame_mutex);

        if (live_frame.pixels.empty()) {
            return {};
        }

        width  = live_frame.width;
        height = live_frame.height;
        pitch  = live_frame.pitch;

        double_width  = live_frame.double_width;
        double_height = live_frame.double_height;
        is_flipped_vertically = live_frame.is_flipped_vertically;
        pixel_format = live_frame.pixel_format;

        pixels  = live_frame.pixels;
        palette = live_frame.palette;
    }

    const auto x_scale = double_width ? 2 : 1;
    const auto y_scale = double_height ? 2 : 1;

    const auto out_width  = width * x_scale;
    const auto out_height = height * y_scale;

    auto ppm = std::string("P6\n") + std::to_string(out_width) + " " +
               std::to_string(out_height) + "\n255\n";

    ppm.reserve(ppm.size() +
                static_cast<size_t>(out_width) *
                static_cast<size_t>(out_height) * 3);

    auto append_rgb = [&ppm](const uint8_t red,
                             const uint8_t green,
                             const uint8_t blue) {
        ppm.push_back(static_cast<char>(red));
        ppm.push_back(static_cast<char>(green));
        ppm.push_back(static_cast<char>(blue));
    };

    for (int y = 0; y < height; ++y) {
        const auto source_y =
                is_flipped_vertically ? (height - 1 - y) : y;

        const auto* row =
                pixels.data() + static_cast<size_t>(source_y) * pitch;

        for (int yr = 0; yr < y_scale; ++yr) {
            for (int x = 0; x < width; ++x) {
                uint8_t red   = 0;
                uint8_t green = 0;
                uint8_t blue  = 0;

                switch (pixel_format) {
                case PixelFormat::Indexed8: {
                    const auto colour = palette[row[x]];
                    red   = colour.red;
                    green = colour.green;
                    blue  = colour.blue;
                } break;

                case PixelFormat::RGB555_Packed16: {
                    uint16_t pixel = 0;
                    std::memcpy(&pixel, row + x * 2, sizeof(pixel));

                    red   = expand_5_to_8((pixel >> 10) & 0x1f);
                    green = expand_5_to_8((pixel >> 5) & 0x1f);
                    blue  = expand_5_to_8(pixel & 0x1f);
                } break;

                case PixelFormat::RGB565_Packed16: {
                    uint16_t pixel = 0;
                    std::memcpy(&pixel, row + x * 2, sizeof(pixel));

                    red   = expand_5_to_8((pixel >> 11) & 0x1f);
                    green = expand_6_to_8((pixel >> 5) & 0x3f);
                    blue  = expand_5_to_8(pixel & 0x1f);
                } break;

                case PixelFormat::BGR24_ByteArray: {
                    const auto* pixel = row + x * 3;
                    blue  = pixel[0];
                    green = pixel[1];
                    red   = pixel[2];
                } break;

                case PixelFormat::BGRX32_ByteArray: {
                    const auto* pixel = row + x * 4;
                    blue  = pixel[0];
                    green = pixel[1];
                    red   = pixel[2];
                } break;

                default:
                    break;
                }

                for (int xr = 0; xr < x_scale; ++xr) {
                    append_rgb(red, green, blue);
                }
            }
        }
    }

    return ppm;
}
// -------------------------------------------------------------------------

bool CAPTURE_IsCapturingAudio()
{
	return capture.state.audio != CaptureState::Off;
}

bool CAPTURE_IsCapturingImage()
{
	if (image_capturer) {
		return image_capturer->IsCaptureRequested();
	}
	return false;
}

bool CAPTURE_IsCapturingPostRenderImage()
{
	if (image_capturer) {
		return image_capturer->IsRenderedCaptureRequested();
	}
	return false;
}

bool CAPTURE_IsCapturingMidi()
{
	return capture.state.midi != CaptureState::Off;
}

bool CAPTURE_IsCapturingVideo()
{
	return capture.state.video != CaptureState::Off;
}

static const char* capture_type_to_string(const CaptureType type)
{
	switch (type) {
	case CaptureType::Audio: return "audio output";
	case CaptureType::Midi: return "MIDI output";
	case CaptureType::RawOplStream: return "rawl OPL output";
	case CaptureType::RadOplInstruments: return "RAD capture";

	case CaptureType::Video: return "video output";

	case CaptureType::RawImage: return "raw image";
	case CaptureType::UpscaledImage: return "upscaled image";
	case CaptureType::RenderedImage: return "rendered image";

	case CaptureType::SerialLog: return "serial log";

	default: assertm(false, "Unknown CaptureType"); return "";
	}
}

static const char* capture_type_to_basename(const CaptureType type)
{
	switch (type) {
	case CaptureType::Audio: return "audio";
	case CaptureType::Midi: return "midi";
	case CaptureType::RawOplStream: return "rawopl";
	case CaptureType::RadOplInstruments: return "oplinstr";

	case CaptureType::Video: return "video";

	case CaptureType::RawImage:
	case CaptureType::UpscaledImage:
	case CaptureType::RenderedImage: return "image";

	case CaptureType::SerialLog: return "serial";

	default: assertm(false, "Unknown CaptureType"); return "";
	}
}

static const char* capture_type_to_extension(const CaptureType type)
{
	switch (type) {
	case CaptureType::Audio: return ".wav";
	case CaptureType::Midi: return ".mid";
	case CaptureType::RawOplStream: return ".dro";
	case CaptureType::RadOplInstruments: return ".rad";

	case CaptureType::Video: return ".avi";

	case CaptureType::RawImage:
	case CaptureType::UpscaledImage:
	case CaptureType::RenderedImage: return ".png";

	case CaptureType::SerialLog: return ".serlog";

	default: assertm(false, "Unknown CaptureType"); return "";
	}
}

static const char* capture_type_to_postfix(const CaptureType type)
{
	switch (type) {
	case CaptureType::RawImage: return "-raw";
	case CaptureType::RenderedImage: return "-rendered";
	default: return "";
	}
}

static bool create_capture_directory()
{
	std::error_code ec = {};

	if (!std_fs::exists(capture.path, ec)) {
		if (!std_fs::create_directory(capture.path, ec)) {
			LOG_WARNING("CAPTURE: Can't create directory '%s': %s",
			            capture.path.string().c_str(),
			            ec.message().c_str());
			return false;
		}
		std_fs::permissions(capture.path, std_fs::perms::owner_all, ec);
	}
	return true;
}

static std::optional<int32_t> find_highest_capture_index(const CaptureType type)
{
	// Find existing capture file with the highest index
	std::string filename_start = capture_type_to_basename(type);
	lowcase(filename_start);

	int32_t highest_index = 0;
	std::error_code ec    = {};

	for (const auto& entry : std_fs::directory_iterator(capture.path, ec)) {
		if (ec) {
			LOG_WARNING("CAPTURE: Cannot open directory '%s': %s",
			            capture.path.string().c_str(),
			            ec.message().c_str());
			return {};
		}
		if (!entry.is_regular_file(ec)) {
			continue;
		}
		auto stem = entry.path().stem().string();
		lowcase(stem);

		if (stem.starts_with(filename_start)) {
			auto index_str = strip_prefix(stem, filename_start);

			// Strip "-raw" or "-rendered" postfix if it's there
			if (const auto dash_pos = index_str.find('-');
			    dash_pos != std::string::npos) {
				index_str = index_str.substr(0, dash_pos);
			}
			const auto index = parse_int(index_str);
			if (index) {
				highest_index = std::max(highest_index, *index);
			}
		}
	}
	return highest_index;
}

static void set_next_capture_index(const CaptureType type, int32_t index)
{
	switch (type) {
	case CaptureType::Audio: capture.next_index.audio = index; break;
	case CaptureType::Midi: capture.next_index.midi = index; break;

	case CaptureType::RawOplStream:
		capture.next_index.raw_opl_stream = index;
		break;

	case CaptureType::RadOplInstruments:
		capture.next_index.rad_opl_instrument = index;
		break;

	case CaptureType::Video: capture.next_index.video = index; break;

	case CaptureType::RawImage:
	case CaptureType::UpscaledImage:
	case CaptureType::RenderedImage:
		capture.next_index.image = index;
		break;

	case CaptureType::SerialLog:
		capture.next_index.serial_log = index;
		break;

	default: assertm(false, "Unknown CaptureType");
	}
}

static bool maybe_create_capture_dir_and_init_capture_indices()
{
	static std::mutex mutex = {};
	std::lock_guard<std::mutex> lock(mutex);

	if (capture.path_initialised) {
		return true;
	}
	if (!create_capture_directory()) {
		return false;
	}

	constexpr CaptureType all_capture_types[] = {CaptureType::Audio,
	                                             CaptureType::Midi,
	                                             CaptureType::RawOplStream,
	                                             CaptureType::RadOplInstruments,
	                                             CaptureType::Video,
	                                             CaptureType::RawImage,
	                                             CaptureType::UpscaledImage,
	                                             CaptureType::RenderedImage,
	                                             CaptureType::SerialLog};

	for (auto type : all_capture_types) {
		const auto index = find_highest_capture_index(type);
		if (index) {
			set_next_capture_index(type, *index + 1);
		} else {
			return false;
		}
	}

	capture.path_initialised = true;

	return true;
}

int32_t get_next_capture_index(const CaptureType type)
{
	if (!maybe_create_capture_dir_and_init_capture_indices()) {
		return 0;
	}

	switch (type) {
	case CaptureType::Audio: return capture.next_index.audio++;
	case CaptureType::Midi: return capture.next_index.midi++;

	case CaptureType::RawOplStream:
		return capture.next_index.raw_opl_stream++;

	case CaptureType::RadOplInstruments:
		return capture.next_index.rad_opl_instrument++;

	case CaptureType::Video: return capture.next_index.video++;

	case CaptureType::RawImage:
	case CaptureType::UpscaledImage:
	case CaptureType::RenderedImage: return capture.next_index.image++;

	case CaptureType::SerialLog: return capture.next_index.serial_log++;

	default: assertm(false, "Unknown CaptureType"); return 0;
	}
}

std_fs::path generate_capture_filename(const CaptureType type, const int32_t index)
{
	const auto filename = format_str("%s%04d%s%s",
	                                    capture_type_to_basename(type),
	                                    index,
	                                    capture_type_to_postfix(type),
	                                    capture_type_to_extension(type));
	return {capture.path / filename};
}

FILE* CAPTURE_CreateFile(const CaptureType type,
                         const std::optional<std_fs::path>& path)
{
	if (!maybe_create_capture_dir_and_init_capture_indices()) {
		return nullptr;
	}

	std::string path_str = {};
	if (path) {
		path_str = path->string();
	} else {
		const auto index = get_next_capture_index(type);
		path_str = generate_capture_filename(type, index).string();
	}

	FILE* handle = open_file(path_str.c_str(), "wb");
	if (handle) {
		LOG_MSG("CAPTURE: Capturing %s to '%s'",
		        capture_type_to_string(type),
		        path_str.c_str());
	} else {
		LOG_WARNING("CAPTURE: Failed to create file '%s' for capturing %s",
		            path_str.c_str(),
		            capture_type_to_string(type));
	}
	return handle;
}

void CAPTURE_StartVideoCapture()
{
	switch (capture.state.video) {
	case CaptureState::Off:
		capture.state.video = CaptureState::Pending;
		TITLEBAR_NotifyVideoCaptureStatus(true);
		break;
	case CaptureState::Pending:
	case CaptureState::InProgress:
		LOG_WARNING("CAPTURE: Already capturing video output");
		break;
	}
}

void CAPTURE_StopVideoCapture()
{
	switch (capture.state.video) {
	case CaptureState::Off:
		LOG_WARNING("CAPTURE: Not capturing video output");
		break;
	case CaptureState::Pending:
		// It's very hard to hit this branch; handling it for
		// completeness only
		LOG_MSG("CAPTURE: Cancelling pending video output capture");
		capture.state.video = CaptureState::Off;
		TITLEBAR_NotifyVideoCaptureStatus(false);
		break;
	case CaptureState::InProgress:
		capture_video_finalise();
		capture.state.video = CaptureState::Off;
		TITLEBAR_NotifyVideoCaptureStatus(false);
		LOG_MSG("CAPTURE: Stopped capturing video output");
	}
}

void CAPTURE_AddFrame(const RenderedImage& image, const float frames_per_second)
{
	update_live_frame(image);

	if (image_capturer) {
		image_capturer->MaybeCaptureImage(image);
	}

	switch (capture.state.video) {
	case CaptureState::Off: break;
	case CaptureState::Pending:
		capture.state.video = CaptureState::InProgress;
		[[fallthrough]];
	case CaptureState::InProgress:
		capture_video_add_frame(image, frames_per_second);
		break;
	}
}

void CAPTURE_AddPostRenderImage(const RenderedImage& image)
{
	if (image_capturer) {
		image_capturer->CapturePostRenderImage(image);
	}
}

void CAPTURE_AddAudioData(const uint32_t sample_rate, const uint32_t num_sample_frames,
                          const int16_t* sample_frames)
{
	switch (capture.state.video) {
	case CaptureState::Off: break;
	case CaptureState::Pending:
		capture.state.video = CaptureState::InProgress;
		[[fallthrough]];
	case CaptureState::InProgress:
		capture_video_add_audio_data(sample_rate,
		                             num_sample_frames,
		                             sample_frames);
		break;
	}

	switch (capture.state.audio) {
	case CaptureState::Off: break;
	case CaptureState::Pending:
		capture.state.audio = CaptureState::InProgress;
		[[fallthrough]];
	case CaptureState::InProgress:
		capture_audio_add_data(sample_rate, num_sample_frames, sample_frames);
		break;
	}
}

void CAPTURE_AddMidiData(const bool sysex, const size_t len, const uint8_t* data)
{
	if (capture.state.midi == CaptureState::Pending) {
		capture.state.midi = CaptureState::InProgress;
	}
	capture_midi_add_data(sysex, len, data);
}

static void handle_capture_audio_event(bool pressed)
{
	// Ignore key-release events
	if (!pressed) {
		return;
	}

	switch (capture.state.audio) {
	case CaptureState::Off:
		// Capturing the audio output will start in the next few
		// milliseconds when CAPTURE_AddAudioData is called
		capture.state.audio = CaptureState::Pending;
		break;
	case CaptureState::Pending:
		// It's practically impossible to hit this branch; handling it
		// for completeness only
		capture.state.audio = CaptureState::Off;
		LOG_MSG("CAPTURE: Cancelled pending audio output capture");
		break;
	case CaptureState::InProgress:
		capture_audio_finalise();
		capture.state.audio = CaptureState::Off;
		LOG_MSG("CAPTURE: Stopped capturing audio output");
		break;
	}
}

static void handle_capture_midi_event(bool pressed)
{
	// Ignore key-release events
	if (!pressed) {
		return;
	}

	switch (capture.state.midi) {
	case CaptureState::Off:
		capture.state.midi = CaptureState::Pending;

		// We need to log this because the actual sending of MIDI data
		// might happen much later
		LOG_MSG("CAPTURE: Preparing to capture MIDI output; "
		        "capturing will start on the first MIDI message");
		break;
	case CaptureState::Pending:
		capture.state.midi = CaptureState::Off;
		LOG_MSG("CAPTURE: Stopped capturing MIDI output before any "
		        "MIDI message was output (no MIDI file has been created)");
		break;
	case CaptureState::InProgress:
		capture_midi_finalise();
		capture.state.midi = CaptureState::Off;
		LOG_MSG("CAPTURE: Stopped capturing MIDI output");
		break;
	}
}

static void handle_capture_grouped_screenshot_event(const bool pressed)
{
	// Ignore key-release events
	if (!pressed) {
		return;
	}
	if (image_capturer) {
		image_capturer->RequestGroupedCapture();
	}
}

static void handle_capture_single_raw_screenshot_event(const bool pressed)
{
	// Ignore key-release events
	if (!pressed) {
		return;
	}
	if (image_capturer) {
		image_capturer->RequestRawCapture();
	}
}

static void handle_capture_single_upscaled_screenshot_event(const bool pressed)
{
	// Ignore key-release events
	if (!pressed) {
		return;
	}
	if (image_capturer) {
		image_capturer->RequestUpscaledCapture();
	}
}

static void handle_capture_single_rendered_screenshot_event(const bool pressed)
{
	// Ignore key-release events
	if (!pressed) {
		return;
	}
	if (image_capturer) {
		image_capturer->RequestRenderedCapture();
	}
}

static void handle_capture_video_event(bool pressed)
{
	// Ignore key-release events
	if (!pressed) {
		return;
	}
	if (capture.state.video != CaptureState::Off) {
		CAPTURE_StopVideoCapture();
	} else if (capture.state.video == CaptureState::Off) {
		CAPTURE_StartVideoCapture();
	}
}

void CAPTURE_Init()
{
	const auto section = get_section("capture");

	auto capture_path = section->GetPath("capture_dir");

	// We can safely change the capture output path even if capturing of any
	// type is in progress.
	capture.path = capture_path->realpath;
	if (capture.path.empty()) {
		LOG_WARNING(
		        "CAPTURE: No value specified for `capture_dir`; "
		        "defaulting to 'capture' in the current working directory");
		capture.path = "capture";
	}

	const auto prefs = section->GetString("default_image_capture_formats");

	image_capturer = std::make_unique<ImageCapturer>(prefs);
}

void CAPTURE_Destroy()
{
	if (capture.state.audio == CaptureState::InProgress) {
		capture_audio_finalise();
		capture.state.audio = CaptureState::Off;
	}

	if (capture.state.midi == CaptureState::InProgress) {
		capture_midi_finalise();
		capture.state.midi = CaptureState::Off;
	}

	// When destructed, the threaded image capturer instances do a blocking
	// wait until all pending capture tasks are processed.
	image_capturer = {};

	if (capture.state.video == CaptureState::InProgress) {
		capture_video_finalise();
		capture.state.video = CaptureState::Off;
	}

	capture.reset();
}

static void notify_capture_setting_updated([[maybe_unused]] SectionProp& section,
                                           [[maybe_unused]] const std::string& prop_name)
{
	CAPTURE_Destroy();
	CAPTURE_Init();
}

static void init_key_mappings()
{
	MAPPER_AddHandler(handle_capture_audio_event,
	                  SDL_SCANCODE_F6,
	                  PRIMARY_MOD,
	                  "recwave",
	                  "Rec. Audio");

	MAPPER_AddHandler(handle_capture_midi_event,
	                  SDL_SCANCODE_F6,
	                  PRIMARY_MOD | MMOD2,
	                  "caprawmidi",
	                  "Rec. MIDI");

	MAPPER_AddHandler(handle_capture_grouped_screenshot_event,
	                  SDL_SCANCODE_F5,
	                  PRIMARY_MOD,
	                  "scrshot",
	                  "Screenshot");

	MAPPER_AddHandler(handle_capture_single_raw_screenshot_event,
	                  SDL_SCANCODE_UNKNOWN,
	                  PRIMARY_MOD,
	                  "rawshot",
	                  "Raw Scrnshot");

	MAPPER_AddHandler(handle_capture_single_upscaled_screenshot_event,
	                  SDL_SCANCODE_UNKNOWN,
	                  PRIMARY_MOD,
	                  "upscshot",
	                  "Upsc Scrnshot");

	MAPPER_AddHandler(handle_capture_single_rendered_screenshot_event,
	                  SDL_SCANCODE_F5,
	                  MMOD2,
	                  "rendshot",
	                  "Rend Scrnshot");

	MAPPER_AddHandler(handle_capture_video_event,
	                  SDL_SCANCODE_F7,
	                  PRIMARY_MOD,
	                  "video",
	                  "Rec. Video");
}

static void init_capture_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto path_prop = section.AddPath("capture_dir", WhenIdle, "capture");
	path_prop->SetHelp(
	        "Directory where the various captures are saved, such as audio, video, MIDI\n"
	        "and screenshot captures ('capture' in the current working directory by\n"
	        "default).");

	auto* str_prop = section.AddString("default_image_capture_formats",
	                                   WhenIdle,
	                                   "upscaled");
	str_prop->SetHelp(
	        "Set the capture format of the default screenshot action ('upscaled' by\n"
	        "default). Possible values:\n"
	        "\n"
	        "  upscaled:  The image is bilinear-sharp upscaled and the correct aspect\n"
	        "             ratio is maintained, depending on the 'aspect' setting. The\n"
	        "             vertical scaling factor is always an integer. For example,\n"
	        "             320x200 content is upscaled to 1600x1200 (5:6 integer scaling),\n"
	        "             640x480 to 1920x1440 (3:3 integer scaling), and 640x350 to\n"
	        "             1400x1050 (2.1875:3 scaling; fractional horizontally and\n"
	        "             integer vertically). The filenames of upscaled screenshots\n"
	        "             have no postfix (e.g. 'image0001.png').\n"
	        "\n"
	        "  rendered:  The post-rendered, post-shader image shown on the screen is\n"
	        "             captured. The filenames of rendered screenshots end with\n"
	        "             '-rendered' (e.g. 'image0001-rendered.png').\n"
	        "\n"
	        "  raw:       The contents of the raw framebuffer is captured (this always\n"
	        "             results in square pixels). The filenames of raw screenshots\n"
	        "             end with '-raw' (e.g. 'image0001-raw.png').\n"
	        "\n"
	        "If multiple formats are specified separated by spaces, the default\n"
	        "screenshot action will save multiple images in the specified formats.\n"
	        "Keybindings for taking single screenshots in specific formats are also\n"
	        "available.");
}

void CAPTURE_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	auto section = conf->AddSection("capture");
	section->AddUpdateHandler(notify_capture_setting_updated);

	init_capture_config_settings(*section);
	init_key_mappings();
}
