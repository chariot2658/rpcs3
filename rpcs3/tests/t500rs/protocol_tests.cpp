// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone tests; no RPCS3/SDL dependencies. See run_tests.sh.
#include "../../Emu/Io/ThrustmasterT500RS.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <string_view>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; std::abort(); } } while (0)
using namespace t500rs;
std::vector<byte> hex(std::string_view text)
{
	std::vector<byte> out;
	int high = -1;
	for (const char c : text)
	{
		if (c == ' ') continue;
		const int n = c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
		CHECK(n >= 0 && n < 16);
		if (high < 0) high = n;
		else { out.push_back(static_cast<byte>((high << 4) | n)); high = -1; }
	}
	CHECK(high == -1);
	return out;
}
void send(protocol& p, std::string_view command, std::uint64_t now = 0)
{
	CHECK(p.output(hex(command), now) == result::ok);
}
void equals(std::span<const byte> actual, std::string_view expected)
{
	const auto bytes = hex(expected);
	CHECK(std::equal(actual.begin(), actual.end(), bytes.begin(), bytes.end()));
}

void equals_report(const input_report& actual, std::string_view prefix)
{
	auto expected = hex(prefix);
	expected.resize(32, 0);
	CHECK(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
}

void hid_report_sizes()
{
	// Parse HID short items independently to check the descriptor's declared
	// lengths, including padding and excluding the one-byte report ID.
	std::array<unsigned, 256> input_bits{}, output_bits{};
	unsigned size = 0, count = 0, id = 0;
	for (std::size_t i = 0; i < report_descriptor.size();)
	{
		const auto tag = report_descriptor[i++];
		const unsigned length = (tag & 3) == 3 ? 4 : tag & 3;
		CHECK(i + length <= report_descriptor.size());
		unsigned value = 0;
		for (unsigned j = 0; j < length; ++j)
			value |= static_cast<unsigned>(report_descriptor[i++]) << (j * 8);
		switch (tag & 0xfc)
		{
		case 0x74: size = value; break;
		case 0x94: count = value; break;
		case 0x84: id = value; CHECK(id < 256); break;
		case 0x80: input_bits[id] += size * count; break;
		case 0x90: output_bits[id] += size * count; break;
		default: break;
		}
	}
	CHECK(input_bits[7] == 31 * 8 && input_bits[2] == 31 * 8 && input_bits[0x14] == 31 * 8);
	CHECK(output_bits[0x0a] == 14 * 8);
	CHECK((configuration_descriptor[25] | (configuration_descriptor[26] << 8)) == report_descriptor.size());
	CHECK(configuration_descriptor[31] == 32 && configuration_descriptor[32] == 0);
}

void enumeration_and_input()
{
	// Capture-derived identity; IN endpoint/report lengths extended for GT5.
	equals(device_descriptor, "12010002000000084f045eb6000101020001");
	equals(configuration_descriptor, "09022900010100c032090400000203000000092111010001228a000705820320000207050103200004");
	hid_report_sizes();
	input in;
	in.steering = 0x80e6;
	equals_report(make_input_report(in), "07e680ff03ff03ff0300000000000f");
	in.steering = 0; in.throttle = 0; in.brake = 1023; in.clutch = 512; in.buttons = 0x1fff; in.hat = 7;
	equals_report(make_input_report(in), "070000ff03000000020000ff1f0007");
	in.steering = 65535; in.throttle = 65535; in.buttons = 0xe000; in.hat = 255;
	const auto r = make_input_report(in);
	CHECK(r[1] == 255 && r[2] == 255 && r[5] == 255 && r[6] == 3);
	CHECK(r[11] == 0 && r[12] == 0 && r[14] == 15);
	for (unsigned bit = 0; bit < 13; ++bit)
	{
		in.buttons = static_cast<std::uint16_t>(1u << bit);
		const auto b = make_input_report(in);
		CHECK(static_cast<unsigned>(b[11] | (b[12] << 8)) == (1u << bit));
	}
	equals(vendor_reply(0x49), "49000000010002000300000002020000");
	equals(vendor_reply(0x47), "4700030000000200");
	equals(vendor_reply(0x56), "56002f00");
	equals(vendor_reply(0x42), "42e8030000000000");
	equals(vendor_reply(0x4e), "4e10000000000000");
	CHECK(vendor_reply(0x99).empty());
}

void gt5_pedal_order()
{
	input state;
	state.throttle = 0;
	equals_report(make_input_report(state), "070080ff030000ff0300000000000f");
	state.throttle = 1023;
	state.brake = 0;
	equals_report(make_input_report(state), "0700800000ff03ff0300000000000f");
	state.brake = 1023;
	state.clutch = 0;
	equals_report(make_input_report(state), "070080ff03ff03000000000000000f");
}

void initialization()
{
	protocol p;
	send(p, "420100000000000000000000000000");
	send(p, "0a0490030000000000000000000000");
	send(p, "0a0412100000000000000000000000");
	send(p, "0a0400060000000000000000000000");
	input_report r;
	CHECK(p.pop_reply(r)); equals_report(r, "14209003afa72e1400000000000000");
	CHECK(p.pop_reply(r)); equals_report(r, "14201210002f5eb600000000000000");
	CHECK(p.pop_reply(r)); equals_report(r, "142000061800000000000000000000");
	CHECK(!p.pop_reply(r));
	CHECK(p.output(hex("0a04ffff")) == result::unsupported);
	for (unsigned i = 0; i < 16; ++i) send(p, "0a041210");
	CHECK(p.output(hex("0a041210")) == result::malformed);
	send(p, "4201"); CHECK(!p.pop_reply(r));
	send(p, "4011ff3f"); CHECK(p.range == 270);
	send(p, "401154d5"); CHECK(p.range == 900);
	send(p, "40110000"); CHECK(p.range == 40);
	send(p, "4011ffff"); CHECK(p.range == 1080);
	// Every supported whole-degree request round-trips the GT5 encoder.
	for (unsigned degrees = 40; degrees <= 1080; ++degrees)
	{
		const unsigned encoded = degrees * 65535 / 1080;
		const std::array<byte, 4> command{0x40, 0x11, static_cast<byte>(encoded), static_cast<byte>(encoded >> 8)};
		CHECK(p.output(command) == result::ok && p.range == degrees);
	}
	send(p, "40033700"); send(p, "40040100"); CHECK(p.autocenter_enabled && p.autocenter_strength == 55);
	send(p, "434c"); CHECK(p.gain == 76);
	send(p, "410f0001"); CHECK(!p.autocenter_enabled);
}

void captured_constant_and_periodic()
{
	protocol p;
	// Captured constant force upload: parameters precede MAIN, START argument=1.
	send(p, "021c00000000000000"); send(p, "030e001c");
	send(p, "01000040271000ffff0e001c000000");
	CHECK(p.decode(0).level == 28 * 32767 / 127);
	CHECK(p.decode(0).delay == 0 && p.decode(0).length == 0x1027);
	CHECK(!p.effect_slots[0].playing);
	send(p, "41004101", 1000); CHECK(p.effect_slots[0].playing && p.effect_slots[0].started_at_us == 1000);
	const auto start = p.effect_slots[0].starts;
	send(p, "030e00cc"); CHECK(p.decode(0).level == -52 * 32767 / 127);
	CHECK(p.effect_slots[0].starts == start);
	send(p, "030e0080"); CHECK(p.decode(0).level == -32767); CHECK(p.decode(0, true).level == 32767);
	send(p, "41000001"); CHECK(!p.effect_slots[0].playing);
	send(p, "41004101"); CHECK(p.effect_slots[0].starts != start);
	// Captured ctl_panel_boing: 149 ms attack, 300 ms delay, 33 ms sine period.
	send(p, "02380095003fe50100"); send(p, "042a002000002100");
	send(p, "01012240bc02002c012a0038000000"); send(p, "41014101");
	const auto e = p.decode(1);
	CHECK(e.kind == effect_kind::sine && e.magnitude == 32 * 32767 / 127);
	CHECK(e.length == 700 && e.delay == 300 && e.period == 33);
	CHECK(e.attack_length == 149 && e.attack_level == 63 * 32767 / 127);
	CHECK(e.fade_length == 485 && e.fade_level == 0);
	// Captured bumpy-road second slot: concurrent slots must retain independent parameters/STOPs.
	send(p, "02540000000c00000c"); send(p, "0446000c00004d01");
	send(p, "01022240e803000000460054000000"); send(p, "41024101");
	CHECK(p.decode(2).period == 333 && p.decode(2).magnitude == 12 * 32767 / 127);
	send(p, "41010001"); CHECK(!p.effect_slots[1].playing && p.effect_slots[2].playing);
}

void linux_stream_and_conditions()
{
	protocol p(dialect::linux_reference);
	send(p, "021c00000000000000"); send(p, "030e007f");
	send(p, "01000040ffff0000000e001c000000"); send(p, "410041ff");
	CHECK(p.decode(0).level == 32767 && p.decode(0).length == 0xffffffffu);
	send(p, "01002240ffff0000000e001c000000"); send(p, "410041ff");
	send(p, "040e0000fe001027");
	CHECK(p.decode(0).kind == effect_kind::constant && p.decode(0).level == -2 * 32767 / 127);
	CHECK(p.output(hex("042a002000002100")) == result::unsupported);
	// Driver sends condition X and Y parameters before MAIN. Y must not erase X.
	send(p, "052a000a05ecff03006432"); send(p, "0538000000000000000000");
	send(p, "01014040ffff0000002a0038000000"); send(p, "410141ff");
	const auto e = p.decode(1);
	CHECK(e.kind == effect_kind::spring && e.right_coeff == 32767 && e.left_coeff == 16383);
	CHECK(e.center == -400 && e.deadband == 195 && e.right_sat == 65535 && e.left_sat == 32767);
	CHECK(p.decode(1, true).right_coeff == -32767);
	// Full 16-slot hardware ID range, including subtype high-byte truncation.
	for (unsigned i = 1; i < 16; ++i)
	{
		const unsigned pc = 14 + 28*i, ec = 28 + 28*i;
		const std::array<byte, 11> condition{5,static_cast<byte>(pc),0,10,10,0,0,0,0,100,100};
		CHECK(p.output(condition) == result::ok);
		const std::array<byte, 15> main{1,static_cast<byte>(i),0x41,0x40,0xff,0xff,0,0,0,static_cast<byte>(pc),static_cast<byte>(pc>>8),static_cast<byte>(ec),static_cast<byte>(ec>>8),0,0};
		CHECK(p.output(main) == result::ok);
		CHECK(p.decode(i).kind == effect_kind::damper && p.decode(i).right_coeff == 32767);
	}
	p.stop_all(); for (const auto& s : p.effect_slots) CHECK(!s.playing);
	p.reset(); CHECK(p.decode(0).kind == effect_kind::none && p.format == dialect::linux_reference);
}

void malformed_and_fuzz()
{
	protocol p;
	for (const auto text : {"01000040ffff0000000e001c000000", "021c00000000000000", "030e007f", "040e000001001027", "052a000a05000000006464", "0a049003", "4011f0d2", "41000001", "4205", "43ff"})
	{
		const auto packet = hex(text);
		for (std::size_t length = 0; length < packet.size(); ++length)
			CHECK(p.output(std::span<const byte>(packet).first(length)) == result::malformed);
	}
	CHECK(p.output(hex("01100040ffff0000000e001c000000")) == result::malformed);
	CHECK(p.output(hex("41ff4101")) == result::malformed);
	CHECK(p.output(hex("01007740ffff0000000e001c000000")) == result::unsupported);
	CHECK(p.output(hex("41004101")) == result::unsupported);
	std::mt19937 random(0x500);
	std::array<byte, 64> bytes{};
	for (unsigned n = 0; n < 100000; ++n)
	{
		for (auto& b : bytes) b = static_cast<byte>(random());
		p.output(std::span<const byte>(bytes).first(random() % 65), n);
		for (std::size_t i = 0; i < 17; ++i) (void)p.decode(i, (n & 1) != 0);
		if ((n & 255) == 0) p.reset();
	}
}

int main()
{
	enumeration_and_input(); gt5_pedal_order(); initialization(); captured_constant_and_periodic(); linux_stream_and_conditions(); malformed_and_fuzz();
	std::cout << "PASS: descriptors, input, captured initialization, FFB lifecycle, 16 slots, truncation and 100000 fuzz packets\n";
}
