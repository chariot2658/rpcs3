// SPDX-License-Identifier: GPL-2.0-or-later
#include "ThrustmasterT500RS.h"

namespace t500rs
{
namespace
{
std::uint16_t le16(const byte* p)
{
	return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
int signed_byte(byte b)
{
	return b < 128 ? b : static_cast<int>(b) - 256;
}
int signed_word(const byte* p)
{
	const int v = le16(p);
	return v < 32768 ? v : v - 65536;
}
std::int16_t force(int value, int maximum)
{
	return static_cast<std::int16_t>(std::clamp(value * 32767 / maximum, -32767, 32767));
}
void put16(input_report& out, std::size_t offset, std::uint16_t value)
{
	out[offset] = static_cast<byte>(value);
	out[offset + 1] = static_cast<byte>(value >> 8);
}
}

input_report make_input_report(const input& state)
{
	input_report out{};
	out[0] = 7;
	put16(out, 1, state.steering);
	// GT5's driving consumer reads channel 1 before channel 0. The user's
	// driving test confirms brake at 3..4 and throttle at 5..6.
	put16(out, 3, std::min<std::uint16_t>(state.brake, 1023));
	put16(out, 5, std::min<std::uint16_t>(state.throttle, 1023));
	put16(out, 7, std::min<std::uint16_t>(state.clutch, 1023));
	put16(out, 11, state.buttons & 0x1fff);
	out[14] = state.hat < 8 ? state.hat : 15;
	return out;
}

std::span<const byte> vendor_reply(byte request)
{
	// T500 model identification returned for vendor request 0x47.
	// Captured by hid-tminit from the wheel's generic boot personality.
	static constexpr std::array<byte, 8> model{0x47,0x00,0x03,0x00,0x00,0x00,0x02,0x00};
	static constexpr std::array<byte, 16> capabilities{0x49,0,0,0,1,0,2,0,3,0,0,0,2,2,0,0};
	static constexpr std::array<byte, 4> firmware{0x56,0,0x2f,0};
	static constexpr std::array<byte, 16> status{0x55};
	static constexpr std::array<byte, 64> settings{0x48};
	// GT5 reads capacity at bytes 1..2, then clamps the slot count to capacity/32.
	// Preserve the captured capacity, but advertise only slots we implement.
	static constexpr std::array<byte, 8> capacity{0x42,0xe8,3};
	static constexpr std::array<byte, 8> slots{0x4e,effect_slot_count};
	switch (request)
	{
	case 0x47: return model;
	case 0x49: return capabilities;
	case 0x56: return firmware;
	case 0x55: return status;
	case 0x48: return settings;
	case 0x42: return capacity;
	case 0x4e: return slots;
	default: return {};
	}
}

void protocol::stop_all()
{
	for (auto& s : effect_slots)
		s.playing = false;
	direct_slot.playing = false;
	autocenter_enabled = false;
}

void protocol::reset()
{
	const auto serial = m_serial + 1;
	*this = protocol(format);
	m_serial = serial;
}

bool protocol::queue_reply(const input_report& report)
{
	if (m_reply_count == m_replies.size())
		return false;
	m_replies[(m_reply_head + m_reply_count++) % m_replies.size()] = report;
	return true;
}

bool protocol::pop_reply(input_report& report)
{
	if (!m_reply_count)
		return false;
	report = m_replies[m_reply_head];
	m_reply_head = (m_reply_head + 1) % m_replies.size();
	--m_reply_count;
	return true;
}

result protocol::output(std::span<const byte> p, std::uint64_t now_us)
{
	if (p.empty() || p.size() > 32)
		return result::malformed;
	std::size_t required = 0;
	switch (p[0])
	{
	case 0x01: required = 15; break;
	case 0x02: required = 9; break;
	case 0x03: required = 4; break;
	case 0x04: required = 8; break;
	case 0x05: required = 11; break;
	case 0x0a: required = 4; break;
	case 0x40: case 0x41: required = 4; break;
	case 0x42: case 0x43: required = 2; break;
	case 0x81: required = 3; break;
	default: return result::unsupported;
	}
	if (p.size() < required)
		return result::malformed;

	switch (p[0])
	{
	case 0x01:
	{
		if (p[1] >= effect_slots.size())
			return result::malformed;
		const bool ps3 = p[2] == 1 || p[2] == 7 || p[2] == 8;
		if (ps3)
		{
			// GT5: flags 00 for initial declarations, 40 for active effects.
			// Byte 6 is direction, 7..8 repeat interval, 13..14 start delay.
			// Only the single-axis, non-repeating form is implemented here.
			if ((p[3] != 0 && p[3] != 0x40) || p[6] != 0 || (le16(&p[7]) != 0 && le16(&p[7]) != 0xffff))
				return result::unsupported;
		}
		else if (p[3] != 0x40)
			return result::malformed;
		const bool periodic = p[2] >= 0x20 && p[2] <= 0x24;
		if (!ps3 && p[2] != 0 && p[2] != 0x40 && p[2] != 0x41 && !periodic)
			return result::unsupported;
		if (format == dialect::linux_reference && periodic && (p[2] != 0x22 || p[1] != 0 || le16(&p[9]) != 0x0e))
			return result::unsupported;
		// PC parameter commands wrap to the low byte; PS3 uses the full reference.
		const std::uint16_t parameter = ps3 ? le16(&p[9]) : static_cast<byte>(le16(&p[9]));
		const std::uint16_t envelope = ps3 ? le16(&p[11]) : static_cast<byte>(le16(&p[11]));
		if (parameter == envelope || parameter >= m_parameters.size() || envelope >= m_envelopes.size())
			return result::malformed;
		m_ps3 |= ps3;
		auto& s = effect_slots[p[1]];
		const bool changed = !s.declared || s.ps3 != ps3 || s.type != p[2] || s.parameter != parameter || s.envelope != envelope;
		s.declared = true;
		s.ps3 = ps3;
		s.type = p[2];
		s.parameter = parameter;
		s.envelope = envelope;
		s.duration = le16(&p[4]);
		s.delay = le16(&p[ps3 ? 13 : format == dialect::linux_reference ? 6 : 7]);
		if (s.delay == 0xffff)
			s.delay = 0;
		if (changed)
		{
			s.playing = false;
			s.generation = ++m_serial;
		}
		return result::ok;
	}
	case 0x02:
		if (m_ps3 && le16(&p[1]) >= m_envelopes.size())
			return result::malformed;
		if (!m_ps3)
			std::copy_n(p.begin(), 9, m_envelopes[p[1]].begin());
		// Also stage the full reference before the first PS3 MAIN identifies
		// the dialect. Legacy Linux uses byte 2 for attack length instead.
		if (le16(&p[1]) < m_envelopes.size())
			std::copy_n(p.begin(), 9, m_envelopes[le16(&p[1])].begin());
		return result::ok;
	case 0x03: case 0x04: case 0x05:
	{
		if (!m_ps3 && format == dialect::linux_reference && p[0] == 4 && (p[1] != 0x0e || le16(&p[6]) != 0x2710))
			return result::unsupported;
		if (m_ps3 && le16(&p[1]) >= m_parameters.size())
			return result::malformed;
		auto& param = m_parameters[m_ps3 ? le16(&p[1]) : p[1]];
		param = {};
		param.type = p[0];
		std::copy_n(p.begin(), required, param.data.begin());
		if (!m_ps3 && le16(&p[1]) < m_parameters.size())
			m_parameters[le16(&p[1])] = param;
		return result::ok;
	}
	case 0x0a:
	{
		if (p[1] != 4)
			return result::unsupported;
		input_report reply{0x14,0x20,p[2],p[3]};
		switch (le16(&p[2]))
		{
		case 0x0390: reply[4]=0xaf; reply[5]=0xa7; reply[6]=0x2e; reply[7]=0x14; break;
		case 0x1012: reply[4]=0; reply[5]=0x2f; reply[6]=0x5e; reply[7]=0xb6; break;
		case 0x0600: reply[4]=0x18; break;
		default: return result::unsupported;
		}
		if (!queue_reply(reply))
			return result::malformed;
		last_identification = reply;
		return result::ok;
	}
	case 0x40:
		switch (p[1])
		{
		case 0x03: autocenter_strength = std::min<byte>(p[2], 100); return result::ok;
		case 0x04: autocenter_enabled = p[2] != 0; return result::ok;
		case 0x11:
			// GT5 encodes degrees * 65535 / 1080 (0x00ADF30C). Round the
			// inverse to recover integer degrees despite the guest's truncation.
			range = static_cast<std::uint16_t>(std::clamp((le16(&p[2]) * 1080u + 32767u) / 65535u, 40u, 1080u));
			return result::ok;
		default: return result::unsupported;
		}
	case 0x41:
	{
		if (p[1] >= effect_slots.size())
			return result::malformed;
		auto& s = effect_slots[p[1]];
		if (p[2] == 0)
		{
			s.playing = false;
			if (p[1] == 15)
				autocenter_enabled = false;
		}
		else if (p[2] == (s.ps3 ? 1 : 0x41))
		{
			if (!s.declared)
				return result::unsupported;
			if (s.ps3 && p[3] != 1)
				return result::unsupported;
			s.playing = true;
			s.started_at_us = now_us;
			s.starts = ++m_serial;
		}
		else
			return result::unsupported;
		return result::ok;
	}
	case 0x42:
		switch (p[1])
		{
		case 1: reset(); return result::ok;
		case 0: case 4: case 5: return result::ok; // Observed initialization/apply fences.
		default: return result::unsupported;
		}
	case 0x43: gain = p[1]; return result::ok;
	case 0x81:
	{
		// GT5 FORCE_X/Y, signed -127..127 (builder 0x00AE1EE0).
		// A steering wheel renders only X; Y has no physical axis.
		m_ps3 = true;
		m_direct_level = force(signed_byte(p[1]), 127);
		if (!direct_slot.declared)
		{
			direct_slot.declared = true;
			direct_slot.generation = ++m_serial;
		}
		if (m_direct_level && !direct_slot.playing)
		{
			direct_slot.starts = ++m_serial;
			direct_slot.started_at_us = now_us;
		}
		direct_slot.playing = m_direct_level != 0;
		return result::ok;
	}
	default: return result::unsupported;
	}
}

effect protocol::decode(std::size_t index, bool reverse) const
{
	effect out{};
	if (index == effect_slots.size() && direct_slot.declared)
	{
		out.kind = effect_kind::constant;
		out.level = reverse ? -m_direct_level : m_direct_level;
		return out;
	}
	if (index >= effect_slots.size() || !effect_slots[index].declared)
		return out;
	const auto& s = effect_slots[index];
	const auto& param = m_parameters[s.parameter];
	const auto& p = param.data;
	const auto& env = m_envelopes[s.envelope];
	const int sign = reverse ? -1 : 1;
	if (s.ps3)
	{
		// Recovered GT5 wrappers normalize +/-10000 into these wire ranges.
		// Zero-duration initialization declarations must never create torque.
		if (!s.duration)
			return out;
		out.length = s.duration == 0xffff ? 0xffffffffu : s.duration;
		out.delay = s.delay;
		if (s.type == 1 && param.type == 3)
		{
			out.kind = effect_kind::constant;
			out.level = force(signed_byte(p[3]) * sign, 127);
			if (env[0] == 2)
			{
				out.attack_length = le16(&env[3]);
				out.attack_level = force(std::min<int>(env[5], 127), 127);
				out.fade_length = le16(&env[6]);
				out.fade_level = force(std::min<int>(env[8], 127), 127);
			}
		}
		else if ((s.type == 7 || s.type == 8) && param.type == 5)
		{
			out.kind = s.type == 7 ? effect_kind::spring : effect_kind::damper;
			out.right_coeff = force(signed_byte(p[3]) * sign, 100);
			out.left_coeff = force(signed_byte(p[4]) * sign, 100);
			out.center = force(signed_word(&p[5]), 500);
			out.deadband = static_cast<std::uint16_t>(std::min<unsigned>(le16(&p[7]), 1000) * 65535 / 1000);
			out.right_sat = static_cast<std::uint16_t>(std::min<unsigned>(p[9], 100) * 65535 / 100);
			out.left_sat = static_cast<std::uint16_t>(std::min<unsigned>(p[10], 100) * 65535 / 100);
		}
		return out;
	}
	const bool reference_mode = format == dialect::linux_reference;
	const bool stream = param.type == 4 && s.type == 0x22 &&
		(reference_mode || (index == 0 && s.duration == 0xffff && p[3] == 0 && le16(&p[6]) == 0x2710));
	out.length = reference_mode || stream || s.duration == 0xffff || s.duration == 0 ? 0xffffffffu : s.duration;
	out.delay = s.delay;
	if (env[0] == 2 && !stream)
	{
		out.attack_length = le16(&env[reference_mode ? 2 : 3]);
		out.attack_level = static_cast<std::uint16_t>(force(std::min<int>(env[reference_mode ? 4 : 5], reference_mode ? 255 : 127), reference_mode ? 255 : 127));
		out.fade_length = le16(&env[reference_mode ? 5 : 6]);
		out.fade_level = static_cast<std::uint16_t>(force(std::min<int>(env[reference_mode ? 7 : 8], reference_mode ? 255 : 127), reference_mode ? 255 : 127));
	}
	if ((s.type == 0 && param.type == 3) || stream)
	{
		out.kind = effect_kind::constant;
		out.level = force(signed_byte(p[stream ? 4 : 3]) * sign, 127);
		return out;
	}
	if (s.type >= 0x20 && s.type <= 0x24 && param.type == 4 && !reference_mode)
	{
		constexpr std::array kinds{effect_kind::square, effect_kind::triangle, effect_kind::sine, effect_kind::saw_up, effect_kind::saw_down};
		out.kind = kinds[s.type - 0x20];
		out.magnitude = force(std::min<int>(p[3], 127) * sign, 127);
		out.offset = force(signed_byte(p[4]) * sign, 127);
		out.phase = static_cast<std::uint16_t>(p[5] * 36000 / 256);
		out.period = std::max<std::uint16_t>(le16(&p[6]), 1);
		return out;
	}
	if ((s.type == 0x40 || s.type == 0x41) && param.type == 5)
	{
		out.kind = s.type == 0x40 ? effect_kind::spring : effect_kind::damper;
		out.right_coeff = force((reference_mode ? std::min<int>(p[3], 10) : signed_byte(p[3])) * sign, reference_mode ? 10 : 127);
		out.left_coeff = force((reference_mode ? std::min<int>(p[4], 10) : signed_byte(p[4])) * sign, reference_mode ? 10 : 127);
		out.center = static_cast<std::int16_t>(std::clamp(signed_word(&p[5]) * 20, -32767, 32767));
		out.deadband = static_cast<std::uint16_t>(std::min<int>(le16(&p[7]) * 65, 65535));
		out.right_sat = static_cast<std::uint16_t>(std::min<int>(p[9], 100) * 65535 / 100);
		out.left_sat = static_cast<std::uint16_t>(std::min<int>(p[10], 100) * 65535 / 100);
	}
	return out;
}
}
