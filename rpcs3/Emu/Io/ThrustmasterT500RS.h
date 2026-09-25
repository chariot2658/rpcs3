// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Pure USB/protocol model: no SDL, RPCS3 globals or host-device calls.
// Sources and unresolved interpretations: docs/T500RS-emulation.md.
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace t500rs
{
using byte = std::uint8_t;

inline constexpr std::array<byte, 18> device_descriptor = {
	0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x4f, 0x04, 0x5e, 0xb6, 0x00, 0x01, 0x01, 0x02, 0x00, 0x01};
inline constexpr std::array<byte, 41> configuration_descriptor = {
	0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0xc0, 0x32,
	0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
	0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x8a, 0x00,
	0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x02,
	0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x04};
// GT5 copies 32 bytes on IN completion. Extend the captured input layouts with
// zero padding and advertise matching lengths; keep the captured OUT layout.
inline constexpr std::array<byte, 138> report_descriptor = {
	0x05,0x01,0x09,0x04,0xa1,0x01,0x09,0x01,0xa1,0x00,0x85,0x07,0x09,0x30,0x15,0x00,
	0x27,0xff,0xff,0x00,0x00,0x35,0x00,0x47,0xff,0xff,0x00,0x00,0x75,0x10,0x95,0x01,
	0x81,0x02,0x09,0x31,0x26,0xff,0x03,0x46,0xff,0x03,0x81,0x02,0x09,0x35,0x81,0x02,
	0x09,0x36,0x81,0x02,0x81,0x03,0x05,0x09,0x19,0x01,0x29,0x0d,0x25,0x01,0x45,0x01,
	0x75,0x01,0x95,0x0d,0x81,0x02,0x75,0x0b,0x95,0x01,0x81,0x03,0x05,0x01,0x09,0x39,
	0x25,0x07,0x46,0x3b,0x01,0x55,0x00,0x65,0x14,0x75,0x04,0x81,0x42,0x65,0x00,0x81,
	0x03,0x75,0x08,0x95,0x11,0x81,0x03,0x85,0x0a,0x06,0x00,0xff,0x09,0x0a,0x75,0x08,
	0x95,0x0e,0x26,0xff,0x00,0x46,0xff,0x00,0x91,0x02,0x85,0x02,0x09,0x02,0x95,0x1f,
	0x81,0x02,0x09,0x14,0x85,0x14,0x81,0x02,0xc0,0xc0};

inline constexpr byte effect_slot_count = 16;
using input_report = std::array<byte, 32>;
struct input
{
	std::uint16_t steering = 0x8000;
	std::uint16_t throttle = 1023;
	std::uint16_t brake = 1023;
	std::uint16_t clutch = 1023;
	std::uint16_t buttons = 0;
	byte hat = 15;
};
input_report make_input_report(const input& state);
// Capture-derived replies, with eight-byte GT5 queries and a slot count bounded
// to the emulator's storage. See docs/T500RS-emulation.md for synthetic fields.
std::span<const byte> vendor_reply(byte request);

// The PR's encoder and archived Windows captures disagree on several fields.
// Keep the interpretations explicit instead of guessing based on force values.
enum class dialect { windows_capture, linux_reference };
enum class result { ok, malformed, unsupported };
enum class effect_kind { none, constant, sine, square, triangle, saw_up, saw_down, spring, damper };

struct effect
{
	effect_kind kind = effect_kind::none;
	std::uint32_t length = 0xffffffffu;
	std::uint16_t delay = 0;
	std::int16_t level = 0;
	std::int16_t magnitude = 0;
	std::int16_t offset = 0;
	std::uint16_t period = 1;
	std::uint16_t phase = 0;
	std::uint16_t attack_length = 0;
	std::uint16_t attack_level = 0;
	std::uint16_t fade_length = 0;
	std::uint16_t fade_level = 0;
	std::int16_t right_coeff = 0;
	std::int16_t left_coeff = 0;
	std::int16_t center = 0;
	std::uint16_t deadband = 0;
	std::uint16_t right_sat = 0;
	std::uint16_t left_sat = 0;
	bool operator==(const effect&) const = default;
};

struct slot
{
	bool declared = false;
	bool playing = false;
	bool ps3 = false;
	byte type = 0;
	std::uint16_t parameter = 0;
	std::uint16_t envelope = 0;
	std::uint16_t duration = 0xffff;
	std::uint16_t delay = 0;
	// A serial preserves STOP/START and reset/redeclare transitions coalesced by the worker.
	std::uint64_t starts = 0;
	std::uint64_t generation = 0;
	std::uint64_t started_at_us = 0;
};

class protocol
{
public:
	explicit protocol(dialect format = dialect::windows_capture) : format(format) {}
	result output(std::span<const byte> packet, std::uint64_t now_us = 0);
	effect decode(std::size_t index, bool reverse = false) const;
	void stop_all();
	void reset();
	bool pop_reply(input_report& report);
	bool enable_input_reporting() { return queue_reply(input_report{2, 0xff, 0x3f}); }
	input_report last_identification{};
	std::array<slot, effect_slot_count> effect_slots{};
	// Opcode 81 is a separate persistent X/Y force, not a numbered effect.
	slot direct_slot{};
	bool ps3_active() const { return m_ps3; }
	unsigned gain_limit() const { return m_ps3 ? 128 : 255; }
	dialect format;
	byte gain = 255;
	bool autocenter_enabled = false;
	byte autocenter_strength = 0;
	std::uint16_t range = 1080;

private:
	struct parameters
	{
		std::array<byte, 11> data{};
		byte type = 0;
	};
	// PS3 uses full LE16 references, with two 16-byte channels per slot.
	std::array<parameters, effect_slot_count * 32> m_parameters{};
	std::array<std::array<byte, 9>, effect_slot_count * 32> m_envelopes{};
	bool m_ps3 = false;
	std::int16_t m_direct_level = 0;
	std::array<input_report, 16> m_replies{};
	std::size_t m_reply_head = 0;
	std::size_t m_reply_count = 0;
	std::uint64_t m_serial = 0;
	bool queue_reply(const input_report& report);
};
}
