// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"

#ifdef HAVE_SDL3
#include "LogitechG27.h"
#include "Emu/Cell/lv2/sys_usbd.h"
#include "Emu/System.h"
#include "Input/sdl_instance.h"

LOG_CHANNEL(t500rs_log, "T500RS");
extern bool is_input_allowed();

void usb_device_logitech_g27::init_t500rs()
{
	m_personality = logitech_personality::t500rs;
	const std::lock_guard lock(g_cfg_logitech_g27.m_mutex);
	m_t500rs = t500rs::protocol(g_cfg_logitech_g27.t500rs_protocol.get() ? t500rs::dialect::linux_reference : t500rs::dialect::windows_capture);
	const auto& raw = t500rs::device_descriptor;
	device = UsbDescriptorNode(raw[0], raw[1], raw.data() + 2);
	const auto& config = t500rs::configuration_descriptor;
	auto& conf = device.add_node(UsbDescriptorNode(config[0], config[1], config.data() + 2));
	for (std::size_t i = config[0]; i < config.size(); i += config[i])
		conf.add_node(UsbDescriptorNode(config[i], config[i + 1], config.data() + i + 2));
	add_string("Thrustmaster");
	add_string("TRS Racing wheel");
	t500rs_log.notice("Experimental T500RS emulation enabled (%s fallback); PS3 force-feedback protocol is detected automatically",
		g_cfg_logitech_g27.t500rs_protocol.get() ? "hid-tmff2" : "captured Windows");
}

void usb_device_logitech_g27::control_t500rs(u8 request_type, u8 request, u16 value, u16 index, u16 length, u32 size, u8* data, UsbTransfer* transfer)
{
	transfer->fake = true;
	transfer->expected_result = HC_CC_NOERR;
	transfer->expected_count = 0;
	transfer->expected_time = get_timestamp() + 100;
	const u32 available = data ? std::min<u32>(size, length) : 0;
	const auto reply = [&](std::span<const u8> bytes)
	{
		const u32 count = std::min<u32>(available, static_cast<u32>(bytes.size()));
		if (count) std::memcpy(data, bytes.data(), count);
		transfer->expected_count = count;
		t500rs_log.trace("Control reply type=%02x request=%02x value=%04x requested=%u buffer=%u returned=%u: %s",
			request_type, request, value, length, size, count, fmt::buf_to_hexstring(bytes.data(), count));
	};
	const auto reply_byte = [&](u8 b) { reply(std::span<const u8>(&b, 1)); };

	t500rs_log.trace("Control type=%02x request=%02x value=%04x index=%04x length=%u", request_type, request, value, index, length);
	if (request == LIBUSB_REQUEST_GET_DESCRIPTOR && (request_type == 0x80 || request_type == 0x81))
	{
		const u8 type = value >> 8;
		if ((value & 0xff) == 0 && type == USB_DESCRIPTOR_DEVICE && request_type == 0x80)
		{ reply(t500rs::device_descriptor); return; }
		if ((value & 0xff) == 0 && type == USB_DESCRIPTOR_CONFIG && request_type == 0x80)
		{ reply(t500rs::configuration_descriptor); return; }
		if ((value & 0xff) == 0 && type == 0x22 && index == 0)
		{ reply(t500rs::report_descriptor); return; }
		if ((value & 0xff) == 0 && type == 0x21 && index == 0)
		{ reply(std::span<const u8>(t500rs::configuration_descriptor).subspan(18, 9)); return; }
		if (type == USB_DESCRIPTOR_STRING && (value & 0xff) <= 2 && request_type == 0x80)
		{
			transfer->expected_count = get_descriptor(type, value & 0xff, data, std::min<u32>(available, 255));
			t500rs_log.trace("String descriptor reply requested=%u buffer=%u returned=%u: %s", length, size,
				transfer->expected_count, fmt::buf_to_hexstring(data, transfer->expected_count));
			return;
		}
	}
	else if (request_type == 0x00 && request == LIBUSB_REQUEST_SET_CONFIGURATION && index == 0 && value <= 1 && length == 0)
	{
		set_configuration(static_cast<u8>(value));
		// SET_CONFIGURATION starts a fresh USB session, including reselecting 1.
		const std::lock_guard lock(m_t500rs_mutex);
		m_t500rs.reset();
		return;
	}
	else if ((request_type == 0x01 || request_type == 0x00) && request == LIBUSB_REQUEST_SET_INTERFACE && value == 0 && index == 0 && length == 0)
	{ set_interface(0, 0); return; }
	else if (request_type == 0x80 && request == LIBUSB_REQUEST_GET_CONFIGURATION && !value && !index)
	{ reply_byte(current_config); return; }
	else if (request_type == 0x81 && request == LIBUSB_REQUEST_GET_INTERFACE && !value && !index)
	{ reply_byte(0); return; }
	else if ((request_type == 0x80 || request_type == 0x81 || request_type == 0x82) && request == LIBUSB_REQUEST_GET_STATUS && value == 0 &&
		(index == 0 || (request_type == 0x82 && (index == 0x82 || index == 1))))
	{
		const std::array<u8, 2> status{static_cast<u8>(request_type == 0x80 ? 1 : 0), 0};
		reply(status); return;
	}
	else if (request_type == 0x02 && request == LIBUSB_REQUEST_CLEAR_FEATURE && value == 0 && (index == 0x82 || index == 1) && length == 0)
	{ return; }
	else if (request_type == 0x00 && request == LIBUSB_REQUEST_SET_ADDRESS && value <= 127 && !index && !length)
	{ return; }
	else if (request_type == 0xc1 && index == 0 && value == 0)
	{
		const auto bytes = t500rs::vendor_reply(request);
		if (!bytes.empty()) { reply(bytes); return; }
	}
	else if (request_type == 0x41 && index == 0 && request == 0x48 && value == 0x40 && length == 0)
	{
		// Capture: switching input reporting is acknowledged by report 0x02.
		// GET_REPORT(2) also exposes this same status. Do not fabricate firmware writes.
		const std::lock_guard lock(m_t500rs_mutex);
		if (!m_t500rs.enable_input_reporting()) transfer->expected_result = EHCI_CC_HALTED;
		return;
	}
	else if (request_type == 0xa1 && index == 0)
	{
		const std::lock_guard lock(m_t500rs_mutex);
		if (request == 2 && (value >> 8) == 0) { reply_byte(m_t500rs_idle[value & 0xff]); return; } // GET_IDLE
		if (request == 3 && value == 0) { reply_byte(m_t500rs_hid_protocol); return; } // GET_PROTOCOL
		if (request == 1 && (value >> 8) == 1 && (value & 0xff) == 2)
		{ const t500rs::input_report status{2,0xff,0x3f}; reply(status); return; }
		if (request == 1 && (value >> 8) == 1 && (value & 0xff) == 0x14 && m_t500rs.last_identification[0] == 0x14)
		{ reply(m_t500rs.last_identification); return; }
	}
	else if (request_type == 0x21 && index == 0)
	{
		if (request == 0x0a && length == 0) // SET_IDLE
		{
			const std::lock_guard lock(m_t500rs_mutex);
			if ((value & 0xff) == 0) m_t500rs_idle.fill(value >> 8);
			else m_t500rs_idle[value & 0xff] = value >> 8;
			return;
		}
		if (request == 0x0b && value == 1 && length == 0) // Report protocol; this is not a boot HID.
		{ const std::lock_guard lock(m_t500rs_mutex); m_t500rs_hid_protocol = 1; return; }
		if (request == 9 && (value >> 8) == 2 && length <= 32 && available == length && available && data[0] == (value & 0xff))
		{
			if (output_t500rs(std::span<const u8>(data, available)) == t500rs::result::ok)
			{ transfer->expected_count = available; return; }
		}
	}
	// Input sampling takes its own locks; never call it while holding the protocol mutex.
	if (request_type == 0xa1 && index == 0 && request == 1 && value == 0x0107)
	{
		sdl_instance::get_instance().pump_events();
		reply(input_t500rs(available)); return;
	}
	transfer->expected_result = EHCI_CC_HALTED;
	const std::lock_guard lock(m_t500rs_mutex);
	const u32 key = 0x1000000u | (static_cast<u32>(request_type) << 8) | request;
	if (m_t500rs_warnings.insert(key).second)
		t500rs_log.warning("Unsupported control type=%02x request=%02x value=%04x index=%04x length=%u (enable T500RS trace for all requests)", request_type, request, value, index, length);
}

void usb_device_logitech_g27::trace_t500rs_input(const t500rs::input& state, const std::array<s16, 4>& axes, bool allowed, u16 range, u32 requested_size, const t500rs::input_report& report) const
{
	if (!t500rs_log.trace)
	{
		return;
	}
	// Called with m_sdl_handles_mutex held. Axis noise should not produce a log
	// entry at every USB poll; digital transitions should still be visible.
	const u64 now = get_timestamp();
	if (now < m_t500rs_next_input_trace && state.buttons == m_t500rs_trace_buttons && state.hat == m_t500rs_trace_hat &&
		allowed == m_t500rs_trace_allowed && requested_size == m_t500rs_trace_size)
	{
		return;
	}
	m_t500rs_next_input_trace = now + 100'000;
	m_t500rs_trace_buttons = state.buttons;
	m_t500rs_trace_hat = state.hat;
	m_t500rs_trace_allowed = allowed;
	m_t500rs_trace_size = requested_size;

	const auto present = [this](const sdl_mapping& mapping)
	{
		const auto it = m_joysticks.find(mapping.device_type_id);
		return it != m_joysticks.end() && !it->second.empty();
	};
	const u32 count = std::min<u32>(requested_size, static_cast<u32>(report.size()));
	t500rs_log.trace("Input sample: allowed=%d steering_present=%d mapped_axes=[%d,%d,%d,%d] axes_present=[%d,%d,%d,%d] buttons=%04x hat=%u host_range=%u guest_range=%u",
		allowed, !!m_steering_device_present, axes[0], axes[1], axes[2], axes[3],
		present(m_mapping.steering), present(m_mapping.throttle), present(m_mapping.brake), present(m_mapping.clutch),
		state.buttons, state.hat, m_t500rs_host_range, range);
	t500rs_log.trace("Input report: requested=%u returned=%u: %s", requested_size, count, fmt::buf_to_hexstring(report.data(), count));
	for (const auto& [device_type_id, joysticks] : m_joysticks)
	{
		for (usz device_index = 0; device_index < joysticks.size(); device_index++)
		{
			SDL_Joystick* joystick = joysticks[device_index];
			std::string raw_axes, pressed_buttons, hats;
			for (int i = 0; i < SDL_GetNumJoystickAxes(joystick); i++)
			{
				if (i)
					raw_axes += ',';
				raw_axes += std::to_string(SDL_GetJoystickAxis(joystick, i));
			}
			for (int i = 0; i < SDL_GetNumJoystickButtons(joystick); i++)
			{
				if (SDL_GetJoystickButton(joystick, i))
				{
					if (!pressed_buttons.empty())
						pressed_buttons += ',';
					pressed_buttons += std::to_string(i);
				}
			}
			for (int i = 0; i < SDL_GetNumJoystickHats(joystick); i++)
			{
				if (i)
					hats += ',';
				hats += std::to_string(SDL_GetJoystickHat(joystick, i));
			}
			t500rs_log.trace("SDL raw: device_type_id=%llu instance=%u axes=[%s] pressed_buttons=[%s] hats=[%s]",
				device_type_id, static_cast<u32>(device_index), raw_axes, pressed_buttons, hats);
		}
	}
}

t500rs::result usb_device_logitech_g27::output_t500rs(std::span<const u8> data)
{
	const std::lock_guard lock(m_t500rs_mutex);
	t500rs_log.trace("OUT: %s", fmt::buf_to_hexstring(data.data(), data.size()));
	const bool was_ps3 = m_t500rs.ps3_active();
	const auto result = m_t500rs.output(data, get_timestamp());
	if (!was_ps3 && m_t500rs.ps3_active())
		t500rs_log.notice("Detected PS3 force-feedback protocol: constant/spring/damper, 128-step gain");
	t500rs_log.trace("OUT result=%u (0=ok, 1=malformed, 2=unsupported)", static_cast<u32>(result));
	if (result != t500rs::result::ok)
	{
		const u32 key = 0x2000000u | (static_cast<u32>(data.empty() ? 0 : data[0]) << 8) | (data.size() > 1 ? data[1] : 0);
		if (m_t500rs_warnings.insert(key).second)
			t500rs_log.warning("Unsupported/malformed OUT: %s", fmt::buf_to_hexstring(data.data(), data.size()));
	}
	return result;
}

void usb_device_logitech_g27::interrupt_t500rs(u32 size, u8* data, u32 endpoint, UsbTransfer* transfer)
{
	transfer->fake = true;
	transfer->expected_result = HC_CC_NOERR;
	transfer->expected_count = 0;
	transfer->expected_time = get_timestamp() + (endpoint == 0x82 ? 2000 : 100);
	if (!data || !size || !current_config)
	{
		transfer->expected_result = EHCI_CC_HALTED;
		t500rs_log.trace("Interrupt halted: endpoint=%02x size=%u buffer_present=%d configuration=%u", endpoint, size, data != nullptr, current_config);
		return;
	}
	if (endpoint == 0x82)
	{
		t500rs::input_report report;
		bool pending;
		{
			const std::lock_guard lock(m_t500rs_mutex);
			pending = m_t500rs.pop_reply(report);
		}
		if (!pending)
		{
			sdl_instance::get_instance().pump_events();
			report = input_t500rs(size);
		}
		transfer->expected_count = std::min<u32>(size, static_cast<u32>(report.size()));
		std::memcpy(data, report.data(), transfer->expected_count);
		if (pending)
		{
			t500rs_log.trace("Queued IN reply: endpoint=%02x requested=%u returned=%u: %s", endpoint, size,
				transfer->expected_count, fmt::buf_to_hexstring(data, transfer->expected_count));
		}
	}
	else if (endpoint == 1 && size <= 32 && output_t500rs(std::span<const u8>(data, size)) == t500rs::result::ok)
		transfer->expected_count = size;
	else
		transfer->expected_result = EHCI_CC_HALTED;
}

void usb_device_logitech_g27::invalidate_t500rs_haptics()
{
	m_t500rs_host_slots = {};
	m_t500rs_gain = -1;
	m_t500rs_autocenter = -1;
	m_t500rs_autocenter_id = -1;
	m_t500rs_muted = false;
}

namespace
{
SDL_HapticEffect make_effect(const t500rs::effect& e, SDL_HapticDirection direction)
{
	SDL_HapticEffect out{};
	switch (e.kind)
	{
	case t500rs::effect_kind::constant:
		out.type = SDL_HAPTIC_CONSTANT;
		out.constant.direction = direction;
		out.constant.length = e.length;
		out.constant.delay = e.delay;
		out.constant.level = e.level;
		out.constant.attack_length = e.attack_length;
		out.constant.attack_level = e.attack_level;
		out.constant.fade_length = e.fade_length;
		out.constant.fade_level = e.fade_level;
		break;
	case t500rs::effect_kind::spring: case t500rs::effect_kind::damper:
		out.type = e.kind == t500rs::effect_kind::spring ? SDL_HAPTIC_SPRING : SDL_HAPTIC_DAMPER;
		out.condition.direction = direction;
		out.condition.length = e.length;
		out.condition.delay = e.delay;
		out.condition.right_coeff[0] = e.right_coeff;
		out.condition.left_coeff[0] = e.left_coeff;
		out.condition.right_sat[0] = e.right_sat;
		out.condition.left_sat[0] = e.left_sat;
		out.condition.center[0] = e.center;
		out.condition.deadband[0] = e.deadband;
		break;
	case t500rs::effect_kind::sine: out.type = SDL_HAPTIC_SINE; break;
	case t500rs::effect_kind::square: out.type = SDL_HAPTIC_SQUARE; break;
	case t500rs::effect_kind::triangle: out.type = SDL_HAPTIC_TRIANGLE; break;
	case t500rs::effect_kind::saw_up: out.type = SDL_HAPTIC_SAWTOOTHUP; break;
	case t500rs::effect_kind::saw_down: out.type = SDL_HAPTIC_SAWTOOTHDOWN; break;
	case t500rs::effect_kind::none: return out;
	}
	if (e.kind >= t500rs::effect_kind::sine && e.kind <= t500rs::effect_kind::saw_down)
	{
		out.periodic.direction = direction;
		out.periodic.length = e.length;
		out.periodic.delay = e.delay;
		out.periodic.period = e.period;
		out.periodic.magnitude = e.magnitude;
		out.periodic.offset = e.offset;
		out.periodic.phase = e.phase;
		out.periodic.attack_length = e.attack_length;
		out.periodic.attack_level = e.attack_level;
		out.periodic.fade_length = e.fade_length;
		out.periodic.fade_level = e.fade_level;
	}
	return out;
}
}

void usb_device_logitech_g27::update_t500rs_haptics()
{
	t500rs::protocol snapshot;
	{
		const std::lock_guard lock(m_t500rs_mutex);
		snapshot = m_t500rs;
	}
	const std::lock_guard lock(m_sdl_handles_mutex);
	if (!m_haptic_handle)
	{
		if (!m_t500rs_missing_haptic_warned)
			t500rs_log.warning("No SDL haptic device is open; T500RS input works but host force feedback is unavailable");
		m_t500rs_missing_haptic_warned = true;
		return;
	}
	m_t500rs_missing_haptic_warned = false;
	const bool muted = !m_steering_device_present || !is_input_allowed() || Emu.IsPaused() || Emu.IsStopped();
	if (muted)
	{
		if (!m_t500rs_muted)
		{
			SDL_StopHapticEffects(m_haptic_handle);
			SDL_SetHapticAutocenter(m_haptic_handle, 0);
			for (auto& h : m_t500rs_host_slots) h.playing = false;
			m_t500rs_autocenter = -1;
		}
		m_t500rs_muted = true;
		return;
	}
	m_t500rs_muted = false;
	// Apply global gain in software: works even when the host lacks SDL_HAPTIC_GAIN.
	// Reset host gain once to avoid unintentionally multiplying an inherited value.
	if (m_t500rs_gain < 0)
	{
		t500rs_log.notice("Host FFB ready: SDL features=0x%x direction=%u reverse=%d",
			SDL_GetHapticFeatures(m_haptic_handle), m_t500rs_direction.type, m_reverse_effects);
		if (SDL_GetHapticFeatures(m_haptic_handle) & SDL_HAPTIC_GAIN)
			SDL_SetHapticGain(m_haptic_handle, 100);
		m_t500rs_gain = 100;
	}
	const unsigned gain_limit = snapshot.gain_limit();
	const unsigned gain = std::min<unsigned>(snapshot.gain, gain_limit);
	const int autocenter = snapshot.autocenter_enabled ? snapshot.autocenter_strength * gain / gain_limit : 0;
	if (autocenter != m_t500rs_autocenter)
	{
		if (SDL_GetHapticFeatures(m_haptic_handle) & SDL_HAPTIC_AUTOCENTER)
		{
			if (!SDL_SetHapticAutocenter(m_haptic_handle, autocenter))
				t500rs_log.warning("Host autocenter failed: %s", SDL_GetError());
		}
		else if (autocenter)
		{
			SDL_HapticEffect spring{};
			spring.type = SDL_HAPTIC_SPRING;
			spring.condition.direction = m_t500rs_direction;
			spring.condition.length = SDL_HAPTIC_INFINITY;
			spring.condition.right_coeff[0] = spring.condition.left_coeff[0] = static_cast<s16>(autocenter * 32767 / 100);
			spring.condition.right_sat[0] = spring.condition.left_sat[0] = 65535;
			if (m_t500rs_autocenter_id < 0)
				m_t500rs_autocenter_id = SDL_CreateHapticEffect(m_haptic_handle, &spring);
			else
				SDL_UpdateHapticEffect(m_haptic_handle, m_t500rs_autocenter_id, &spring);
			if (m_t500rs_autocenter_id >= 0)
				SDL_RunHapticEffect(m_haptic_handle, m_t500rs_autocenter_id, 1);
			else
				t500rs_log.warning("Host does not support autocenter fallback: %s", SDL_GetError());
		}
		else if (m_t500rs_autocenter_id >= 0)
		{
			SDL_DestroyHapticEffect(m_haptic_handle, m_t500rs_autocenter_id);
			m_t500rs_autocenter_id = -1;
		}
		m_t500rs_autocenter = autocenter;
	}

	const u64 now = get_timestamp();
	for (std::size_t i = 0; i < m_t500rs_host_slots.size(); ++i)
	{
		const auto& s = i == snapshot.effect_slots.size() ? snapshot.direct_slot : snapshot.effect_slots[i];
		auto& h = m_t500rs_host_slots[i];
		auto effect = snapshot.decode(i, m_reverse_effects);
		const bool expired = effect.length != SDL_HAPTIC_INFINITY && s.playing && now >= s.started_at_us &&
			(now - s.started_at_us) / 1000 >= static_cast<u64>(effect.delay) + effect.length;
		const bool playing = s.playing && !expired && effect.kind != t500rs::effect_kind::none;
		if (!s.declared || h.generation != s.generation)
		{
			if (h.id >= 0) SDL_DestroyHapticEffect(m_haptic_handle, h.id);
			h = {};
			h.generation = s.generation;
		}
		if (!playing)
		{
			// Free stopped slots so wheels with small hardware pools can play later effects.
			if (h.id >= 0) SDL_DestroyHapticEffect(m_haptic_handle, h.id);
			h = {};
			h.generation = s.generation;
			continue;
		}
		const auto scale = [&](auto value) { return static_cast<decltype(value)>(static_cast<int>(value) * static_cast<int>(gain) / static_cast<int>(gain_limit)); };
		effect.level = scale(effect.level);
		effect.magnitude = scale(effect.magnitude);
		effect.offset = scale(effect.offset);
		effect.attack_level = scale(effect.attack_level);
		effect.fade_level = scale(effect.fade_level);
		effect.right_coeff = scale(effect.right_coeff);
		effect.left_coeff = scale(effect.left_coeff);
		effect.right_sat = scale(effect.right_sat);
		effect.left_sat = scale(effect.left_sat);
		// Some host drivers lack square-wave support. Synthesize a constant
		// slot, retaining START/STOP and delay/duration semantics.
		if (effect.kind == t500rs::effect_kind::square && !(SDL_GetHapticFeatures(m_haptic_handle) & SDL_HAPTIC_SQUARE))
		{
			const u64 elapsed = now >= s.started_at_us ? (now - s.started_at_us) / 1000 : 0;
			const u64 phase = elapsed >= effect.delay ? elapsed - effect.delay : 0;
			const u64 shifted = phase + static_cast<u64>(effect.phase) * effect.period / 36000;
			int magnitude = effect.magnitude;
			if (effect.attack_length && phase < effect.attack_length)
				magnitude = (magnitude < 0 ? -1 : 1) * (effect.attack_level + (std::abs(magnitude) - effect.attack_level) * static_cast<int>(phase) / effect.attack_length);
			if (effect.length != SDL_HAPTIC_INFINITY && effect.fade_length && phase < effect.length && effect.length - phase < effect.fade_length)
				magnitude = (magnitude < 0 ? -1 : 1) * (effect.fade_level + (std::abs(magnitude) - effect.fade_level) * static_cast<int>(effect.length - phase) / effect.fade_length);
			effect.level = elapsed < effect.delay ? 0 : static_cast<s16>(std::clamp(effect.offset + (shifted % effect.period < effect.period / 2 ? magnitude : -magnitude), -32767, 32767));
			effect.kind = t500rs::effect_kind::constant;
			effect.length = SDL_HAPTIC_INFINITY;
			effect.delay = effect.attack_length = effect.fade_length = 0;
		}
		if (h.failed && effect == h.last && h.starts == s.starts)
			continue;
		SDL_HapticEffect native = make_effect(effect, m_t500rs_direction);
		// Delay is managed by SDL only on a new START. Updating a running effect
		// must not call Run again, which would restart the envelope every packet.
		if (h.id >= 0 && make_effect(h.last, m_t500rs_direction).type != native.type)
		{
			SDL_DestroyHapticEffect(m_haptic_handle, h.id);
			h.id = -1;
			h.playing = false;
		}
		bool ok = true;
		if (h.id < 0)
		{
			if (SDL_HapticEffectSupported(m_haptic_handle, &native))
				h.id = SDL_CreateHapticEffect(m_haptic_handle, &native);
			ok = h.id >= 0;
			t500rs_log.trace("Host FFB create: slot=%u kind=%u id=%d ok=%d", static_cast<u32>(i), native.type, h.id, ok);
		}
		else if (!(effect == h.last) || h.failed)
		{
			ok = SDL_UpdateHapticEffect(m_haptic_handle, h.id, &native);
			t500rs_log.trace("Host FFB update: slot=%u ok=%d level=%d coeff=[%d,%d] center=%d saturation=[%u,%u] gain=%u/%u",
				static_cast<u32>(i), ok, effect.level, effect.right_coeff, effect.left_coeff, effect.center,
				effect.right_sat, effect.left_sat, gain, gain_limit);
		}
		if (ok && (!h.playing || h.starts != s.starts))
		{
			if (h.playing) SDL_StopHapticEffect(m_haptic_handle, h.id);
			ok = SDL_RunHapticEffect(m_haptic_handle, h.id, 1);
			t500rs_log.trace("Host FFB run: slot=%u id=%d ok=%d length=%u delay=%u", static_cast<u32>(i), h.id, ok, effect.length, effect.delay);
			h.playing = ok;
			h.starts = s.starts;
		}
		if (!ok && !h.failed)
			t500rs_log.warning("Host FFB slot %u failed (type=%u): %s", static_cast<u32>(i), native.type, SDL_GetError());
		if (!ok && h.id >= 0)
		{
			SDL_StopHapticEffect(m_haptic_handle, h.id);
			h.playing = false;
		}
		h.starts = s.starts;
		h.failed = !ok;
		h.last = effect;
	}
}
#endif
