/*
 * prompt.c -- the debugger's command line (intvdebug_command) and its Tab
 * completion. Every command is a thin layer over the public API, so a
 * window's buttons and the prompt can never disagree.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "debugger_internal.h"
#include "intvdebug.h"
#include "intvmemtools.h"
#include "intvvidtab.h"
#include "srcmap.h"
#include "symbols.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void emit(outbuf *buf, const char *fmt, ...)
{
    va_list args;
    int wrote;
    if (buf->len >= buf->cap - 1)
        return;
    va_start(args, fmt);
    wrote = vsnprintf(buf->dst + buf->len, (size_t)(buf->cap - buf->len), fmt, args);
    va_end(args);
    if (wrote > 0)
        buf->len += wrote < buf->cap - buf->len ? wrote : buf->cap - buf->len - 1;
}

static const char *const commands[] = {
    "help", "step", "over", "out", "line", "overline", "lineout", "run", "stop", "runto",
    "break", "bpr", "bpw", "bpa", "delete", "enable", "disable", "clear", "hits", "breaks",
    "print", "regs", "set", "mem", "poke", "disasm", "trace", "watch", "unwatch", "find",
    "symbols", "label", "labels", "source", "locals", "map", "io", "cart", "sound", "video", "save",
    NULL,
};

static const char *const save_kinds[] = { "ram", "gram", "regs", "trace", NULL };

static const char *const help_text =
    "step [n]               run n instructions (F7)\n"
    "over | out             step over a JSR / run to the routine's return (F8, Shift+F8)\n"
    "line | overline | lineout   the same by IntyBASIC line\n"
    "run | stop             go / break\n"
    "runto <addr>           run until PC = addr\n"
    "break <addr> [cond]    execute breakpoint\n"
    "bpr | bpw | bpa <addr>[-<end>] [cond]   read / write / read-or-write breakpoint\n"
    "delete <id> | enable <id> | disable <id> | clear | hits | breaks\n"
    "print <expr>           e.g. print r5, print [$0200], print sp+1\n"
    "regs                   CPU registers and flags\n"
    "set <reg> <expr>       e.g. set pc $5000, set r0 0, set c 1\n"
    "mem <addr> [len]       dump words (side-effect free)\n"
    "poke <addr> <w...>     write words\n"
    "disasm [addr] [n]\n"
    "trace [on|off|clear|n] record / show the last n instructions\n"
    "watch [addr|label|var|lo-hi] | unwatch <n|all>   the watch list\n"
    "find [hex] <words> | find text <string>   search memory (stopped)\n"
    "symbols [text] | label <addr> <name> | labels <file>\n"
    "source | locals        the IntyBASIC line at the PC / the program's variables\n"
    "map | io | cart | sound   the memory map, controllers and ports, cartridge, PSGs\n"
    "video [section]          the STIC state: status, regs, colours, lines, timing\n"
    "save <ram|gram|regs|trace> <file>\n"
    "Expressions: $5000 0x5000 5000h 20480 %1010; r0-r7 pc sp, flags s z o c i d;\n"
    "[addr] is the word at addr; labels; frame cycles; in a condition value, addr\n"
    "and old name the access (old: the word before a write); C operators.\n"
    "The scratchpad, GROM and GRAM hold 8 bits; everything else is a 16-bit word.\n";

static char *next_tok(char **cursor)
{
    char *scan = *cursor, *start;
    while (*scan && isspace((unsigned char)*scan))
        scan++;
    if (!*scan) {
        *cursor = scan;
        return NULL;
    }
    start = scan;
    while (*scan && !isspace((unsigned char)*scan))
        scan++;
    if (*scan)
        *scan++ = '\0';
    *cursor = scan;
    return start;
}

static char *rest_of(char *cursor)
{
    while (*cursor && isspace((unsigned char)*cursor))
        cursor++;
    return cursor;
}

static int eval(intvdebug *dbg, outbuf *buf, const char *text, long *value)
{
    char err[96];
    if (intvdebug_eval(dbg, text, value, err, sizeof err) != 0) {
        emit(buf, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/* "<addr>" or "<addr>-<end>" */
static int range(intvdebug *dbg, outbuf *buf, char *token, long *first, long *last)
{
    char *dash = strchr(token + 1, '-');
    if (dash) {
        *dash = '\0';
        if (eval(dbg, buf, token, first) || eval(dbg, buf, dash + 1, last))
            return -1;
        return 0;
    }
    if (eval(dbg, buf, token, first))
        return -1;
    *last = *first;
    return 0;
}

/* Waits (bounded) for the stop that follows a step-like command. */
static void wait_stopped(intvdebug *dbg, uint64_t serial)
{
    int tries = 0;
    while ((intvdebug_stop_serial(dbg) == serial || !intvdebug_is_paused(dbg)) && tries++ < 3000) {
        struct timespec pause = { 0, 1000000L };
        nanosleep(&pause, NULL);
    }
}

static void show_regs(intvdebug *dbg, outbuf *buf)
{
    intvdebug_regs regs;
    int reg;
    if (!intvdebug_regs_get(dbg, &regs)) {
        emit(buf, "the machine is running (stop first)\n");
        return;
    }
    for (reg = 0; reg < 8; reg++)
        emit(buf, "R%d %04X%s", reg, regs.r[reg], reg % 4 == 3 ? "\n" : "  ");
    emit(buf, "S%d Z%d O%d C%d I%d D%d%s  %s%s%s\n", regs.S, regs.Z, regs.O, regs.C, regs.I, regs.D,
         regs.D ? " (SDBD pending)" : "", regs.interruptible ? "interruptible" : "not interruptible",
         regs.intrq ? "  INTRQ" : "", regs.busrq ? "  BUSRQ" : "");
    emit(buf, "frame %u  cycles %llu\n", regs.frame, (unsigned long long)regs.cycles);
}

/* A value in the width of the word it came from. */
static void emit_word(outbuf *buf, intvdebug *dbg, uint16_t addr, uint16_t value)
{
    if (intvdebug_word_width(dbg, addr) == 8)
        emit(buf, "%02X", value & 0xFF);
    else
        emit(buf, "%04X", value);
}

static void dump(outbuf *buf, intvdebug *dbg, uint16_t addr, const uint16_t *words, int count)
{
    int row, col;
    for (row = 0; row < count; row += 8) {
        emit(buf, "%04X ", (uint16_t)(addr + row));
        for (col = 0; col < 8 && row + col < count; col++) {
            emit(buf, " ");
            emit_word(buf, dbg, (uint16_t)(addr + row + col), words[row + col]);
        }
        emit(buf, "\n");
    }
}

static int reg_by_name(const char *name)
{
    static const struct { const char *name; int reg; } map[] = {
        { "pc", INTVDEBUG_REG_PC }, { "sp", INTVDEBUG_REG_SP }, { "s", INTVDEBUG_REG_S },
        { "z", INTVDEBUG_REG_Z }, { "o", INTVDEBUG_REG_O }, { "c", INTVDEBUG_REG_C },
        { "i", INTVDEBUG_REG_I }, { "d", INTVDEBUG_REG_D },
    };
    unsigned idx;
    if (name[0] == 'r' && name[1] >= '0' && name[1] <= '7' && !name[2])
        return INTVDEBUG_REG_R0 + (name[1] - '0');
    for (idx = 0; idx < sizeof map / sizeof map[0]; idx++)
        if (!strcmp(map[idx].name, name))
            return map[idx].reg;
    return -1;
}

static void list_breaks(intvdebug *dbg, outbuf *buf)
{
    intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
    const int count = intvdebug_bp_list(dbg, bps, INTVDEBUG_MAX_BREAKPOINTS);
    int idx;
    if (!count) {
        emit(buf, "no breakpoints\n");
        return;
    }
    for (idx = 0; idx < count; idx++) {
        char text[160];
        intvbp_describe(&bps[idx], intvsymtab_shared(), text, sizeof text);
        emit(buf, "#%d %s%s  hits %u\n", bps[idx].id, text, bps[idx].enabled ? "" : " (disabled)",
             bps[idx].hits);
    }
}

static void add_bp(intvdebug *dbg, outbuf *buf, int type, char *args)
{
    char *token = next_tok(&args);
    long first, last;
    intvdebug_bp spec;
    int id;

    if (!token) {
        emit(buf, "usage: <addr>[-<end>] [condition]\n");
        return;
    }
    if (range(dbg, buf, token, &first, &last))
        return;
    memset(&spec, 0, sizeof spec);
    spec.type = type;
    spec.lo = (uint16_t)first;
    spec.hi = (uint16_t)last;
    snprintf(spec.condition, sizeof spec.condition, "%s", rest_of(args));
    id = intvdebug_bp_add(dbg, &spec);
    if (id < 0)
        emit(buf, "error: the condition does not parse, or the table is full\n");
    else
        emit(buf, "breakpoint #%d\n", id);
}

static void show_disasm(intvdebug *dbg, outbuf *buf, uint16_t addr, int count)
{
    intvdebug_dasm_line lines[64];
    intvdebug_regs regs;
    int got, idx;
    if (!intvdebug_regs_get(dbg, &regs)) {
        emit(buf, "the machine is running (stop first)\n");
        return;
    }
    got = intvdebug_disassemble(dbg, addr, addr == regs.pc ? regs.D : 0, lines, count);
    for (idx = 0; idx < got; idx++) {
        char label[64], comment[96];
        if (intvsymtab_lookup_addr(intvsymtab_shared(), lines[idx].addr, label, sizeof label))
            emit(buf, "%s:\n", label);
        intvsym_annotate_line(intvsymtab_shared(), &lines[idx], comment, sizeof comment);
        emit(buf, "%c%c%04X  %-26s%s\n", lines[idx].addr == regs.pc ? '>' : ' ',
             intvdebug_breakpoint_state(dbg, lines[idx].addr) == 1 ? '*' : ' ', lines[idx].addr,
             lines[idx].text, comment);
    }
}

static void show_trace(intvdebug *dbg, outbuf *buf, int count)
{
    static intvdebug_hist entries[64];
    int got, idx;
    got = intvdebug_history_copy(dbg, entries, count);
    if (!got)
        emit(buf, "trace is %s and empty\n", intvdebug_history_enabled(dbg) ? "on" : "off");
    for (idx = 0; idx < got; idx++) {
        char text[200];
        intvdebug_history_format(&entries[idx], text, sizeof text);
        emit(buf, "%10llu f%-5u %s\n", (unsigned long long)entries[idx].cycle, entries[idx].frame, text);
    }
}

static void show_source(intvdebug *dbg, outbuf *buf)
{
    intvsrcmap *const map = intvsrcmap_shared();
    intvdebug_regs regs;
    int file, line, first, idx;

    if (!intvdebug_regs_get(dbg, &regs)) {
        emit(buf, "the machine is running (stop first)\n");
        return;
    }
    if (!map || !intvsrcmap_lookup(map, regs.pc, &file, &line)) {
        emit(buf, "no IntyBASIC line at $%04X (import the source in the BASIC tab)\n", regs.pc);
        return;
    }
    emit(buf, "%s\n", intvsrcmap_file_name(map, file));
    first = line > 3 ? line - 3 : 1;
    for (idx = first; idx <= line + 3 && idx <= intvsrcmap_line_count(map, file); idx++) {
        const char *text = intvsrcmap_line_text(map, file, idx);
        emit(buf, "%c%5d  %s\n", idx == line ? '>' : ' ', idx, text ? text : "");
    }
}

/* The program's variables: every var_NAME the compiler's symbols hold, with
 * its value in the right width. # variables are 16-bit. */
static void show_locals(intvdebug *dbg, outbuf *buf)
{
    intvsymtab *const table = intvsymtab_shared();
    const int total = table ? intvsymtab_count(table) : 0;
    int idx, shown = 0;

    if (!intvdebug_is_paused(dbg)) {
        emit(buf, "the machine is running (stop first)\n");
        return;
    }
    for (idx = 0; idx < total && shown < 64; idx++) {
        char name[48], shown_name[48];
        uint16_t addr, word;
        intvsym_var var;
        int pos;
        if (!intvsymtab_enum(table, idx, &addr, name, sizeof name, NULL, 0) || strncmp(name, "var_", 4))
            continue;
        for (pos = 0; name[4 + pos] && pos < (int)sizeof(shown_name) - 1; pos++)
            shown_name[pos] = name[4 + pos] == '&' ? '#' : (char)tolower((unsigned char)name[4 + pos]);
        shown_name[pos] = '\0';
        if (!intvsym_resolve_basic(table, shown_name, &var) || intvdebug_read(dbg, var.addr, &word, 1) != 1)
            continue;
        if (var.width == 8)
            emit(buf, "%-16s $%04X  $%02X (%u)\n", shown_name, var.addr, word & 0xFF, word & 0xFF);
        else
            emit(buf, "%-16s $%04X  $%04X (%u)\n", shown_name, var.addr, word, word);
        shown++;
    }
    if (!shown)
        emit(buf, "no IntyBASIC variables (import the program's .sym in the Symbols tab)\n");
}

static void show_map(intvdebug *dbg, outbuf *buf)
{
    intvdebug_region regions[64];
    int count, idx;
    if (!intvdebug_is_paused(dbg)) {
        emit(buf, "the machine is running (stop first)\n");
        return;
    }
    count = intvdebug_memory_map(dbg, regions, 64);
    for (idx = 0; idx < count; idx++)
        if (!regions[idx].readable && !regions[idx].writable)
            emit(buf, "$%04X-$%04X  %s\n", regions[idx].lo, regions[idx].hi, regions[idx].name);
        else
            emit(buf, "$%04X-$%04X  %s%s%s\n", regions[idx].lo, regions[idx].hi, regions[idx].name,
                 regions[idx].readable ? "" : " (no read)", regions[idx].writable ? "" : " (read only)");
}

static void show_pad(outbuf *buf, const intvdebug_pad *pad, const char *title)
{
    int line, side;
    if (!pad->present)
        return;
    emit(buf, "%s: ports $%02X $%02X%s\n", title, pad->side[0], pad->side[1],
         pad->output[0] || pad->output[1] ? " (output set)" : "");
    for (side = 0; side < 2; side++) {
        emit(buf, "  %s controller:", side ? "right" : "left");
        for (line = 0; line < 18; line++)
            if ((side ? pad->right : pad->left)[line])
                emit(buf, " %d", line);
        emit(buf, "\n");
    }
}

static void show_io(intvdebug *dbg, outbuf *buf)
{
    intvdebug_io ports;
    int row, seen = 0;
    if (!intvdebug_io_get(dbg, &ports)) {
        emit(buf, "the Intellivision is off\n");
        return;
    }
    show_pad(buf, &ports.pad[0], "pads");
    show_pad(buf, &ports.pad[1], "ECS pads");
    if (ports.ecs) {
        emit(buf, "ECS keyboard rows:");
        for (row = 0; row < 8; row++) {
            emit(buf, " %02X", ports.pad[1].keys[row] & 0xFF);
            seen |= ports.pad[1].keys[row] != 0;
        }
        emit(buf, "%s\n", seen ? "" : " (no key)");
        emit(buf, "ECS ROM pages: $2xxx %d, $7xxx %d, $Exxx %d\n", ports.ecs_page[0], ports.ecs_page[1],
             ports.ecs_page[2]);
    }
    if (ports.ivoice)
        emit(buf, "Intellivoice: LRQ %d  ALD %d  PC %03X  mode %X  page %X  FIFO %u..%u  %s%s\n",
             ports.ivoice_lrq, ports.ivoice_ald, ports.ivoice_pc & 0xFFF, ports.ivoice_mode, ports.ivoice_page,
             ports.ivoice_fifo_tail, ports.ivoice_fifo_head, ports.ivoice_halted ? "halted" : "running",
             ports.ivoice_fifo_sel ? ", from the FIFO" : "");
    if (ports.jlp)
        emit(buf, "JLP: accelerators %s, flash %s%s\n", ports.jlp_accel ? "on" : "off",
             ports.jlp_flash ? "open" : "none", ports.jlp_mb_hole ? ", FujiNet mailbox hole at $9C00-$9F3F" : "");
    if (ports.locutus)
        emit(buf, "Locutus mapper\n");
}

static void show_cart(intvdebug *dbg, outbuf *buf)
{
    char text[400];
    intvdebug_cart cart;
    intvdebug_cart_summary(dbg, text, sizeof text);
    emit(buf, "%s\n", text);
    intvdebug_cart_info(dbg, &cart);
    if (cart.path[0])
        emit(buf, "image: %s\n", cart.path);
    if (cart.fujinet_mailbox) {
        intvdebug_fujinet_state(dbg, text, sizeof text);
        emit(buf, "%s", text);
    }
}

static void show_sound(intvdebug *dbg, outbuf *buf)
{
    static const char voices[] = "ABC";
    int unit, voice;
    if (!intvdebug_is_paused(dbg)) {
        emit(buf, "the machine is running (stop first)\n");
        return;
    }
    for (unit = 0; unit < 2; unit++) {
        intvdebug_psg psg;
        if (!intvdebug_sound_get(dbg, unit, &psg))
            continue;
        emit(buf, "%s PSG\n", unit ? "ECS" : "Base");
        for (voice = 0; voice < 3; voice++)
            emit(buf, "  %c  period %03X  %7.1f Hz  %s%s  level %X%s\n", voices[voice], psg.period[voice],
                 psg.hz[voice], psg.tone_on[voice] ? "tone" : "----", psg.noise_on[voice] ? "+noise" : "",
                 psg.level[voice], psg.envelope[voice] ? " (envelope)" : "");
        emit(buf, "  noise period %02X  envelope period %04X shape %X level %X\n", psg.noise_period,
             psg.env_period, psg.env_shape, psg.env_level);
    }
}

static int write_file(const char *path, const void *data, size_t size, char *msg, int msgsz)
{
    FILE *const file = fopen(path, "wb");
    if (!file) {
        snprintf(msg, (size_t)msgsz, "cannot write %s", path);
        return -1;
    }
    if (fwrite(data, 1, size, file) != size) {
        fclose(file);
        snprintf(msg, (size_t)msgsz, "short write to %s", path);
        return -1;
    }
    fclose(file);
    snprintf(msg, (size_t)msgsz, "saved %lu bytes to %s", (unsigned long)size, path);
    return 0;
}

static void save_to(intvdebug *dbg, outbuf *buf, const char *kind, const char *path)
{
    char msg[300];
    if (!strcmp(kind, "ram") || !strcmp(kind, "gram")) {
        static uint16_t words[0x300];
        static uint8_t bytes[0x600];
        const int ram = !strcmp(kind, "ram");
        const uint16_t base = ram ? 0x0100 : 0x3800;
        const int count = ram ? 0x300 : 0x200;   /* scratchpad + system RAM words / the 64 GRAM cards */
        int idx;
        if (!intvdebug_is_paused(dbg)) {
            emit(buf, "the machine is running (stop first)\n");
            return;
        }
        if (intvdebug_read(dbg, base, words, count) != count) {
            emit(buf, "cannot read memory\n");
            return;
        }
        for (idx = 0; idx < count; idx++) {   /* RAM as little-endian words, GRAM as bytes */
            if (ram) {
                bytes[idx * 2] = (uint8_t)(words[idx] & 0xFF);
                bytes[idx * 2 + 1] = (uint8_t)(words[idx] >> 8);
            } else {
                bytes[idx] = (uint8_t)(words[idx] & 0xFF);
            }
        }
        write_file(path, bytes, (size_t)(ram ? count * 2 : count), msg, sizeof msg);
        emit(buf, "%s\n", msg);
    } else if (!strcmp(kind, "regs") || !strcmp(kind, "trace")) {
        char text[1 << 16];
        outbuf sub = { text, (int)sizeof text, 0 };
        if (!strcmp(kind, "regs")) {
            show_regs(dbg, &sub);
        } else {
            static intvdebug_hist entries[1024];
            const int got = intvdebug_history_copy(dbg, entries, 1024);
            int idx;
            for (idx = 0; idx < got; idx++) {
                char line[200];
                intvdebug_history_format(&entries[idx], line, sizeof line);
                emit(&sub, "%llu f%u %s\n", (unsigned long long)entries[idx].cycle, entries[idx].frame, line);
            }
        }
        write_file(path, text, (size_t)sub.len, msg, sizeof msg);
        emit(buf, "%s\n", msg);
    } else {
        emit(buf, "usage: save <ram|gram|regs|trace> <file>\n");
    }
}

static void do_find(intvdebug *dbg, outbuf *buf, char *args)
{
    char *token = next_tok(&args);
    int kind = INTVSEARCH_HEX, count, flags, found, idx;
    uint16_t pattern[64], mask, hits[32];
    char joined[160];
    const char *text = token;

    if (token && !strcmp(token, "text")) {
        kind = INTVSEARCH_TEXT;
        text = rest_of(args);
    } else if (token && !strcmp(token, "hex")) {
        text = rest_of(args);
    } else if (token) {
        snprintf(joined, sizeof joined, "%s %s", token, rest_of(args));
        text = joined;
    }
    count = text && *text ? intvsearch_parse(kind, text, pattern, &mask, &flags) : 0;
    if (count < 1) {
        emit(buf, "find [hex] <words> | find text <string>\n");
        return;
    }
    found = intvdebug_search(dbg, pattern, count, mask, flags, 0, 0xFFFF, hits, 32);
    if (found < 0)
        emit(buf, "stop the machine first\n");
    else if (!found)
        emit(buf, "not found\n");
    else
        for (idx = 0; idx < found; idx++)
            emit(buf, "$%04X\n", hits[idx]);
}

static void do_watch(intvdebug *dbg, outbuf *buf, char *text)
{
    int idx;
    if (*text && intvwatch_add(text) < 0)
        emit(buf, "no room for another watch\n");
    if (!intvwatch_count())
        emit(buf, "no watches (watch <addr|label|var|lo-hi>)\n");
    else
        for (idx = 0; idx < intvwatch_count(); idx++) {
            char line[200];
            intvwatch_format(dbg, intvsymtab_shared(), idx, line, sizeof line);
            emit(buf, "%2d  %s\n", idx, line);
        }
}

int intvdebug_command(intvdebug *dbg, const char *command, char *dst, int dstsz)
{
    char line[512];
    char *cursor = line, *cmd;
    outbuf buf = { dst, dstsz, 0 };
    long value, extra;

    if (!dbg || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    snprintf(line, sizeof line, "%s", command ? command : "");
    cmd = next_tok(&cursor);
    if (!cmd)
        return 0;
    for (char *chr = cmd; *chr; chr++)
        *chr = (char)tolower((unsigned char)*chr);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        emit(&buf, "%s", help_text);
    } else if (!strcmp(cmd, "step") || !strcmp(cmd, "s")) {
        char *token = next_tok(&cursor);
        value = 1;
        if (token && eval(dbg, &buf, token, &value))
            return buf.len;
        if (!intvdebug_is_paused(dbg)) {
            emit(&buf, "the machine is running (stop first)\n");
        } else {
            long count;
            for (count = 0; count < value && count < 100000; count++) {
                const uint64_t serial = intvdebug_stop_serial(dbg);
                intvdebug_step(dbg);
                wait_stopped(dbg, serial);
            }
        }
    } else if (!strcmp(cmd, "over") || !strcmp(cmd, "o")) {
        intvdebug_step_over(dbg);
    } else if (!strcmp(cmd, "out")) {
        intvdebug_step_out(dbg);
    } else if (!strcmp(cmd, "line")) {
        intvdebug_step_line(dbg);
    } else if (!strcmp(cmd, "overline")) {
        intvdebug_step_line_over(dbg);
    } else if (!strcmp(cmd, "lineout")) {
        intvdebug_step_line_out(dbg);
    } else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "g")) {
        intvdebug_resume(dbg);
    } else if (!strcmp(cmd, "stop")) {
        intvdebug_pause(dbg);
    } else if (!strcmp(cmd, "runto")) {
        char *token = next_tok(&cursor);
        if (!token || eval(dbg, &buf, token, &value))
            return buf.len;
        intvdebug_run_to(dbg, (uint16_t)value);
    } else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp")) {
        add_bp(dbg, &buf, INTVDEBUG_BP_EXEC, cursor);
    } else if (!strcmp(cmd, "bpr")) {
        add_bp(dbg, &buf, INTVDEBUG_BP_READ, cursor);
    } else if (!strcmp(cmd, "bpw")) {
        add_bp(dbg, &buf, INTVDEBUG_BP_WRITE, cursor);
    } else if (!strcmp(cmd, "bpa")) {
        add_bp(dbg, &buf, INTVDEBUG_BP_ACCESS, cursor);
    } else if (!strcmp(cmd, "delete") || !strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
        char *token = next_tok(&cursor);
        if (!token) {
            emit(&buf, "usage: %s <id>\n", cmd);
        } else {
            const int id = (int)strtol(token[0] == '#' ? token + 1 : token, NULL, 10);
            int done;
            if (!strcmp(cmd, "delete"))
                done = intvdebug_bp_remove(dbg, id);
            else
                done = intvdebug_bp_enable(dbg, id, !strcmp(cmd, "enable"));
            if (!done)
                emit(&buf, "no breakpoint #%d\n", id);
        }
    } else if (!strcmp(cmd, "clear")) {
        intvdebug_breakpoint_clear_all(dbg);
    } else if (!strcmp(cmd, "hits")) {
        intvdebug_bp_clear_hits(dbg);
    } else if (!strcmp(cmd, "breaks") || !strcmp(cmd, "bl")) {
        list_breaks(dbg, &buf);
    } else if (!strcmp(cmd, "print") || !strcmp(cmd, "p")) {
        if (eval(dbg, &buf, rest_of(cursor), &value) == 0) {
            char bin[17];
            int bit;
            for (bit = 0; bit < 16; bit++)
                bin[bit] = (char)('0' + ((value >> (15 - bit)) & 1));
            bin[16] = '\0';
            emit(&buf, "$%04lX  %ld  %%%s\n", (unsigned long)value & 0xffff, value, bin);
        }
    } else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r")) {
        show_regs(dbg, &buf);
    } else if (!strcmp(cmd, "set")) {
        char *name = next_tok(&cursor);
        const int reg = name ? reg_by_name(name) : -1;
        if (reg < 0) {
            emit(&buf, "usage: set <reg> <expr>   (r0-r7 pc sp s z o c i d)\n");
        } else if (eval(dbg, &buf, rest_of(cursor), &value) == 0) {
            if (!intvdebug_regs_set(dbg, reg, (unsigned)value))
                emit(&buf, "the machine is running (stop first)\n");
            else
                show_regs(dbg, &buf);
        }
    } else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m")) {
        char *first = next_tok(&cursor), *length = next_tok(&cursor);
        uint16_t words[128];
        extra = 64;
        if (!first || eval(dbg, &buf, first, &value) || (length && eval(dbg, &buf, length, &extra)))
            return buf.len;
        if (extra > 128)
            extra = 128;
        if (extra < 1 || intvdebug_read(dbg, (uint16_t)value, words, (int)extra) != (int)extra)
            emit(&buf, "the machine is running (stop first)\n");
        else
            dump(&buf, dbg, (uint16_t)value, words, (int)extra);
    } else if (!strcmp(cmd, "poke")) {
        char *first = next_tok(&cursor), *token;
        int count = 0;
        if (!first || eval(dbg, &buf, first, &value))
            return buf.len;
        if (!intvdebug_is_paused(dbg)) {
            emit(&buf, "the machine is running (stop first)\n");
            return buf.len;
        }
        while ((token = next_tok(&cursor)) != NULL) {
            uint16_t word;
            if (eval(dbg, &buf, token, &extra))
                return buf.len;
            word = (uint16_t)extra;
            intvdebug_write(dbg, (uint16_t)(value + count), &word, 1);
            count++;
        }
        emit(&buf, "%d word%s at $%04lX\n", count, count == 1 ? "" : "s", (unsigned long)value & 0xffff);
    } else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "d")) {
        char *first = next_tok(&cursor), *length = next_tok(&cursor);
        intvdebug_regs regs;
        value = intvdebug_regs_get(dbg, &regs) ? regs.pc : 0;
        extra = 16;
        if ((first && eval(dbg, &buf, first, &value)) || (length && eval(dbg, &buf, length, &extra)))
            return buf.len;
        show_disasm(dbg, &buf, (uint16_t)value, extra < 1 ? 1 : extra > 64 ? 64 : (int)extra);
    } else if (!strcmp(cmd, "trace")) {
        char *token = next_tok(&cursor);
        if (token && !strcmp(token, "on")) {
            intvdebug_history_enable(dbg, 1);
        } else if (token && !strcmp(token, "off")) {
            intvdebug_history_enable(dbg, 0);
        } else if (token && !strcmp(token, "clear")) {
            intvdebug_history_clear(dbg);
        } else {
            extra = 16;
            if (token && eval(dbg, &buf, token, &extra))
                return buf.len;
            show_trace(dbg, &buf, extra < 1 ? 1 : extra > 64 ? 64 : (int)extra);
        }
    } else if (!strcmp(cmd, "watch")) {
        do_watch(dbg, &buf, rest_of(cursor));
    } else if (!strcmp(cmd, "unwatch")) {
        char *token = next_tok(&cursor);
        if (token && !strcmp(token, "all"))
            intvwatch_clear();
        else if (token && eval(dbg, &buf, token, &value))
            return buf.len;
        else if (token)
            intvwatch_remove((int)value);
        else
            emit(&buf, "unwatch <n|all>\n");
    } else if (!strcmp(cmd, "find")) {
        do_find(dbg, &buf, cursor);
    } else if (!strcmp(cmd, "symbols")) {
        int picks[40], count, idx;
        count = intvdebug_symbols_filter(dbg, rest_of(cursor), picks, 40);
        if (!count)
            emit(&buf, "no symbols match\n");
        for (idx = 0; idx < count; idx++) {
            uint16_t addr;
            char name[64];
            if (intvsymtab_enum(intvsymtab_shared(), picks[idx], &addr, name, sizeof name, NULL, 0))
                emit(&buf, "$%04X  %s\n", addr, name);
        }
    } else if (!strcmp(cmd, "label")) {
        char *first = next_tok(&cursor), *name = next_tok(&cursor);
        if (!first || !name || eval(dbg, &buf, first, &value))
            emit(&buf, "usage: label <addr> <name>\n");
        else
            intvsymtab_add(intvsymtab_shared(), (uint16_t)value, name);
    } else if (!strcmp(cmd, "labels")) {
        char *path = rest_of(cursor);
        if (!*path)
            emit(&buf, "usage: labels <file.sym>\n");
        else
            emit(&buf, "%d labels loaded\n", intvsymtab_load(intvsymtab_shared(), path));
    } else if (!strcmp(cmd, "source")) {
        show_source(dbg, &buf);
    } else if (!strcmp(cmd, "locals")) {
        show_locals(dbg, &buf);
    } else if (!strcmp(cmd, "map")) {
        show_map(dbg, &buf);
    } else if (!strcmp(cmd, "io")) {
        show_io(dbg, &buf);
    } else if (!strcmp(cmd, "cart")) {
        show_cart(dbg, &buf);
    } else if (!strcmp(cmd, "sound")) {
        show_sound(dbg, &buf);
    } else if (!strcmp(cmd, "video")) {
        char text[4096];
        char *section = next_tok(&cursor);
        if (intvvid_tab_text_for(dbg, section, text, sizeof text) > 0)
            emit(&buf, "%s", text);
        else
            emit(&buf, "no video state yet\n");
    } else if (!strcmp(cmd, "save")) {
        char *kind = next_tok(&cursor);
        char *path = rest_of(cursor);
        if (!kind || !*path)
            emit(&buf, "usage: save <ram|gram|regs|trace> <file>\n");
        else
            save_to(dbg, &buf, kind, path);
    } else {
        emit(&buf, "unknown command '%s' (try help)\n", cmd);
    }
    intvdebug_touch(dbg);
    return buf.len;
}

int intvdebug_completions(intvdebug *dbg, const char *prefix, char *dst, int dstsz)
{
    outbuf buf = { dst, dstsz, 0 };
    const char *word;
    size_t len;
    int count = 0, idx;
    const int first_word = !strchr(prefix ? prefix : "", ' ');

    if (!dbg || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!prefix)
        prefix = "";
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    len = strlen(word);

    if (first_word) {
        for (idx = 0; commands[idx]; idx++)
            if (!strncmp(commands[idx], word, len)) {
                emit(&buf, "%s\n", commands[idx]);
                count++;
            }
        return count;
    }
    if (!strncmp(prefix, "save ", 5) && !strchr(prefix + 5, ' ')) {
        for (idx = 0; save_kinds[idx]; idx++)
            if (!strncmp(save_kinds[idx], word, len)) {
                emit(&buf, "%s\n", save_kinds[idx]);
                count++;
            }
        return count;
    }
    for (idx = 0; intvdebug_reg_names[idx]; idx++)
        if (len && !strncmp(intvdebug_reg_names[idx], word, len)) {
            emit(&buf, "%s\n", intvdebug_reg_names[idx]);
            count++;
        }
    if (len) {
        intvsymtab *const table = intvsymtab_shared();
        const int total = table ? intvsymtab_count(table) : 0;
        for (idx = 0; idx < total && count < 200; idx++) {
            char name[64];
            if (intvsymtab_enum(table, idx, NULL, name, sizeof name, NULL, 0) && !strncasecmp(name, word, len)) {
                emit(&buf, "%s\n", name);
                count++;
            }
        }
    }
    return count;
}
