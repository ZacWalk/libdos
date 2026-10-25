#pragma once

// libdos by Zac Walker
//
// 8086/80186 real-mode CPU state and interpreter. The CPU is a plain-data
// struct so the whole machine can be copied, saved or inspected; every entry
// point takes the machine_state it belongs to.

#include <cstdint>

struct machine_state;

// Register file order matches the x86 encoding, so a modrm field is a direct
// index. Segment registers follow the eight general registers.
enum : int { reg_ax, reg_cx, reg_dx, reg_bx, reg_sp, reg_bp, reg_si, reg_di };
enum : int { seg_es, seg_cs, seg_ss, seg_ds, seg_fs, seg_gs };
constexpr int reg_seg = 8;

constexpr uint16_t flag_cf = 0x0001;
constexpr uint16_t flag_pf = 0x0004;
constexpr uint16_t flag_af = 0x0010;
constexpr uint16_t flag_zf = 0x0040;
constexpr uint16_t flag_sf = 0x0080;
constexpr uint16_t flag_tf = 0x0100;
constexpr uint16_t flag_if = 0x0200;
constexpr uint16_t flag_df = 0x0400;
constexpr uint16_t flag_of = 0x0800;

constexpr uint8_t int_divide_error = 0;
constexpr uint8_t int_single_step = 1;
constexpr uint8_t int_breakpoint = 3;
constexpr uint8_t int_overflow = 4;
constexpr uint8_t int_bound = 5;
constexpr uint8_t int_invalid_opcode = 6;

// Which processor the interpreter is. The default runs what late-80s games
// assume: 8086 and 80186, and the 80386's two-byte opcodes in their 16-bit
// forms. The 8086 models stop the machine at anything an 8088 does not have,
// so a program meant for an IBM PC cannot quietly rely on a newer chip, and
// behave like one where it differs: PUSH SP and shift counts. The strict model
// also refuses the instructions a program on an XT without a coprocessor has
// no business executing - x87 escapes, WAIT, LOCK and HLT - which is what the
// original 1988 toolchain is run under.
enum class cpu_model : uint8_t
{
	i386,
	i80186,
	i8086,
	i8086_strict,
};

struct cpu_state
{
	uint16_t regs[14] = {};
	uint16_t ip = 0;
	uint16_t flags = 0x0202;

	// Prefix and REP state survive between cpu_step calls: one call executes
	// one repetition of a string instruction, so an interrupt can be taken in
	// the middle of a REP exactly as it can on hardware.
	uint8_t opcode = 0;
	bool prefix = false;
	bool repeating = false;
	int rep = 0; // 0 none, 1 REPNZ, 2 REPZ
	int segment_override = -1;
	uint16_t instruction_ip = 0; // first byte of the instruction, prefixes included

	bool halted = false;
	uint64_t cycles = 0;
	uint64_t instructions = 0; // one per repetition of a string instruction, as cpu_step counts them

	uint16_t ax() const { return regs[reg_ax]; }
	uint16_t cx() const { return regs[reg_cx]; }
	uint16_t dx() const { return regs[reg_dx]; }
	uint16_t bx() const { return regs[reg_bx]; }
	uint16_t sp() const { return regs[reg_sp]; }
	uint16_t bp() const { return regs[reg_bp]; }
	uint16_t si() const { return regs[reg_si]; }
	uint16_t di() const { return regs[reg_di]; }
	uint16_t es() const { return regs[reg_seg + seg_es]; }
	uint16_t cs() const { return regs[reg_seg + seg_cs]; }
	uint16_t ss() const { return regs[reg_seg + seg_ss]; }
	uint16_t ds() const { return regs[reg_seg + seg_ds]; }
	uint16_t fs() const { return regs[reg_seg + seg_fs]; }
	uint16_t gs() const { return regs[reg_seg + seg_gs]; }

	void set_ax(uint16_t v) { regs[reg_ax] = v; }
	void set_cx(uint16_t v) { regs[reg_cx] = v; }
	void set_dx(uint16_t v) { regs[reg_dx] = v; }
	void set_bx(uint16_t v) { regs[reg_bx] = v; }
	void set_sp(uint16_t v) { regs[reg_sp] = v; }
	void set_bp(uint16_t v) { regs[reg_bp] = v; }
	void set_si(uint16_t v) { regs[reg_si] = v; }
	void set_di(uint16_t v) { regs[reg_di] = v; }
	void set_es(uint16_t v) { regs[reg_seg + seg_es] = v; }
	void set_cs(uint16_t v) { regs[reg_seg + seg_cs] = v; }
	void set_ss(uint16_t v) { regs[reg_seg + seg_ss] = v; }
	void set_ds(uint16_t v) { regs[reg_seg + seg_ds] = v; }
	void set_fs(uint16_t v) { regs[reg_seg + seg_fs] = v; }
	void set_gs(uint16_t v) { regs[reg_seg + seg_gs] = v; }

	uint8_t al() const { return uint8_t(regs[reg_ax]); }
	uint8_t ah() const { return uint8_t(regs[reg_ax] >> 8); }
	uint8_t bl() const { return uint8_t(regs[reg_bx]); }
	uint8_t bh() const { return uint8_t(regs[reg_bx] >> 8); }
	uint8_t cl() const { return uint8_t(regs[reg_cx]); }
	uint8_t ch() const { return uint8_t(regs[reg_cx] >> 8); }
	uint8_t dl() const { return uint8_t(regs[reg_dx]); }
	uint8_t dh() const { return uint8_t(regs[reg_dx] >> 8); }

	void set_al(uint8_t v) { regs[reg_ax] = uint16_t((regs[reg_ax] & 0xff00) | v); }
	void set_ah(uint8_t v) { regs[reg_ax] = uint16_t((regs[reg_ax] & 0x00ff) | (v << 8)); }
	void set_bl(uint8_t v) { regs[reg_bx] = uint16_t((regs[reg_bx] & 0xff00) | v); }
	void set_bh(uint8_t v) { regs[reg_bx] = uint16_t((regs[reg_bx] & 0x00ff) | (v << 8)); }
	void set_cl(uint8_t v) { regs[reg_cx] = uint16_t((regs[reg_cx] & 0xff00) | v); }
	void set_ch(uint8_t v) { regs[reg_cx] = uint16_t((regs[reg_cx] & 0x00ff) | (v << 8)); }
	void set_dl(uint8_t v) { regs[reg_dx] = uint16_t((regs[reg_dx] & 0xff00) | v); }
	void set_dh(uint8_t v) { regs[reg_dx] = uint16_t((regs[reg_dx] & 0x00ff) | (v << 8)); }

	// Byte register encoding: 0-3 are AL CL DL BL, 4-7 are AH CH DH BH.
	uint8_t reg8(int i) const
	{
		const uint16_t r = regs[i & 3];
		return uint8_t(i < 4 ? r : r >> 8);
	}

	void set_reg8(int i, uint8_t v)
	{
		uint16_t& r = regs[i & 3];
		if (i < 4) r = uint16_t((r & 0xff00) | v);
		else r = uint16_t((r & 0x00ff) | (v << 8));
	}

	bool cf() const { return (flags & flag_cf) != 0; }
	bool zf() const { return (flags & flag_zf) != 0; }
	bool sf() const { return (flags & flag_sf) != 0; }
	bool if_() const { return (flags & flag_if) != 0; }
	bool df() const { return (flags & flag_df) != 0; }
	bool of() const { return (flags & flag_of) != 0; }

	void set_flag(uint16_t mask, bool on) { flags = uint16_t(on ? (flags | mask) : (flags & ~mask)); }
};

void cpu_reset(cpu_state& c);

// Executes one instruction (or one repetition of a string instruction) and
// returns the cycles it cost. Hardware interrupts are taken at entry.
int cpu_step(machine_state& s);

// Pushes flags/CS/IP and vectors through the IVT, exactly as INT n does.
void cpu_interrupt(machine_state& s, uint8_t vector);

// Rewrites the flags word an IRET will pop, so an intercepted DOS or BIOS call
// can return a status in CF the way the real service does.
void cpu_set_iret_flag(machine_state& s, uint16_t mask, bool on);
