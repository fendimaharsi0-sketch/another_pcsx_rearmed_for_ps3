#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

#if P_HAVE_MMAP
#include <sys/mman.h>
#endif

#include "lightrec.h"
#include "lightrec-private.h"
#include "../cdrom.h"
#include "../gpu.h"
#include "../gte.h"
#include "../pgxp.h"
#include "../mdec.h"
#include "../psxdma.h"
#include "../psxhw.h"
#include "../psxmem.h"
#include "../r3000a.h"
#include "../psxinterpreter.h"
#include "../psxhle.h"
#include "../psxevents.h"

#include "../frontend/main.h"

#include "mem.h"
#include "plugin.h"

#if (defined(__arm__) || defined(__aarch64__)) && !defined(ALLOW_LIGHTREC_ON_ARM)
#error "Lightrec should not be used on ARM (please specify DYNAREC=ari64 to make)"
#endif

#define ARRAY_SIZE(x) (sizeof(x) ? sizeof(x) / sizeof((x)[0]) : 0)

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#	define LE32TOH(x)	__builtin_bswap32(x)
#	define HTOLE32(x)	__builtin_bswap32(x)
#	define LE16TOH(x)	__builtin_bswap16(x)
#	define HTOLE16(x)	__builtin_bswap16(x)
#else
#	define LE32TOH(x)	(x)
#	define HTOLE32(x)	(x)
#	define LE16TOH(x)	(x)
#	define HTOLE16(x)	(x)
#endif

#ifdef __GNUC__
#	define likely(x)       __builtin_expect(!!(x),1)
#	define unlikely(x)     __builtin_expect(!!(x),0)
#else
#	define likely(x)       (x)
#	define unlikely(x)     (x)
#endif

#ifndef LIGHTREC_PROG_NAME
#  ifdef __linux__
#    define LIGHTREC_PROG_NAME "/proc/self/exe"
#  else
#    define LIGHTREC_PROG_NAME "retroarch.exe"
#  endif
#endif

#ifndef GPUSTATUS_POLLING_THRESHOLD
#  define GPUSTATUS_POLLING_THRESHOLD 0
#endif

psxRegisters psxRegs;
Rcnt rcnts[4];

void* code_buffer;

#if defined(__lv2ppu__)
/* The PS3 OS only lets a game execute code from its own loaded image, and that
 * image's text pages stay writable at run time. So the dynarec's code buffer
 * is reserved inside .text. (The "#" ends the section flags gcc emits, which
 * would otherwise clash with .text's.) */
static unsigned char ps3_code_buffer[CODE_BUFFER_SIZE]
	__attribute__((aligned(4096), used, section(".text.lightrec_code #")));
#endif

static struct lightrec_state *lightrec_state;

/* where lightrec_plugin_execute_internal() is (for hang reports) */
static volatile int plugin_where;

static bool use_lightrec_interpreter;
static bool block_stepping;
//static bool use_pcsx_interpreter;
#define use_pcsx_interpreter 0
static bool ram_disabled;
static bool lightrec_debug, lightrec_very_debug;
static u32 lightrec_begin_cycles;

extern u32 lightrec_hacks;

static void lightrec_plugin_apply_config();
extern void lightrec_code_inv(void *ptr, uint32_t len);

static char cache_buf[64 * 1024];

static void cop2_op(struct lightrec_state *state, u32 func)
{
	struct lightrec_registers *regs = lightrec_get_registers(state);
	/* This works because regs->cp2c comes right after regs->cp2d,
	 * so it can be cast to a pcsxCP2Regs pointer. */
	gteDispatch((psxCP2Regs *)regs->cp2d, func);
}

static void cop2_notify(struct lightrec_state *state, u32 op, u32 data)
{
	/* Registering this callback makes the Lightrec emitter fall back to C
	 * calls for MFC2/MTC2 (instead of inlining them), so every GTE register
	 * move is visible here, in both the JIT and the interpreter paths.
	 * Cost: MFC2/MTC2 become C calls even when tracking is off (the choice
	 * is made at block-compile time); pgxp_on_notify itself returns
	 * immediately when tracking is off. Unmeasured on hardware; revisit
	 * if FPS drops. */
	(void)state;
	pgxp_on_notify(op, data);
}

/* PGXP gate: the master switch fully disconnects the JIT hooks. lightrec_init
 * memcpy'd lightrec_ops into state->ops, so this re-points the state's OWN
 * copy (the const table is untouched) and then flushes every compiled block,
 * so all live code matches the new registration: with NULL, the emitter
 * inlines MFC2/MTC2 again and rec_pgxp_mem emits nothing; with cop2_notify,
 * both hooks are back. Called only on a master change, from
 * config_mirror_gte_pgxp (menu toggle, game begin/end). NULL state (boot
 * before init, no game running, interpreter CPU) skips both steps; the next
 * init uses the current ops. Toggling happens with the game paused in the
 * menu, so flushing all blocks is safe (no block is executing). */
void lightrec_pgxp_gate(int on)
{
	if (!lightrec_state) return;
	lightrec_state->ops.cop2_notify = on ? cop2_notify : NULL;
	lightrec_invalidate_all(lightrec_state);
}

static bool has_interrupt(void)
{
	struct lightrec_registers *regs = lightrec_get_registers(lightrec_state);

	return ((psxHu32(0x1070) & psxHu32(0x1074)) &&
		(regs->cp0[12] & 0x401) == 0x401) ||
		(regs->cp0[12] & regs->cp0[13] & 0x0300);
}

static void lightrec_transition_to_pcsx(struct lightrec_state *state)
{
	psxRegs.cycle += lightrec_current_cycle_count(state) / 1024;
	lightrec_reset_cycle_count(state, 0);
}

static void lightrec_transition_from_pcsx(struct lightrec_state *state)
{
	s32 cycles_left = psxRegs.next_interupt - psxRegs.cycle;

	if (block_stepping || cycles_left <= 0 || has_interrupt())
		lightrec_set_exit_flags(state, LIGHTREC_EXIT_CHECK_INTERRUPT);
	else {
		lightrec_set_target_cycle_count(state, cycles_left * 1024);
	}
}

static void hw_write_byte(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u32 val)
{
	lightrec_transition_to_pcsx(state);

	psxHwWrite8(&psxRegs, mem, val);

	lightrec_transition_from_pcsx(state);
}

static void hw_write_half(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u32 val)
{
	lightrec_transition_to_pcsx(state);

	psxHwWrite16(&psxRegs, mem, val);

	lightrec_transition_from_pcsx(state);
}

static void hw_write_word(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u32 val)
{
	lightrec_transition_to_pcsx(state);

	psxHwWrite32(&psxRegs, mem, val);

	lightrec_transition_from_pcsx(state);
}

/*
 * Busy-wait polling of a hardware status register (the BIOS waits for the
 * vblank and the CD-ROM like this while it shows its logo, some games too):
 * each read is a call out of the recompiled code, and on a slow host (the PS3's
 * PPU) tens of thousands of them per frame took most of the frame. When the
 * same register is read again with the same value after the same number of
 * cycles as the time before, the CPU is in such a loop: time moves on by that
 * many cycles per read, without running the loop, until the value changes or
 * the next event is due. Only registers whose reads change nothing.
 */
unsigned int lightrec_poll_skips, lightrec_poll_addr;

static bool poll_safe(u32 mem)
{
	switch (mem & 0x1fffffff) {
	case 0x1f801070: case 0x1f801074:   /* interrupt status / mask */
	case 0x1f801044:                    /* pad/memory card status */
	case 0x1f801800: case 0x1f801803:   /* CD-ROM status, interrupt enable/flags */
	case 0x1f801814:                    /* GPU status */
	case 0x1f8010f0: case 0x1f8010f4:   /* DMA control, DMA interrupts */
		return true;
	}
	/* DMA channel control (the busy bit): 0x1f801088 + 0x10 * channel */
	return (mem & 0x1fffff8f) == 0x1f801088 && ((mem >> 4) & 0xf) >= 8 && ((mem >> 4) & 0xf) <= 0xe;
}

static u32 poll_skip(u32 mem, u32 val, int size)
{
	static u32 last_mem, last_val, c1, c2;
	static int last_size;
	u32 diff = psxRegs.cycle - c1;

	if (mem == last_mem && size == last_size && val == last_val
	    && diff > 0 && diff < 256 && diff == c1 - c2 && poll_safe(mem)) {
		u32 n = 0;
		while ((s32)(psxRegs.next_interupt - psxRegs.cycle) > (s32)diff && val == last_val) {
			psxRegs.cycle += diff;
			val = size == 4 ? psxHwRead32(&psxRegs, mem)
			    : size == 2 ? psxHwRead16(&psxRegs, mem)
			    : psxHwRead8(&psxRegs, mem);
			n++;
		}
		if (n) {
			lightrec_poll_skips += n;
			lightrec_poll_addr = mem;
		}
	}

	c2 = c1; c1 = psxRegs.cycle;
	last_mem = mem; last_val = val; last_size = size;
	return val;
}

static u8 hw_read_byte(struct lightrec_state *state, u32 op, void *host, u32 mem)
{
	u8 val;

	lightrec_transition_to_pcsx(state);

	val = psxHwRead8(&psxRegs, mem);
	val = poll_skip(mem, val, 1);

	lightrec_transition_from_pcsx(state);

	return val;
}

static u16 hw_read_half(struct lightrec_state *state,
			u32 op, void *host, u32 mem)
{
	u16 val;

	lightrec_transition_to_pcsx(state);

	val = psxHwRead16(&psxRegs, mem);
	val = poll_skip(mem, val, 2);

	lightrec_transition_from_pcsx(state);

	return val;
}

static u32 hw_read_word(struct lightrec_state *state,
			u32 op, void *host, u32 mem)
{
	u32 val;

	lightrec_transition_to_pcsx(state);

	val = psxHwRead32(&psxRegs, mem);
	val = poll_skip(mem, val, 4);

	lightrec_transition_from_pcsx(state);

	return val;
}

static struct lightrec_mem_map_ops hw_regs_ops = {
	.sb = hw_write_byte,
	.sh = hw_write_half,
	.sw = hw_write_word,
	.lb = hw_read_byte,
	.lh = hw_read_half,
	.lw = hw_read_word,
};

static u32 cache_ctrl;

static void cache_ctrl_write_word(struct lightrec_state *state,
				  u32 op, void *host, u32 mem, u32 val)
{
	cache_ctrl = val;
}

static u32 cache_ctrl_read_word(struct lightrec_state *state,
				u32 op, void *host, u32 mem)
{
	return cache_ctrl;
}

static struct lightrec_mem_map_ops cache_ctrl_ops = {
	.sw = cache_ctrl_write_word,
	.lw = cache_ctrl_read_word,
};

static struct lightrec_mem_map lightrec_map[] = {
	[PSX_MAP_KERNEL_USER_RAM] = {
		/* Kernel and user memory */
		.pc = 0x00000000,
		.length = 0x200000,
	},
	[PSX_MAP_BIOS] = {
		/* BIOS */
		.pc = 0x1fc00000,
		.length = 0x80000,
	},
	[PSX_MAP_SCRATCH_PAD] = {
		/* Scratch pad */
		.pc = 0x1f800000,
		.length = 0x400,
	},
	[PSX_MAP_PARALLEL_PORT] = {
		/* Parallel port */
		.pc = 0x1f000000,
		.length = 0x10000,
	},
	[PSX_MAP_HW_REGISTERS] = {
		/* Hardware registers */
		.pc = 0x1f801000,
		.length = 0x8000,
		.ops = &hw_regs_ops,
	},
	[PSX_MAP_CACHE_CONTROL] = {
		/* Cache control */
		.pc = 0x5ffe0130,
		.length = 4,
		.ops = &cache_ctrl_ops,
	},

	/* Mirrors of the kernel/user memory */
	[PSX_MAP_MIRROR1] = {
		.pc = 0x00200000,
		.length = 0x200000,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
	[PSX_MAP_MIRROR2] = {
		.pc = 0x00400000,
		.length = 0x200000,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
	[PSX_MAP_MIRROR3] = {
		.pc = 0x00600000,
		.length = 0x200000,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
	[PSX_MAP_BIOS_MIRROR1] = {
		.pc = 0x1fc80000,
		.length = 0x80000,
		.mirror_of = &lightrec_map[PSX_MAP_BIOS],
	},
	[PSX_MAP_BIOS_MIRROR2] = {
		.pc = 0x1fd00000,
		.length = 0x80000,
		.mirror_of = &lightrec_map[PSX_MAP_BIOS],
	},
	[PSX_MAP_BIOS_MIRROR3] = {
		.pc = 0x1fd80000,
		.length = 0x80000,
		.mirror_of = &lightrec_map[PSX_MAP_BIOS],
	},
	[PSX_MAP_BIOS_MIRROR4] = {
		.pc = 0x1fe00000,
		.length = 0x80000,
		.mirror_of = &lightrec_map[PSX_MAP_BIOS],
	},
	[PSX_MAP_BIOS_MIRROR5] = {
		.pc = 0x1fe80000,
		.length = 0x80000,
		.mirror_of = &lightrec_map[PSX_MAP_BIOS],
	},
	[PSX_MAP_BIOS_MIRROR6] = {
		.pc = 0x1ff00000,
		.length = 0x80000,
		.mirror_of = &lightrec_map[PSX_MAP_BIOS],
	},
	[PSX_MAP_BIOS_MIRROR7] = {
		.pc = 0x1ff80000,
		.length = 0x80000,
		.mirror_of = &lightrec_map[PSX_MAP_BIOS],
	},

	/* Mirror of the parallel port. Only used by the PS2/PS3 BIOS */
	[PSX_MAP_PPORT_MIRROR] = {
		.pc = 0x1fa00000,
		.length = 0x10000,
		.mirror_of = &lightrec_map[PSX_MAP_PARALLEL_PORT],
	},

	/* Code buffer */
	[PSX_MAP_CODE_BUFFER] = {
		.length = CODE_BUFFER_SIZE,
	},
};

static void lightrec_enable_ram(struct lightrec_state *state, bool enable)
{
	if (enable)
		memcpy(psxRegs.ptrs.psxM, cache_buf, sizeof(cache_buf));
	else
		memcpy(cache_buf, psxRegs.ptrs.psxM, sizeof(cache_buf));

	ram_disabled = !enable;
}

static bool lightrec_can_hw_direct(u32 kaddr, bool is_write, u8 size)
{
	kaddr &= 0xffff;
	if (is_write && size != 32) {
		// force32 so must go through handlers
		if (0x1000 <= kaddr && kaddr < 0x1024)
			return false;
		if ((kaddr & 0xff80) == 0x1080) // dma
			return false;
	}

	// io/fifo
	if ((kaddr & 0xffec) == 0x1040 || // 1040, 1050
	    (kaddr & 0xffc0) == 0x1100 ||
	    (kaddr & 0xffc0) == 0x1800)
		return false;

	// need write handlers/read-only
	if ((kaddr & 0xffe0) == 0x1040 ||
	    (kaddr & 0xfff8) == 0x1070 ||
	    (kaddr & 0xff88) == 0x1088 ||
	    (kaddr & 0xfff0) == 0x10f0 ||
	     kaddr >= 0x2000)
		return !is_write;

	// spu
	if (0x1c00 <= kaddr && kaddr < 0x2000)
		return false;

	return true;
}

static const struct lightrec_ops lightrec_ops = {
	.cop2_op = cop2_op,
	.cop2_notify = cop2_notify,
	.enable_ram = lightrec_enable_ram,
	.hw_direct = lightrec_can_hw_direct,
	.code_inv = LIGHTREC_CODE_INV ? lightrec_code_inv : NULL,
};

static int lightrec_plugin_init(void)
{
	lightrec_map[PSX_MAP_KERNEL_USER_RAM].address = psxRegs.ptrs.psxM;
	lightrec_map[PSX_MAP_BIOS].address = psxRegs.ptrs.psxR;
	lightrec_map[PSX_MAP_SCRATCH_PAD].address = psxRegs.ptrs.psxH;
	lightrec_map[PSX_MAP_HW_REGISTERS].address = psxRegs.ptrs.psxH + 0x1000;
	lightrec_map[PSX_MAP_PARALLEL_PORT].address = psxRegs.ptrs.psxP;

	if (!LIGHTREC_CUSTOM_MAP) {
#if defined(__lv2ppu__)
		code_buffer = ps3_code_buffer;
#elif P_HAVE_MMAP
		code_buffer = mmap(0, CODE_BUFFER_SIZE,
				   PROT_EXEC | PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (code_buffer == MAP_FAILED)
			return -ENOMEM;
#else
		code_buffer = malloc(CODE_BUFFER_SIZE);
		if (!code_buffer)
			return -ENOMEM;
#endif
	}

	if (LIGHTREC_CUSTOM_MAP) {
		lightrec_map[PSX_MAP_MIRROR1].address = psxRegs.ptrs.psxM + 0x200000;
		lightrec_map[PSX_MAP_MIRROR2].address = psxRegs.ptrs.psxM + 0x400000;
		lightrec_map[PSX_MAP_MIRROR3].address = psxRegs.ptrs.psxM + 0x600000;
	}

	lightrec_map[PSX_MAP_CODE_BUFFER].address = code_buffer;

	use_lightrec_interpreter = !!getenv("LIGHTREC_INTERPRETER");

#ifdef LIGHTREC_DEBUG
	char *cycles = getenv("LIGHTREC_BEGIN_CYCLES");

	lightrec_very_debug = !!getenv("LIGHTREC_VERY_DEBUG");
	lightrec_debug = lightrec_very_debug || !!getenv("LIGHTREC_DEBUG");

	if (cycles)
		lightrec_begin_cycles = (unsigned int) strtol(cycles, NULL, 0);
#endif

	lightrec_state = lightrec_init(LIGHTREC_PROG_NAME,
			lightrec_map, ARRAY_SIZE(lightrec_map),
			&lightrec_ops);

	// fprintf(stderr, "M=0x%lx, P=0x%lx, R=0x%lx, H=0x%lx\n",
	// 		(uintptr_t) psxRegs.ptrs.psxM,
	// 		(uintptr_t) psxRegs.ptrs.psxP,
	// 		(uintptr_t) psxRegs.ptrs.psxR,
	// 		(uintptr_t) psxRegs.ptrs.psxH);

#ifndef _WIN32
	signal(SIGPIPE, exit);
#endif
	lightrec_plugin_apply_config();
	return 0;
}

static u32 do_calculate_hash(const void *buffer, u32 count, u32 needle, bool le)
{
	unsigned int i;
	const u32 *data = (const u32 *) buffer;
	u32 hash = needle;

	count /= 4;
	for(i = 0; i < count; ++i) {
		hash += le ? LE32TOH(data[i]) : data[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
}

static u32 hash_calculate_le(const void *buffer, u32 count)
{
	return do_calculate_hash(buffer, count, 0xffffffff, true);
}

u32 hash_calculate(const void *buffer, u32 count)
{
	return do_calculate_hash(buffer, count, 0xffffffff, false);
}

static u32 hash_calculate_ram(const void *buffer, u32 ram_size)
{
	u32 hash;

	if (ram_disabled)
		hash = hash_calculate_le(cache_buf, sizeof(cache_buf));
	else
		hash = hash_calculate_le(buffer, sizeof(cache_buf));

	return do_calculate_hash(buffer + sizeof(cache_buf),
				 ram_size - sizeof(cache_buf),
				 hash, true);
}

static const char * const mips_regs[] = {
	"zero",
	"at",
	"v0", "v1",
	"a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"t8", "t9",
	"k0", "k1",
	"gp", "sp", "fp", "ra",
	"lo", "hi",
};

static void print_for_big_ass_debugger(void)
{
	struct lightrec_registers *regs;
	unsigned int i;

	regs = lightrec_get_registers(lightrec_state);

	printf("CYCLE 0x%08x PC 0x%08x", psxRegs.cycle, psxRegs.pc);

	if (lightrec_very_debug)
		printf(" RAM 0x%08x SCRATCH 0x%08x HW 0x%08x",
				hash_calculate_ram(psxRegs.ptrs.psxM, 0x200000),
				hash_calculate_le(psxRegs.ptrs.psxH, 0x400),
				hash_calculate_le(psxRegs.ptrs.psxH + 0x1000, 0x2000));

	printf(" CP0 0x%08x CP2D 0x%08x CP2C 0x%08x INT 0x%04x INTCYCLE 0x%08x GPU 0x%08x",
			hash_calculate(regs->cp0, sizeof(regs->cp0)),
			hash_calculate(regs->cp2d, sizeof(regs->cp2d)),
			hash_calculate(regs->cp2c, sizeof(regs->cp2c)),
			psxRegs.interrupt,
			hash_calculate(psxRegs.intCycle, sizeof(psxRegs.intCycle)),
			LE32TOH(HW_GPU_STATUS));

	if (lightrec_very_debug) {
		for (i = 0; i < 32; i++)
			printf(" CP2D%u 0x%08x", i, regs->cp2d[i]);
		for (i = 0; i < 32; i++)
			printf(" CP2C%u 0x%08x", i, regs->cp2c[i]);
	}

	if (lightrec_very_debug)
		for (i = 0; i < 34; i++)
			printf(" %s 0x%08x", mips_regs[i], regs->gpr[i]);
	else
		printf(" GPR 0x%08x",
		       hash_calculate(regs->gpr, sizeof(regs->gpr)));
	printf("\n");

	fflush(stdout);
}

static void lightrec_plugin_sync_regs_to_pcsx(bool need_cp2);
static void lightrec_plugin_sync_regs_from_pcsx(bool need_cp2);

static void lightrec_plugin_execute_internal(bool block_only)
{
	struct lightrec_registers *regs;
	u32 flags, cycles_pcsx;
	u32 old_pc = psxRegs.pc;

	regs = lightrec_get_registers(lightrec_state);
	plugin_where = 1;
	gen_interupt((psxCP0Regs *)regs->cp0);
	plugin_where = 0;
	if (!block_only && psxRegs.stop)
		return;

	cycles_pcsx = psxRegs.next_interupt - psxRegs.cycle;
	assert((s32)cycles_pcsx > 0);

	// step during early boot so that 0x80030000 fastboot hack works
	// must not use 0 cycles to ensure progress
	block_stepping = block_only;
	if (block_only)
		cycles_pcsx = 1;

	if (use_pcsx_interpreter) {
		psxInt.ExecuteBlock(&psxRegs, 0);
	} else {
		u32 cycles_lightrec = cycles_pcsx * 1024;
		if (unlikely(use_lightrec_interpreter)) {
			psxRegs.pc = lightrec_run_interpreter(lightrec_state,
							      psxRegs.pc,
							      cycles_lightrec);
		} else {
			plugin_where = 2;
			psxRegs.pc = lightrec_execute(lightrec_state,
						      psxRegs.pc, cycles_lightrec);
			plugin_where = 3;
		}

		lightrec_transition_to_pcsx(lightrec_state);

		flags = lightrec_exit_flags(lightrec_state);

		if (flags & LIGHTREC_EXIT_SEGFAULT) {
			fprintf(stderr, "Exiting at cycle 0x%08x\n",
				psxRegs.cycle);
			/* stderr goes nowhere on some hosts (PS3): say it in the log too */
			SysPrintf("lightrec: cannot go on at pc %08x (cycle %08x), exiting\n",
				  psxRegs.pc, psxRegs.cycle);
			if (lightrec_debug)
				print_for_big_ass_debugger();
			exit(1);
		}
		else if (flags & LIGHTREC_EXIT_SYSCALL)
			psxException(R3000E_Syscall << 2, 0, (psxCP0Regs *)regs->cp0);
		else if (flags & LIGHTREC_EXIT_BREAK)
			psxException(R3000E_Bp << 2, 0, (psxCP0Regs *)regs->cp0);
		else if (flags & LIGHTREC_EXIT_UNKNOWN_OP) {
			u32 op = intFakeFetch(psxRegs.pc);
			u32 hlec = op & 0x03ffffff;
			if ((op >> 26) == 0x3b && hlec < ARRAY_SIZE(psxHLEt) && Config.HLE) {
				lightrec_plugin_sync_regs_to_pcsx(0);
				psxRegs.pc += 4; // match psxinterpreter
				psxHLEt[hlec]();
				lightrec_plugin_sync_regs_from_pcsx(0);
			}
			else
				psxException(R3000E_RI << 2, 0, (psxCP0Regs *)regs->cp0);
		}
		else if (psxRegs.biosFuncsHooked && ((psxRegs.pc & ~0xa0000010) == 0xa0 ||
			 (psxRegs.pc & ~0xa0000000) == 0xc0)) {
			// this is rather costly, but hooking should be off in most cases
			lightrec_plugin_sync_regs_to_pcsx(0);
			psxBiosJumpTest(&psxRegs);
			lightrec_plugin_sync_regs_from_pcsx(0);
		}
	}

	if (lightrec_debug && psxRegs.cycle >= lightrec_begin_cycles && psxRegs.pc != old_pc) {
		print_for_big_ass_debugger();
	}

	if ((regs->cp0[13] & regs->cp0[12] & 0x300) && (regs->cp0[12] & 0x1)) {
		/* Handle software interrupts */
		regs->cp0[13] &= ~0x7c;
		psxException(regs->cp0[13], 0, (psxCP0Regs *)regs->cp0);
	}
	plugin_where = 0;
}

static void lightrec_plugin_execute(psxRegisters *regs)
{
	while (!regs->stop)
		lightrec_plugin_execute_internal(lightrec_very_debug);
}

static void lightrec_plugin_execute_block(psxRegisters *regs,
	enum blockExecCaller caller)
{
	lightrec_plugin_execute_internal(true);
}

static void lightrec_plugin_clear(u32 addr, u32 size)
{
	if ((addr == 0 && size == UINT32_MAX)
	    || (lightrec_hacks & LIGHTREC_OPT_INV_DMA_ONLY))
		lightrec_invalidate_all(lightrec_state);
	else
		/* size * 4: PCSX uses DMA units */
		lightrec_invalidate(lightrec_state, addr, size * 4);
}

static void lightrec_plugin_notify(enum R3000Anote note, void *data)
{
	switch (note)
	{
	case R3000ACPU_NOTIFY_CACHE_ISOLATED:
	case R3000ACPU_NOTIFY_CACHE_UNISOLATED:
		/* not used, lightrec calls lightrec_enable_ram() instead */
		break;
	case R3000ACPU_NOTIFY_BEFORE_SAVE:
		/* non-null 'data' means this is HLE related sync */
		lightrec_plugin_sync_regs_to_pcsx(data == NULL);
		break;
	case R3000ACPU_NOTIFY_AFTER_LOAD:
	case R3000ACPU_NOTIFY_AFTER_LOAD_STATE:
		lightrec_plugin_sync_regs_from_pcsx(data == NULL);
		if (data == NULL)
			lightrec_invalidate_all(lightrec_state);
		break;
	}
}

static void lightrec_plugin_apply_config()
{
	static u32 cycles_per_op_old;
	u32 cycle_mult = Config.cycle_multiplier_override && Config.cycle_multiplier == CYCLE_MULT_DEFAULT
		? Config.cycle_multiplier_override : Config.cycle_multiplier;
	u32 cycles_per_op = cycle_mult * 1024 / 100;
	assert(cycles_per_op);

	if (cycles_per_op_old && cycles_per_op_old != cycles_per_op) {
		SysPrintf("lightrec: reinit block cache for cycles_per_op %.2f\n",
			cycles_per_op / 1024.f);
	}
	cycles_per_op_old = cycles_per_op;
	lightrec_set_cycles_per_opcode(lightrec_state, cycles_per_op);

	lightrec_set_unsafe_opt_flags(lightrec_state, lightrec_hacks);
	intApplyConfig();
}

static void lightrec_plugin_shutdown(void)
{
	lightrec_destroy(lightrec_state);
	lightrec_state = NULL;

	if (!LIGHTREC_CUSTOM_MAP) {
#if defined(__lv2ppu__)
		/* static buffer in .text: nothing to free */
#elif P_HAVE_MMAP
		munmap(code_buffer, CODE_BUFFER_SIZE);
#else
		free(code_buffer);
#endif
	}
}

#if LIGHTREC_THREADED_RUNTIME && LIGHTREC_ENABLE_THREADED_COMPILER
int lightrec_threaded_compiler = 1;

/* The front end's setting, before a game is loaded: Lightrec compiles new blocks on
 * a second thread (they run in Lightrec's interpreter until compiled, so a burst of
 * new code no longer holds up the frame), or on the emulation thread. Lightrec is
 * restarted when it changes; the game's reset rebuilds its blocks anyway. */
void lightrec_plugin_set_threaded(int on)
{
	on = !!on;
	if (on == lightrec_threaded_compiler)
		return;
	if (lightrec_state && psxCpu == &psxRec) {
		lightrec_plugin_shutdown();
		lightrec_threaded_compiler = on;
		lightrec_plugin_init();
	} else {
		lightrec_threaded_compiler = on;
	}
	SysPrintf("lightrec: blocks compiled %s\n", on ? "on a second thread" : "on the emulation thread");
}
#endif

static void lightrec_plugin_reset(void)
{
	struct lightrec_registers *regs;

	regs = lightrec_get_registers(lightrec_state);

	/* Invalidate all blocks */
	lightrec_invalidate_all(lightrec_state);

	/* Reset registers */
	memset(regs, 0, sizeof(*regs));

	regs->cp0[12] = 0x10900000; // COP0 enabled | BEV = 1 | TS = 1
	regs->cp0[15] = 0x00000002; // PRevID = Revision ID, same as R3000A
}

static void lightrec_plugin_sync_regs_from_pcsx(bool need_cp2)
{
	struct lightrec_registers *regs;

	regs = lightrec_get_registers(lightrec_state);
	memcpy(regs->gpr, &psxRegs.GPR, sizeof(regs->gpr));
	memcpy(regs->cp0, &psxRegs.CP0, sizeof(regs->cp0));
	if (need_cp2)
		memcpy(regs->cp2d, &psxRegs.CP2, sizeof(regs->cp2d) + sizeof(regs->cp2c));
}

static void lightrec_plugin_sync_regs_to_pcsx(bool need_cp2)
{
	struct lightrec_registers *regs;

	regs = lightrec_get_registers(lightrec_state);
	memcpy(&psxRegs.GPR, regs->gpr, sizeof(regs->gpr));
	memcpy(&psxRegs.CP0, regs->cp0, sizeof(regs->cp0));
	if (need_cp2)
		memcpy(&psxRegs.CP2, regs->cp2d, sizeof(regs->cp2d) + sizeof(regs->cp2c));
}

R3000Acpu psxRec =
{
	lightrec_plugin_init,
	lightrec_plugin_reset,
	lightrec_plugin_execute,
	lightrec_plugin_execute_block,
	lightrec_plugin_clear,
	lightrec_plugin_notify,
	lightrec_plugin_apply_config,
	lightrec_plugin_shutdown,
};

/* front-end hang reports: native code of the block at pc / the dispatcher */
int lightrec_plugin_debug_code(u32 pc, int which, const void **code, unsigned int *size,
			       unsigned int *nb_ops, unsigned int *flags)
{
	if (!lightrec_state)
		return 0;
	return lightrec_debug_code(lightrec_state, pc, which, code, size, nb_ops, flags);
}

int lightrec_plugin_debug_info(char *b, int n)
{
	static const char *const names[] = { "idle", "events", "dynarec", "after the dynarec" };
	int k = snprintf(b, n, "; plugin at %s", names[plugin_where & 3]);
	if (lightrec_state && k > 0 && k < n)
		k += lightrec_debug_info(lightrec_state, b + k, n - k);
	return k;
}

void lightrec_plugin_debug_regs(u32 *gpr34, u32 *cp0_3)
{
	if (lightrec_state)
		lightrec_debug_regs(lightrec_state, gpr34, cp0_3);
}

void lightrec_plugin_debug_trace(u32 *pc, u32 *count)
{
	*pc = *count = 0;
	if (lightrec_state)
		lightrec_debug_trace(lightrec_state, pc, count);
}
