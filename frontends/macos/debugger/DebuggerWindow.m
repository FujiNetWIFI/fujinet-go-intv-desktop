/*
 * Debugger window (AppKit): thirteen tabs (Console, Disassembly, BASIC,
 * Memory, Breakpoints, Watches, Cart & I/O, Trace, History, Video, Sound,
 * Symbols, FujiNet) over an editable register strip -- the same content and
 * scope as the GNOME/KDE/Windows debugger windows, built from plain AppKit
 * views (an NSTabView, NSTextView/NSTextField/NSButton children). The Video
 * tab is VideoTab.m, a view over core/include/intvvidtab.h; the old STIC
 * panes are its Decoded sources.
 *
 * A FujiNet tab, last, shows every FujiBus packet between the machine and
 * FujiNet (intvdebug_fujinet_trace, always on) beside the mailbox's state;
 * the timer also refreshes it twice a second while it shows, since it is
 * the one view that changes while the machine runs.
 *
 * Refreshing is a 0.1s NSTimer poll, not a callback: intvdebug.h has no
 * stop-callback mechanism at all (unlike the CoCo/MSX ports' own
 * cocodebug/msxdebug, which fire one on the emulator thread and marshal it
 * with dispatch_async) -- the engine stops the machine synchronously
 * inside cp1600_instr_tick on the emulator thread, and this window
 * watches intvdebug_stop_serial()/intvdebug_is_paused() on its own timer
 * instead, same reasoning the GNOME/KDE/Windows ports' own debugger
 * windows document.
 *
 * The symbol table (core/debugger/symbols.h) is not owned by intvdebug --
 * unlike msxdebug's own -- so this window uses the process-wide
 * intvsymtab_shared() instead of its own instance: a cart arrives through
 * FujiNet CONFIG with no path a .sym could ever be derived from, so a user
 * loading one by hand is the only way symbols get here at all, and that
 * work should survive closing this window, not be thrown away with it. It
 * ships pre-seeded with EXEC ROM/RAM and cart-header symbols (see
 * symbols.c); "Load Symbols..." loads a per-game as1600 .sym file on top
 * via NSOpenPanel, matching the GNOME/KDE/Windows ports' own UI. The
 * register strip and the Memory tab's Poke bar write the machine while it
 * is stopped (intvdebug_regs_set, the core's poke command).
 *
 * NOT BUILT OR RUN-VERIFIED -- see ../main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"
#import "VideoTab.h"
#import "../help/HelpWindow.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "intvdebug.h"
#include "intvmemtools.h"
#include "intvsession.h"
#include "memtools.h"
#include "srcmap.h"
#include "symbols.h"

#define DISASM_LINES 32
#define MEM_ROWS 16
#define MEM_WORDS_PER_ROW 8
#define FUJI_TICKS 5 /* the FujiNet tab, every 0.5s while it shows */
#define VIDEO_TICKS 3 /* the Video tab follows a running machine, every 0.3s */
#define REG_FIELDS 14 /* R0-R7, then S Z O C I D */

static DebuggerWindow *g_debugger;

/* The tabs, in order. The identifiers are the help contexts' ends
 * (debugger/<id>, intvhelp.h). INTV_DEBUGGER_TAB picks one by number, by the
 * start of its title or id (any case), or by an older name ("cpu", "stic"),
 * for tests and captures. */
static const char *const kTabNames[] = {"Console", "Disassembly", "BASIC", "Memory", "Breakpoints", "Watches",
                                        "Cart & I/O", "Trace", "History", "Video", "Sound", "Symbols", "FujiNet"};
static const char *const kTabIds[] = {"console", "disassembly", "basic", "memory", "breakpoints", "watches",
                                      "cart", "trace", "history", "video", "sound", "symbols", "fujinet"};
#define TAB_COUNT ((int)(sizeof(kTabNames) / sizeof(kTabNames[0])))
enum { MT_CONSOLE, MT_DISASM, MT_BASIC, MT_MEMORY, MT_BREAKS, MT_WATCHES, MT_CART, MT_TRACE, MT_HISTORY, MT_VIDEO,
       MT_SOUND, MT_SYMBOLS, MT_FUJINET };

static int tabFromText(const char *tab)
{
    if (!tab || !*tab)
        return -1;
    char *end;
    const long n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return n >= 0 && n < TAB_COUNT ? (int)n : -1;
    if (strcasecmp(tab, "cpu") == 0)
        return MT_DISASM;
    if (strcasecmp(tab, "stic") == 0)
        return MT_VIDEO;
    for (int idx = 0; idx < TAB_COUNT; idx++)
        if (strncasecmp(tab, kTabNames[idx], strlen(tab)) == 0 || strncasecmp(tab, kTabIds[idx], strlen(tab)) == 0)
            return idx;
    return -1;
}

static int tabFromEnv(void)
{
    return tabFromText(getenv("INTV_DEBUGGER_TAB"));
}

/* Disassembly text view: a click toggles the breakpoint on the clicked
 * line. The address sits right after the two flag columns (refreshDisasm's
 * "%c%c%04X  ..." format), matching the Windows/GNOME renderings. */
@interface DasmTextView : NSTextView
@property(nonatomic, copy) void (^onToggleAddr)(uint16_t addr);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    NSString *line = [text substringFromIndex:start];
    if (line.length >= 6) {
        unsigned addr = 0;
        NSScanner *scan =
            [NSScanner scannerWithString:[line substringWithRange:
                                                   NSMakeRange(2, 4)]];
        if ([scan scanHexInt:&addr] && addr <= 0xFFFF && self.onToggleAddr)
            self.onToggleAddr((uint16_t)addr);
    }
}
@end

/* Symbols tab text view: a click jumps the memory view to the clicked
 * row's address, which is the first 4 hex characters of each line
 * (refreshSymbols' "%04X  NAME  ; note" format) -- same click-parses-the-
 * line-prefix idiom as DasmTextView above, just column 0 instead of 2. */
@interface SymTextView : NSTextView
@property(nonatomic, copy) void (^onJumpAddr)(uint16_t addr);
@end

@implementation SymTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    NSString *line = [text substringFromIndex:start];
    if (line.length >= 4) {
        unsigned addr = 0;
        NSScanner *scan = [NSScanner
            scannerWithString:[line substringWithRange:NSMakeRange(0, 4)]];
        if ([scan scanHexInt:&addr] && addr <= 0xFFFF && self.onJumpAddr)
            self.onJumpAddr((uint16_t)addr);
    }
}
@end

/* BASIC tab text view: a click toggles the breakpoint on the clicked source
 * line. The view shows one file, a line per text line, so the line number is
 * the clicked line's position. */
@interface BasTextView : NSTextView
@property(nonatomic, copy) void (^onToggleLine)(int line);
@end

@implementation BasTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    const NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    int line = 1;
    for (NSUInteger i = 0; i < start; i++)
        if ([text characterAtIndex:i] == '\n')
            line++;
    if (self.onToggleLine)
        self.onToggleLine(line);
}
@end

/* A list of lines the user picks one of by clicking (the Breakpoints and
 * Watches tabs). The picked line is kept in `picked`, -1 for none, and
 * drawn with a background by whoever owns the text. */
@interface PickTextView : NSTextView
@property(nonatomic) int picked;
@property(nonatomic, copy) void (^onPick)(int line);
@end

@implementation PickTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    const NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    int line = 0;
    for (NSUInteger i = 0; i < start; i++)
        if ([text characterAtIndex:i] == '\n')
            line++;
    self.picked = line;
    if (self.onPick)
        self.onPick(line);
}
@end

@interface DebuggerWindow () <NSWindowDelegate, NSTabViewDelegate, NSTextFieldDelegate>
- (instancetype)initWithSession:(intvsession *)session;
@end

@implementation DebuggerWindow {
    intvsession *_session;
    intvdebug *_dbg;
    intvsymtab *_symtab;
    intvsrcmap *_srcmap;
    NSWindow *_window;

    /* Console tab */
    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_conHistory;
    NSInteger _conPos;

    /* Video and Sound tabs */
    IntvVideoTab *_video;
    NSTextView *_soundText;
    int _videoTicks;

    /* The register strip under every tab: R0-R7, then S Z O C I D */
    NSTextField *_reg[REG_FIELDS];
    NSTextField *_regInfo;

    /* Memory tab: the Poke bar, the highlighted words, and the search */
    NSTextField *_memPoke;
    NSTextField *_memNote;
    BOOL _memHl;
    uint16_t _memHlLo, _memHlHi;
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry;
    NSTextField *_srchNote;

    /* Cart tab; Trace (0, live) and History (1, when stopped) share one ring */
    NSTextView *_cartText;
    NSButton *_hRec[2];
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];
    uint64_t _hSeen[2];
    int _hTicks;

    /* Tabs peeled off into windows of their own */
    NSTabViewItem *_items[16];
    NSWindow *_popped[16];

    /* Breakpoints tab */
    NSPopUpButton *_bpType;
    NSTextField *_bpAddr, *_bpCondition, *_bpStop;
    PickTextView *_bpList;
    NSMutableArray<NSNumber *> *_bpIds;
    unsigned _bpGen;

    /* Watches tab */
    NSTextField *_wtEntry;
    PickTextView *_wtList;

    /* BASIC tab: the imported IntyBASIC source (core/debugger/srcmap.h) */
    NSTabViewItem *_basicItem;
    NSTextField *_basNote;
    NSPopUpButton *_basFiles;
    BasTextView *_basView;
    int _basFile;       /* index into the map; -1 = nothing shown */
    unsigned _basGen;   /* intvsrcmap_generation() the view was built from */
    int _basPcLine;     /* the machine's line in the file shown, 0 = none */
    uint64_t _basSerial; /* the stop the view last followed */

    NSButton *_runBtn;
    NSTextField *_status;
    DasmTextView *_disasm;

    NSTextField *_memAddr;
    NSTextView *_memView;
    uint16_t _memBase;
    NSTextField *_breakAddr;

    SymTextView *_symList;
    NSTextField *_symFilter;
    NSTextField *_symCount;
    unsigned _symSeenGen;
    NSString *_symFilterText;

    NSTabView *_tabs;
    NSTabViewItem *_fujiItem;
    NSTextField *_fujiNote;
    NSTextView *_fujiText;
    NSTextView *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount; /* seeded -1: the first refresh builds the log */
    int _fujiTicks;

    NSTimer *_tick;
    uint64_t _seenSerial;
    unsigned _seenGen;  /* intvdebug_generation: a command ran, a breakpoint changed */
    int _wasPaused; /* seeded -1: force the first tick to refresh */
}

/* The current line is a bar in the accent (the icon's green) with white text;
 * a breakpoint is a red dot in the gutter's first column. */
static NSColor *rgbColor(unsigned rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1];
}

/* The accent as a fill (the icon's green), the ink on that fill, and the
 * accent as text or lines on a dark theme (the green fill is too dark there). */
static NSColor *accentColor(void)
{
    return rgbColor(INTVSESSION_ACCENT_RGB);
}

static NSColor *accentInk(void)
{
    return rgbColor(INTVSESSION_ACCENT_INK_RGB);
}

static NSColor *accentLight(void)
{
    return rgbColor(INTVSESSION_ACCENT_LIGHT_RGB);
}

static NSColor *breakpointRed(void)
{
    return [NSColor colorWithSRGBRed:0xE0 / 255.0 green:0x1B / 255.0 blue:0x24 / 255.0 alpha:1];
}

/* Appends one source/disassembly line to `out`: gutter dot, text, newline. */
static void appendLine(NSMutableAttributedString *out, NSString *body, BOOL pc, int bp)
{
    NSFont *font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    NSString *line = [NSString stringWithFormat:@"%@ %@\n", bp == 1 ? @"\u25CF" : bp == 2 ? @"\u25CB" : @" ", body];
    NSMutableAttributedString *attr = [[NSMutableAttributedString alloc]
        initWithString:line
            attributes:@{ NSFontAttributeName : font, NSForegroundColorAttributeName : NSColor.textColor }];
    if (pc) {
        [attr addAttribute:NSBackgroundColorAttributeName value:accentColor() range:NSMakeRange(0, line.length)];
        [attr addAttribute:NSForegroundColorAttributeName value:accentInk() range:NSMakeRange(1, line.length - 1)];
    }
    if (bp)   /* enabled: red; disabled: attr hollow grey dot */
        [attr addAttribute:NSForegroundColorAttributeName
                  value:bp == 1 ? breakpointRed() : NSColor.systemGrayColor
                  range:NSMakeRange(0, 1)];
    [out appendAttributedString:attr];
}

/* Replaces a view's styled text where the reader left it. */
static void setAttrText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

+ (void)showForSession:(intvsession *)session
{
    if (!g_debugger) {
        g_debugger = [[DebuggerWindow alloc] initWithSession:session];
    } else {
        /* Reengage: closing hides rather than destroys (see
         * windowShouldClose:), and disengages on the way out. */
        intvdebug_set_engaged(g_debugger->_dbg, 1);
        intvdebug_pause(g_debugger->_dbg);
    }
    [g_debugger->_window makeKeyAndOrderFront:nil];
    for (int t = 0; t < TAB_COUNT; t++)
        [g_debugger->_popped[t] orderFront:nil];
}

- (instancetype)initWithSession:(intvsession *)session
{
    self = [super init];
    if (!self)
        return nil;
    _session = session;
    _dbg = intvdebug_get();
    _symtab = intvsymtab_shared();
    _srcmap = intvsrcmap_shared();
    _basFile = -1;
    _symFilterText = @"";
    _conHistory = [NSMutableArray array];
    _fujiCount = -1;
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self buildWindow];

    /* For captures and tests, like INTV_DEBUGGER_TAB: INTV_BASIC_IMPORT=<.bas
     * file or folder> imports it as Import File / Import Folder would, and
     * INTV_BASIC_BREAK=<name.bas>:<line> sets a breakpoint on that line. */
    {
        const char *imp = getenv("INTV_BASIC_IMPORT");
        const char *brk = getenv("INTV_BASIC_BREAK");
        if (imp && *imp)
            [self basicImported:[NSString stringWithUTF8String:imp] folder:NO];
        if (brk && strchr(brk, ':')) {
            NSString *spec = [NSString stringWithUTF8String:brk];
            NSString *name = [spec componentsSeparatedByString:@":"].firstObject;
            const int line = [[spec componentsSeparatedByString:@":"].lastObject intValue];
            for (int f = 0; f < intvsrcmap_file_count(_srcmap); f++) {
                const int addr = intvsrcmap_first_addr(_srcmap, f, line);
                if ([@(intvsrcmap_file_name(_srcmap, f)) isEqualToString:name] && addr >= 0 &&
                    !intvdebug_breakpoint_is_set(_dbg, (uint16_t)addr))
                    intvdebug_breakpoint_toggle(_dbg, (uint16_t)addr);
            }
        }
    }

    /* More captures: INTV_WATCH="x" adds a watch, INTV_WATCH_BREAK="x" a
     * breakpoint on a write that changes it, INTV_MEM_GOTO="x" shows it in
     * Memory, INTV_POPOUT=<tab> pops that tab out into a window. */
    {
        const char *watchText = getenv("INTV_WATCH"), *breakText = getenv("INTV_WATCH_BREAK"),
                   *gotoText = getenv("INTV_MEM_GOTO");
        uint16_t first, last;
        if (watchText && *watchText)
            intvwatch_add(watchText);
        if (breakText && *breakText && intvsym_parse_range(_symtab, breakText, &first, &last, NULL))
            [self breakOnChangeFrom:first to:last];
        if (gotoText && *gotoText && intvsym_parse_range(_symtab, gotoText, &first, &last, NULL))
            [self showInMemoryLo:first hi:last];
    }

    if (getenv("INTV_HISTORY"))   /* record from the start, for captures */
        intvdebug_history_enable(_dbg, 1);

    /* Engaging switches the CPU hook to the path that honours breakpoints
     * (see intvdebug.h). Pausing immediately is what a user opening a
     * debugger expects -- otherwise the window opens showing nothing. */
    intvdebug_set_engaged(_dbg, 1);
    intvdebug_pause(_dbg);

    __weak DebuggerWindow *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1
                                            repeats:YES
                                              block:^(NSTimer *t) {
                                                (void)t;
                                                [weakSelf onTick];
                                              }];
    _wasPaused = -1;

    {
        const int pop = tabFromText(getenv("INTV_POPOUT"));
        if (pop >= 0)
            [self popOutTab:pop];
    }
    return self;
}

- (void)onTick
{
    intvdebug_snapshot snap;
    intvdebug_stop_snapshot(_dbg, &snap);
    const unsigned gen = intvdebug_generation(_dbg);
    const BOOL paused = snap.paused != 0;
    if (snap.serial != _seenSerial || paused != _wasPaused || gen != _seenGen) {
        _seenSerial = snap.serial;
        _seenGen = gen;
        _wasPaused = paused;
        [self refreshAll:paused];
    } else if (_window.visible && !paused && [self tabVisible:MT_VIDEO] && ++_videoTicks >= VIDEO_TICKS) {
        _videoTicks = 0;   /* the picture follows attr running machine */
        [_video refreshForce:NO];
    } else if (_window.visible && [self tabVisible:MT_TRACE] && ++_hTicks >= 4) {
        _hTicks = 0;   /* the live view moves while the machine runs */
        [self refreshHist:0 force:NO];
    } else if (_window.visible && [self fujiShowing] &&
               ++_fujiTicks >= FUJI_TICKS) {
        /* the one view that moves while the machine runs */
        [self refreshFujinet:NO];
    }
}

/* ---- construction helpers -------------------------------------------------- */

static NSTextView *monoView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    NSTextView *view =
        [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11
                                            weight:NSFontWeightRegular];
    view.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* A log: no wrapping, so each hex row stays one line. */
static NSTextView *logView(NSScrollView **scrollOut)
{
    NSTextView *view = monoView(scrollOut);
    (*scrollOut).hasHorizontalScroller = YES;
    view.minSize = NSMakeSize(0, 0);
    view.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.verticallyResizable = YES;
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.size = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll)
        return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >=
           NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at =
            scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow)
        [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    return [NSButton buttonWithTitle:title target:self action:sel];
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

/* The Console: the core's command line (intvdebug_command). Tab completes,
 * Up and Down walk what was typed. */
- (NSView *)buildConsoleTab
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll);
    _conOut.string = @"Debugger console. Type 'help' for every command; Tab completes, Up/Down walk the history.\n";
    _conIn = [[NSTextField alloc] init];
    _conIn.placeholderString = @"command (help, step, break $5000, bpa $0200, print r3, mem $0200 32, video ...)";
    _conIn.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _conIn.target = self;
    _conIn.action = @selector(runCommand:);
    _conIn.delegate = self;

    NSStackView *root = [NSStackView stackViewWithViews:@[ outScroll, _conIn ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [outScroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_conIn.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [outScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return root;
}

/* The two AY-3-8914 PSGs, as text; only a stopped machine can be read. */
- (NSView *)buildSoundTab
{
    NSScrollView *scroll;
    _soundText = monoView(&scroll);
    NSStackView *root = [NSStackView stackViewWithViews:@[ scroll ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [scroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    return root;
}

/* The Video tab: a controller (core intvvidtab.h) behind its own views. */
- (NSView *)buildVideoTab
{
    _video = [[IntvVideoTab alloc] initWithDebugger:_dbg];
    NSView *view = _video.view;
    NSStackView *root = [NSStackView stackViewWithViews:@[ view ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [view.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [view.heightAnchor constraintEqualToAnchor:root.heightAnchor constant:-16].active = YES;
    return root;
}

- (NSView *)buildDisasmTab
{
    NSScrollView *dasmScroll = [[NSScrollView alloc] init];
    dasmScroll.hasVerticalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 900, 600)];
    _disasm.editable = NO;
    _disasm.richText = YES;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11
                                               weight:NSFontWeightRegular];
    _disasm.autoresizingMask = NSViewWidthSizable;
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onToggleAddr = ^(uint16_t addr) {
        DebuggerWindow *s = weakSelf;
        if (!s)
            return;
        intvdebug_breakpoint_toggle(s->_dbg, addr);
        s->_seenSerial = 0;
    };
    dasmScroll.documentView = _disasm;

    _breakAddr = [[NSTextField alloc] init];
    _breakAddr.placeholderString = @"Symbol or hex addr";
    _breakAddr.target = self;
    _breakAddr.action = @selector(breakAt:);
    NSButton *breakGo = [self button:@"Break" action:@selector(breakAt:)];
    NSStackView *breakRow = [NSStackView
        stackViewWithViews:@[ [self label:@"Break at"], _breakAddr, breakGo ]];
    breakRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSStackView *root = [NSStackView stackViewWithViews:@[ dasmScroll, breakRow ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [dasmScroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    return root;
}

/* The memory view, with a tab of its own. */
- (NSView *)buildMemoryTab
{
    _memAddr = [[NSTextField alloc] init];
    _memAddr.placeholderString = @"Address, symbol or BASIC variable";
    _memAddr.target = self;
    _memAddr.action = @selector(gotoMem:);
    NSButton *memGo = [self button:@"Go" action:@selector(gotoMem:)];
    NSStackView *memRow = [NSStackView stackViewWithViews:@[ _memAddr, memGo ]];
    memRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSScrollView *memScroll;
    _memView = monoView(&memScroll);
    _srchKind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_srchKind addItemsWithTitles:@[ @"Hex words", @"Text" ]];
    _srchKind.toolTip = @"Hex words (02A0 0268) or text, one character per word";
    _srchEntry = [[NSTextField alloc] init];
    _srchEntry.target = self;
    _srchEntry.action = @selector(searchNext:);
    _srchNote = [self label:@""];
    _srchNote.textColor = NSColor.secondaryLabelColor;
    NSStackView *searchRow = [NSStackView stackViewWithViews:@[
        [self label:@"Search"], _srchKind, _srchEntry,
        [self button:@"Find Next" action:@selector(searchNext:)],
        [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote
    ]];
    searchRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    _memPoke = [[NSTextField alloc] init];
    _memPoke.placeholderString = @"address value ... (hex; several values go to consecutive words)";
    _memPoke.target = self;
    _memPoke.action = @selector(pokeMem:);
    _memNote = [self label:@""];
    _memNote.textColor = NSColor.secondaryLabelColor;
    NSStackView *pokeRow = [NSStackView stackViewWithViews:@[
        [self label:@"Poke"], _memPoke, [self button:@"Write" action:@selector(pokeMem:)], _memNote
    ]];
    pokeRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSStackView *root = [NSStackView stackViewWithViews:@[ memRow, searchRow, memScroll, pokeRow ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [memRow.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [searchRow.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [memScroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [pokeRow.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [memScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return root;
}

- (NSView *)buildBasicTab
{
    NSButton *fileBtn = [self button:@"Import File…" action:@selector(basImportFile:)];
    fileBtn.toolTip = @"Load a .bas file and everything it INCLUDEs";
    NSButton *folderBtn = [self button:@"Import Folder…" action:@selector(basImportFolder:)];
    folderBtn.toolTip = @"Load every .bas file under a folder";
    NSButton *mapBtn = [self button:@"Load Map…" action:@selector(basLoadMap:)];
    mapBtn.toolTip = @"Load an as1600 -j source map, or a -l listing";
    _basFiles = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _basFiles.target = self;
    _basFiles.action = @selector(basFileChosen:);
    _basFiles.toolTip = @"The source file shown";
    NSStackView *bar = [NSStackView stackViewWithViews:@[ fileBtn, folderBtn, mapBtn, _basFiles ]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    _basNote = [self label:@""];
    _basNote.textColor = NSColor.secondaryLabelColor;
    _basNote.lineBreakMode = NSLineBreakByTruncatingTail;

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    _basView = [[BasTextView alloc] initWithFrame:NSMakeRect(0, 0, 900, 600)];
    _basView.editable = NO;
    _basView.richText = YES;
    _basView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _basView.minSize = NSMakeSize(0, 0);
    _basView.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _basView.verticallyResizable = YES;
    _basView.horizontallyResizable = YES;
    _basView.textContainer.widthTracksTextView = NO;
    _basView.textContainer.size = NSMakeSize(FLT_MAX, FLT_MAX);
    _basView.autoresizingMask = NSViewWidthSizable;
    __weak DebuggerWindow *weakSelf = self;
    _basView.onToggleLine = ^(int line) {
        DebuggerWindow *s = weakSelf;
        if (!s || s->_basFile < 0)
            return;
        const int addr = intvsrcmap_first_addr(s->_srcmap, s->_basFile, line);
        if (addr < 0) {
            s->_status.stringValue = @"No code on that line";
            return;
        }
        intvdebug_breakpoint_toggle(s->_dbg, (uint16_t)addr);
        s->_seenSerial = 0; /* redraw the marker immediately */
    };
    scroll.documentView = _basView;

    NSStackView *root = [NSStackView stackViewWithViews:@[ bar, _basNote, scroll ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [bar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_basNote.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [scroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    return root;
}

- (NSView *)buildSymbolsTab
{
    /* Filters on Enter, like _memAddr/_breakAddr above -- matching this
     * window's existing target/action convention rather than adding a
     * QLineEdit::textChanged-style live filter via NSTextFieldDelegate. */
    _symFilter = [[NSTextField alloc] init];
    _symFilter.placeholderString = @"Filter (Enter to apply)";
    _symFilter.target = self;
    _symFilter.action = @selector(symFilterChanged:);
    _symCount = [self label:@""];
    NSStackView *filterRow =
        [NSStackView stackViewWithViews:@[ _symFilter, _symCount ]];
    filterRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSScrollView *symScroll = [[NSScrollView alloc] init];
    symScroll.hasVerticalScroller = YES;
    _symList = [[SymTextView alloc] initWithFrame:NSMakeRect(0, 0, 560, 600)];
    _symList.editable = NO;
    _symList.richText = NO;
    _symList.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    _symList.autoresizingMask = NSViewWidthSizable;
    __weak DebuggerWindow *weakSelf = self;
    _symList.onJumpAddr = ^(uint16_t addr) {
        DebuggerWindow *s = weakSelf;
        if (!s)
            return;
        s->_memBase = addr;
        s->_memAddr.stringValue = @"";
        s->_seenSerial = 0;
        if (intvdebug_is_paused(s->_dbg))
            [s refreshMem];
    };
    symScroll.documentView = _symList;

    NSStackView *root =
        [NSStackView stackViewWithViews:@[ filterRow, symScroll ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [filterRow.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                          constant:-16].active = YES;
    [symScroll.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                          constant:-16].active = YES;
    return root;
}

- (NSView *)buildFujinetTab
{
    _fujiNote = [self label:@""];
    [_fujiNote setContentHuggingPriority:1
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSButton *clear = [self button:@"Clear" action:@selector(fujiClear:)];
    clear.toolTip = @"Forget the packets so far";
    NSStackView *row = [NSStackView stackViewWithViews:@[ _fujiNote, clear ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.distribution = NSStackViewDistributionFill;

    NSScrollView *logScroll;
    _fujiText = logView(&logScroll);
    [logScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationVertical];
    NSStackView *left = [NSStackView stackViewWithViews:@[
        [self label:@"FujiBus packets (SLIP-decoded)"], row, logScroll
    ]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [row.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;
    [logScroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active =
        YES;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:560].active = YES;

    NSScrollView *stateScroll;
    _fujiState = monoView(&stateScroll);
    [stateScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                            forOrientation:NSLayoutConstraintOrientationVertical];
    [stateScroll.widthAnchor constraintEqualToConstant:320].active = YES;
    NSStackView *right = [NSStackView stackViewWithViews:@[
        [self label:@"FujiNet's mailbox"], stateScroll
    ]];
    right.orientation = NSUserInterfaceLayoutOrientationVertical;
    right.alignment = NSLayoutAttributeLeading;
    right.spacing = 6;

    NSSplitView *split = [[NSSplitView alloc] init];
    split.vertical = YES;
    [split addArrangedSubview:left];
    [split addArrangedSubview:right];
    return split;
}

/* The registers under every tab: R0-R7, then the flags S Z O C I and D. Each
 * is a field that takes a value (hex, $hex, or an expression) while the
 * machine is stopped; Enter applies it (intvdebug_regs_set). */
- (NSView *)buildRegStrip
{
    static const char *const names[REG_FIELDS] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6 (SP)", "R7 (PC)",
                                                   "S", "Z", "O", "C", "I", "D"};
    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int idx = 0; idx < REG_FIELDS; idx++) {
        _reg[idx] = [[NSTextField alloc] init];
        _reg[idx].font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        _reg[idx].tag = idx;
        _reg[idx].target = self;
        _reg[idx].action = @selector(applyRegister:);
        _reg[idx].toolTip = @"Edit while the machine is stopped (hex, $hex or a label), then Enter. "
                          @"A flag takes 0 or 1; D set means SDBD is pending";
        [_reg[idx].widthAnchor constraintEqualToConstant:idx < 8 ? 56 : 30].active = YES;
        NSMutableArray<NSView *> *row = idx < 8 ? row1 : row2;
        [row addObject:[self label:@(names[idx])]];
        [row addObject:_reg[idx]];
    }
    _regInfo = [self label:@""];
    _regInfo.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _regInfo.textColor = NSColor.secondaryLabelColor;
    [row2 addObject:_regInfo];
    NSStackView *topRow = [NSStackView stackViewWithViews:row1];
    NSStackView *bottomRow = [NSStackView stackViewWithViews:row2];
    topRow.orientation = bottomRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSStackView *strip = [NSStackView stackViewWithViews:@[ topRow, bottomRow ]];
    strip.orientation = NSUserInterfaceLayoutOrientationVertical;
    strip.alignment = NSLayoutAttributeLeading;
    strip.spacing = 4;
    return strip;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1240, 860)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Intellivision Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    _runBtn = [self button:@"Pause (F5)" action:@selector(togglePause:)];
    NSButton *stepBtn = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *stepOverBtn = [self button:@"Step Over (F8)"
                                  action:@selector(stepOver:)];
    NSButton *stepOutBtn = [self button:@"Step Out (⇧F8)"
                                 action:@selector(stepOut:)];
    NSButton *clearBpBtn = [self button:@"Clear Breakpoints"
                                 action:@selector(clearBreakpoints:)];
    NSButton *loadSymsBtn = [self button:@"Load Symbols…"
                                  action:@selector(loadSymbols:)];
    NSButton *clearSymsBtn = [self button:@"Clear Symbols"
                                   action:@selector(clearSymbols:)];
    _runBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    stepBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    stepOverBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];

    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;

    NSButton *helpBtn = IntvHelpButton(nil);
    [helpBtn setRefusesFirstResponder:YES];
    NSButton *popOutBtn = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popOutBtn.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";
    NSStackView *toolbar = [NSStackView stackViewWithViews:@[
        _runBtn, stepBtn, stepOverBtn, stepOutBtn, clearBpBtn, loadSymsBtn,
        clearSymsBtn, popOutBtn, _status, helpBtn
    ]];
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 8;

    NSTabView *tabs = [[NSTabView alloc] init];
    NSArray<NSView *> *pages = @[
        [self buildConsoleTab], [self buildDisasmTab], [self buildBasicTab], [self buildMemoryTab],
        [self buildBreaksTab], [self buildWatchesTab], [self buildCartTab], [self buildHistTab:0],
        [self buildHistTab:1], [self buildVideoTab], [self buildSoundTab], [self buildSymbolsTab],
        [self buildFujinetTab]
    ];
    for (int idx = 0; idx < TAB_COUNT; idx++) {   /* each keeps its place when moved around */
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@(kTabIds[idx])];
        item.label = @(kTabNames[idx]);
        item.view = pages[(NSUInteger)idx];
        _items[idx] = item;
        [tabs addTabViewItem:item];
    }
    _basicItem = _items[MT_BASIC];
    _fujiItem = _items[MT_FUJINET];
    _tabs = tabs;
    tabs.delegate = self;
    if (tabFromEnv() >= 0)
        [tabs selectTabViewItemAtIndex:tabFromEnv()];
    [self updateHelpContext];

    /* The registers sit under every tab. */
    NSView *strip = [self buildRegStrip];

    NSStackView *root = [NSStackView stackViewWithViews:@[ toolbar, tabs, strip ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                        constant:-16].active = YES;
    [tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                     constant:-16].active = YES;
    [strip.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                       constant:-16].active = YES;
    [tabs.heightAnchor constraintGreaterThanOrEqualToConstant:640].active =
        YES;

    _window.contentView = root;
    [_window setContentSize:NSMakeSize(1240, 860)];
    [_window center];
}

/* ---- actions ---------------------------------------------------------------- */

- (void)togglePause:(id)sender
{
    (void)sender;
    if (intvdebug_is_paused(_dbg))
        intvdebug_resume(_dbg);
    else
        intvdebug_pause(_dbg);
}

/* On the BASIC tab the three steppers go by BASIC line. */
- (BOOL)basicShowing
{
    return _tabs.selectedTabViewItem == _basicItem || _popped[MT_BASIC].isKeyWindow;
}

- (void)step:(id)sender
{
    (void)sender;
    if ([self basicShowing])
        intvdebug_step_line(_dbg);
    else
        intvdebug_step(_dbg);
}

- (void)stepOver:(id)sender
{
    (void)sender;
    if ([self basicShowing])
        intvdebug_step_line_over(_dbg);
    else
        intvdebug_step_over(_dbg);
}

- (void)stepOut:(id)sender
{
    (void)sender;
    if ([self basicShowing])
        intvdebug_step_line_out(_dbg);
    else
        intvdebug_step_out(_dbg);
}

- (void)clearBreakpoints:(id)sender
{
    (void)sender;
    intvdebug_breakpoint_clear_all(_dbg);
    _seenSerial = 0;
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedFileTypes = @[ @"sym" ];
    panel.allowsMultipleSelection = NO;
    /* Remember the directory (settings key "sym_dir") so re-opening the
     * panel -- expected to happen often, since loading is always a manual
     * step taken after CONFIG has already booted a cart, never automatic --
     * starts back where the user just was. */
    if (_session) {
        const char *lastDir = intvsession_get_str(_session, "sym_dir", "");
        if (lastDir[0])
            panel.directoryURL =
                [NSURL fileURLWithPath:[NSString stringWithUTF8String:lastDir]
                            isDirectory:YES];
    }
    __weak DebuggerWindow *weakSelf = self;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
        DebuggerWindow *s = weakSelf;
        if (!s || result != NSModalResponseOK)
            return;
        NSURL *url = panel.URLs.firstObject;
        if (!url)
            return;
        if (intvsymtab_load(s->_symtab, url.fileSystemRepresentation) >= 0) {
            if (s->_session) {
                char dir[1024];
                if (intvsym_dirname(url.fileSystemRepresentation, dir,
                                    sizeof(dir)) > 0) {
                    intvsession_set_str(s->_session, "sym_dir", dir);
                    intvsession_settings_flush(s->_session);
                }
            }
            s->_seenSerial = 0;
            if (intvdebug_is_paused(s->_dbg))
                [s refreshAll:YES];
        }
    }];
}

- (void)clearSymbols:(id)sender
{
    (void)sender;
    intvsymtab_clear_user(_symtab);
    _seenSerial = 0;
    if (intvdebug_is_paused(_dbg))
        [self refreshAll:YES];
}

/* Shows a range in the Memory tab, highlighted, on a row boundary. */
- (void)showInMemoryLo:(uint16_t)lo hi:(uint16_t)hi
{
    _memBase = (uint16_t)(lo & 0xFFF8);
    _memHl = YES;
    _memHlLo = lo;
    _memHlHi = hi;
    _seenSerial = 0;
    if (intvdebug_is_paused(_dbg))
        [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint16_t lo, hi;
    if (intvsym_parse_range(_symtab, _memAddr.stringValue.UTF8String, &lo, &hi, NULL)) {
        [self showInMemoryLo:lo hi:hi];
    } else {
        _status.stringValue = [NSString
            stringWithFormat:@"No such symbol, variable or address: %@",
                            _memAddr.stringValue];
    }
}

- (void)breakAt:(id)sender
{
    (void)sender;
    uint16_t addr;
    if (intvsym_parse_addr(_symtab, _breakAddr.stringValue.UTF8String,
                           &addr)) {
        intvdebug_breakpoint_toggle(_dbg, addr);
        _seenSerial = 0; /* redraw the marker immediately */
    } else {
        _status.stringValue = [NSString
            stringWithFormat:@"No such symbol or address: %@",
                            _breakAddr.stringValue];
    }
}

- (void)symFilterChanged:(id)sender
{
    (void)sender;
    [self refreshSymbols];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    intvdebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

/* Help's contexts (intvhelp.h): the tab identifiers are the contexts' ends */
- (void)updateHelpContext
{
    NSString *ident = _tabs.selectedTabViewItem.identifier;
    if (ident)
        IntvSetHelpContext(_window, [@"debugger/" stringByAppendingString:ident]);
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    [self updateHelpContext];
    if (item == _fujiItem)
        [self refreshFujinet:NO];
    if (item == _items[MT_VIDEO])
        [_video refreshForce:YES];
    if (item == _items[MT_CONSOLE])
        [_window makeFirstResponder:_conIn];
}

/* windowShouldClose: fires whenever the window's close control (or Window >
 * Close) is used; keep the window alive but hidden, like GNOME/KDE/Windows
 * do, so F12 reopens it instantly with its state intact -- but disengage
 * RIGHT HERE rather than relying on -dealloc, which for this hide-not-
 * destroy shape would only ever run at process exit. Leaving the debugger
 * engaged after every close would otherwise run the emulator on the slow
 * instruction-hooked path forever with the window merely hidden. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int t = 0; t < TAB_COUNT; t++)
        if (sender == _popped[t]) {   /* closing a peeled-off tab's window docks it */
            [self dockTab:t];
            return NO;
        }
    intvdebug_set_engaged(_dbg, 0);
    [_window orderOut:nil];
    for (int t = 0; t < TAB_COUNT; t++)
        [_popped[t] orderOut:nil];
    return NO;
}

/* ---- refreshers --------------------------------------------------------------- */

/* ---- the BASIC tab ------------------------------------------------------- */

/* The map changed (an import, a Load Map): the file list. */
- (void)basicReload
{
    const int n = intvsrcmap_file_count(_srcmap);

    [_basFiles removeAllItems];
    for (int i = 0; i < n; i++)
        [_basFiles addItemWithTitle:@(intvsrcmap_file_name(_srcmap, i))];
    if (_basFile >= n)
        _basFile = -1;
    if (_basFile < 0 && n > 0) {
        BOOL found = NO;
        _basFile = 0; /* the first file with any code on it */
        for (int f = 0; f < n && !found; f++) {
            const int lines = intvsrcmap_line_count(_srcmap, f);
            for (int l = 1; l <= lines && !found; l++)
                if (intvsrcmap_first_addr(_srcmap, f, l) >= 0) {
                    _basFile = f;
                    found = YES;
                }
        }
    }
    if (_basFile >= 0)
        [_basFiles selectItemAtIndex:_basFile];
    _basPcLine = 0;
    if (n == 0)
        _basNote.stringValue =
            @"Import a .bas file, or a folder of them. The assembler's map "
            @"(as1600 -j game.smap) puts each line at its address.";
}

- (void)refreshBasic:(BOOL)paused
{
    const unsigned gen = intvsrcmap_generation(_srcmap);
    int pf = -1, pl = 0;
    intvdebug_regs r;

    if (gen != _basGen) {
        _basGen = gen;
        [self basicReload];
    }
    if (paused && intvdebug_regs_get(_dbg, &r)) {
        int f, l;
        if (intvsrcmap_lookup(_srcmap, r.pc, &f, &l)) {
            pf = f;
            pl = l;
        }
    }
    if (pf >= 0 && pf != _basFile) { /* followed into another file */
        _basFile = pf;
        [_basFiles selectItemAtIndex:pf];
    }

    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
    NSRange pcRange = NSMakeRange(NSNotFound, 0);
    if (_basFile >= 0) {
        const int n = intvsrcmap_line_count(_srcmap, _basFile);
        for (int i = 1; i <= n; i++) {
            const int addr = intvsrcmap_first_addr(_srcmap, _basFile, i);
            const char *src = intvsrcmap_line_text(_srcmap, _basFile, i);
            const BOOL isPc = pf == _basFile && i == pl;
            const NSUInteger before = text.length;
            appendLine(text, [NSString stringWithFormat:@"%5d  %s", i, src ? src : ""], isPc,
                       addr >= 0 ? intvdebug_breakpoint_state(_dbg, (uint16_t)addr) : 0);
            if (isPc)
                pcRange = NSMakeRange(before, text.length - before);
        }
    }
    setAttrText(_basView, text);

    const int line = pf == _basFile ? pl : 0;
    /* every new stop brings the current line into view, even the same line as last time */
    const uint64_t ser = intvdebug_stop_serial(_dbg);
    const BOOL newStop = paused && ser != _basSerial;
    if (paused)
        _basSerial = ser;
    if (line != _basPcLine || (newStop && line)) {
        _basPcLine = line;
        if (pcRange.location != NSNotFound)
            [_basView scrollRangeToVisible:pcRange];
    }
}

- (void)basicImported:(NSString *)path folder:(BOOL)folder
{
    intvsrcmap_import_result r;
    const int n = intvsrcmap_import(_srcmap, path.fileSystemRepresentation, _symtab, &r);

    if (n < 0) {
        _basNote.stringValue = folder ? @"No .bas files in that folder."
                                      : @"Could not read that file.";
        return;
    }
    [self basicNote:n result:&r];
    _basFile = -1; /* basicReload picks the first file with code */
    _seenSerial = 0;
    if (intvdebug_is_paused(_dbg))
        [self refreshAll:YES];
}

- (void)basicNote:(int)files result:(const intvsrcmap_import_result *)r
{
    if (r->mapped > 0)
        _basNote.stringValue = [NSString
            stringWithFormat:@"%d file%s, %d address range%s from %@%s.", files,
                             files == 1 ? "" : "s", r->mapped, r->mapped == 1 ? "" : "s",
                             [@(r->map_path) lastPathComponent],
                             r->symbols ? ", with its symbols" : ""];
    else
        _basNote.stringValue = [NSString
            stringWithFormat:@"%d file%s loaded, but no .smap or .lst was found beside them. "
                             @"Build with as1600 -j game.smap, then Load Map.",
                             files, files == 1 ? "" : "s"];
}

/* kind: 0 file, 1 folder, 2 map */
- (void)basicPick:(int)kind
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = kind != 1;
    panel.canChooseDirectories = kind == 1;
    if (kind == 0)
        panel.allowedFileTypes = @[ @"bas" ];
    else if (kind == 2)
        panel.allowedFileTypes = @[ @"smap", @"lst" ];
    if (_session) {
        const char *lastDir = intvsession_get_str(_session, "bas_dir", "");
        if (lastDir[0])
            panel.directoryURL = [NSURL fileURLWithPath:@(lastDir) isDirectory:YES];
    }
    __weak DebuggerWindow *weakSelf = self;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
        DebuggerWindow *s = weakSelf;
        NSURL *url = panel.URLs.firstObject;
        if (!s || result != NSModalResponseOK || !url)
            return;
        NSString *path = url.path;
        if (s->_session) { /* start the next panel where this one ended */
            NSString *dir = kind == 1 ? path : [path stringByDeletingLastPathComponent];
            intvsession_set_str(s->_session, "bas_dir", dir.fileSystemRepresentation);
            intvsession_settings_flush(s->_session);
        }
        if (kind == 2) {
            const int n = intvsrcmap_load_map(s->_srcmap, path.fileSystemRepresentation);
            if (n < 0) {
                s->_basNote.stringValue = @"Could not read that map.";
                return;
            }
            intvsrcmap_import_result r;
            memset(&r, 0, sizeof(r));
            r.mapped = n;
            r.symbols = intvsrcmap_load_sibling_sym(path.fileSystemRepresentation, s->_symtab);
            snprintf(r.map_path, sizeof(r.map_path), "%s", path.fileSystemRepresentation);
            [s basicNote:intvsrcmap_file_count(s->_srcmap) result:&r];
            s->_seenSerial = 0;
            if (intvdebug_is_paused(s->_dbg))
                [s refreshAll:YES];
        } else {
            [s basicImported:path folder:kind == 1];
        }
    }];
}

- (void)basImportFile:(id)sender { (void)sender; [self basicPick:0]; }
- (void)basImportFolder:(id)sender { (void)sender; [self basicPick:1]; }
- (void)basLoadMap:(id)sender { (void)sender; [self basicPick:2]; }

- (void)basFileChosen:(id)sender
{
    (void)sender;
    _basFile = (int)_basFiles.indexOfSelectedItem;
    _basPcLine = 0;
    [self refreshBasic:intvdebug_is_paused(_dbg)];
}

/* ---- the console, the register strip and the Poke bar ------------------------- */

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);   /* attr reader scrolled back keeps their place */
    NSAttributedString *attr = [[NSAttributedString alloc]
        initWithString:text
            attributes:@{ NSFontAttributeName : [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
                          NSForegroundColorAttributeName : NSColor.textColor }];
    [_conOut.textStorage appendAttributedString:attr];
    if (follow)
        [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_conIn.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (cmd.length == 0)
        return;
    if (![_conHistory.lastObject isEqualToString:cmd])
        [_conHistory addObject:cmd];
    _conPos = (NSInteger)_conHistory.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", cmd]];
    out[0] = '\0';
    intvdebug_command(_dbg, cmd.UTF8String, out, (int)sizeof(out));
    NSString *text = @(out) ?: @"";
    if (text.length)
        [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    _seenSerial = 0;   /* attr command may have moved the machine or changed attr table */
}

/* In the console, Tab completes instead of moving the focus, and Up/Down walk
 * the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn)
        return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger count = (NSInteger)_conHistory.count;
        if (count == 0)
            return YES;
        _conPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_conPos < 0)
            _conPos = 0;
        if (_conPos > count)
            _conPos = count;
        _conIn.stringValue = _conPos < count ? _conHistory[(NSUInteger)_conPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:))
        return NO;
    NSString *text = _conIn.stringValue;
    const NSRange space = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = space.location == NSNotFound ? text : [text substringFromIndex:space.location + 1];
    char comps[4096];
    comps[0] = '\0';
    const int count = intvdebug_completions(_dbg, word.UTF8String, comps, (int)sizeof(comps));
    NSString *list = @(comps) ?: @"";
    if (count == 1) {
        NSString *completion = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = space.location == NSNotFound ? @"" : [text substringToIndex:space.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, completion];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (count > 1) {
        [self appendConsole:[list hasSuffix:@"\n"] ? list : [list stringByAppendingString:@"\n"]];
    }
    return YES;
}

/* A bare run of hex digits is hex (the fields show hex); anything else is an
 * expression for the core to evaluate ($1234, 0x1234, a label, r3 + 1). */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *trim = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    const char *chars = trim.UTF8String;
    char err[160];
    if (!*chars) {
        *why = @"nothing to apply";
        return NO;
    }
    char *end;
    const unsigned long hex = strtoul(chars, &end, 16);
    if (*end == '\0') {
        *out = (long)hex;
        return YES;
    }
    err[0] = '\0';
    if (intvdebug_eval(_dbg, chars, out, err, (int)sizeof(err)) == 0)
        return YES;
    *why = @(err) ?: @"does not parse";
    return NO;
}

- (void)applyRegister:(NSTextField *)sender
{
    long value = 0;
    NSString *why = nil;
    [_window makeFirstResponder:nil];   /* leave the field, so the refresh may rewrite it */
    if (!intvdebug_is_paused(_dbg)) {
        _status.stringValue = @"Pause the machine (F5) to edit registers";
    } else if (sender.tag < 0 || sender.tag >= REG_FIELDS) {
        /* not one of ours */
    } else if ([self evaluate:sender.stringValue into:&value why:&why]) {
        intvdebug_regs_set(_dbg, (int)sender.tag, (unsigned)value);
    } else {
        _status.stringValue = [NSString stringWithFormat:@"Register: %@", why];
    }
    [self refreshRegs];
    [self refreshDisasm];
    _seenSerial = 0;
}

/* "address value ..." (hex) through the core's poke: one word per value, at the
 * width the bus gives that address. */
- (void)pokeMem:(id)sender
{
    (void)sender;
    char out[512];
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    for (NSString *tok in [_memPoke.stringValue componentsSeparatedByCharactersInSet:
                                                    [NSCharacterSet characterSetWithCharactersInString:@" \t,"]]) {
        if (tok.length == 0)
            continue;
        const char *chars = tok.UTF8String;
        char *end;
        (void)strtoul(chars, &end, 16);
        [tokens addObject:*end == '\0' ? [@"$" stringByAppendingString:tok] : tok];   /* bare hex is hex */
    }
    if (tokens.count < 2) {
        _memNote.stringValue = @"Type an address and one or more values.";
        return;
    }
    out[0] = '\0';
    intvdebug_command(_dbg, [@"poke " stringByAppendingString:[tokens componentsJoinedByString:@" "]].UTF8String, out,
                      (int)sizeof(out));
    _memNote.stringValue = [(@(out) ?: @"") stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (intvdebug_is_paused(_dbg))
        [self refreshMem];
}

/* ---- Breakpoints, Watches, memory search ------------------------------------- */

/* Why the machine stopped, worded by the core so the width is right. */
- (NSString *)stopTextOf:(const intvdebug_snapshot *)snap
{
    return snap->paused ? (@(snap->text) ?: @"") : @"";
}

/* Text for a pick list, the picked line drawn with a background. */
static void setPickText(PickTextView *view, NSArray<NSString *> *lines)
{
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
    NSFont *font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    int i = 0;
    for (NSString *l in lines) {
        NSMutableAttributedString *attr = [[NSMutableAttributedString alloc]
            initWithString:[l stringByAppendingString:@"\n"]
                attributes:@{ NSFontAttributeName : font, NSForegroundColorAttributeName : NSColor.textColor }];
        if (i == view.picked) {
            [attr addAttribute:NSBackgroundColorAttributeName value:accentColor() range:NSMakeRange(0, attr.length)];
            [attr addAttribute:NSForegroundColorAttributeName value:accentInk() range:NSMakeRange(0, attr.length)];
        }
        [text appendAttributedString:attr];
        i++;
    }
    setAttrText(view, text);
}

static PickTextView *makePickView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    PickTextView *view = [[PickTextView alloc] initWithFrame:NSMakeRect(0, 0, 900, 400)];
    view.editable = NO;
    view.richText = YES;
    view.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    view.autoresizingMask = NSViewWidthSizable;
    view.picked = -1;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

- (NSView *)buildBreaksTab
{
    _bpType = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_bpType addItemsWithTitles:@[ @"Execute", @"Read", @"Write", @"Access" ]];
    _bpType.toolTip = @"Execute stops before an instruction; Read, Write and Access stop after the "
                      @"instruction that touched the words (data accesses only, not code fetches)";
    _bpAddr = [[NSTextField alloc] init];
    _bpAddr.placeholderString = @"address, lo-hi, symbol, x, #score, a(3)";
    _bpAddr.target = self;
    _bpAddr.action = @selector(addBreak:);
    _bpCondition = [[NSTextField alloc] init];
    _bpCondition.placeholderString = @"condition (optional): value != old, r0 == 5, [$0200] > 3";
    _bpCondition.toolTip = @"An expression: r0-r7 pc sp s z o c i d, [addr], labels, frame cycles and, on an "
                           @"access, value, addr and old (the word before a write). On a picked entry, "
                           @"Set Condition replaces its condition";
    _bpCondition.target = self;
    _bpCondition.action = @selector(addBreak:);
    NSStackView *form = [NSStackView stackViewWithViews:@[
        _bpType, _bpAddr, _bpCondition, [self button:@"Add" action:@selector(addBreak:)],
        [self button:@"Set Condition" action:@selector(bpSetCondition:)]
    ]];
    form.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    form.distribution = NSStackViewDistributionFill;
    [_bpCondition setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_bpAddr.widthAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;

    NSScrollView *scroll;
    _bpList = makePickView(&scroll);
    _bpIds = [NSMutableArray array];
    __weak DebuggerWindow *weakSelf = self;
    _bpList.onPick = ^(int line) {
        (void)line;
        DebuggerWindow *s = weakSelf;
        if (s) {
            s->_bpGen = 0;   /* redraw with the pick */
            [s refreshBreaks];
        }
    };

    _bpStop = [self label:@""];
    _bpStop.textColor = NSColor.secondaryLabelColor;
    NSStackView *foot = [NSStackView stackViewWithViews:@[
        [self button:@"Enable / Disable" action:@selector(bpToggle:)],
        [self button:@"Remove" action:@selector(bpRemove:)],
        [self button:@"Clear Hits" action:@selector(bpHits:)],
        [self button:@"Remove All" action:@selector(bpClear:)], _bpStop
    ]];
    foot.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSStackView *root = [NSStackView stackViewWithViews:@[ form, scroll, foot ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [form.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [scroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [foot.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    return root;
}

- (NSView *)buildWatchesTab
{
    _wtEntry = [[NSTextField alloc] init];
    _wtEntry.placeholderString = @"address, symbol, x, #score, a(3), lo-hi";
    _wtEntry.target = self;
    _wtEntry.action = @selector(addWatch:);
    NSStackView *form = [NSStackView stackViewWithViews:@[
        [self label:@"Watch"], _wtEntry, [self button:@"Add" action:@selector(addWatch:)]
    ]];
    form.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSScrollView *scroll;
    _wtList = makePickView(&scroll);
    __weak DebuggerWindow *weakSelf = self;
    _wtList.onPick = ^(int line) {
        (void)line;
        DebuggerWindow *s = weakSelf;
        if (s)
            [s refreshWatches];
    };

    NSStackView *foot = [NSStackView stackViewWithViews:@[
        [self button:@"Show in Memory" action:@selector(wtMemory:)],
        [self button:@"Break on Change" action:@selector(wtBreak:)],
        [self button:@"Remove" action:@selector(wtRemove:)]
    ]];
    foot.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSStackView *root = [NSStackView stackViewWithViews:@[ form, scroll, foot ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [form.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [scroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    return root;
}

- (void)refreshBreaks
{
    intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
    const unsigned gen = intvdebug_bp_generation(_dbg);
    if (gen != _bpGen) {
        const int n = intvdebug_bp_list(_dbg, bps, INTVDEBUG_MAX_BREAKPOINTS);
        NSMutableArray<NSString *> *lines = [NSMutableArray array];
        _bpGen = gen;
        [_bpIds removeAllObjects];
        for (int i = 0; i < n; i++) {
            char desc[160];
            intvbp_describe(&bps[i], _symtab, desc, sizeof(desc));
            [lines addObject:[NSString stringWithFormat:@"[%c] %s    hits %u", bps[i].enabled ? 'x' : ' ',
                                                        desc, bps[i].hits]];
            [_bpIds addObject:@(bps[i].id)];
        }
        if (_bpList.picked >= n)
            _bpList.picked = -1;
        setPickText(_bpList, lines);
    }
    {
        intvdebug_snapshot snap;
        intvdebug_stop_snapshot(_dbg, &snap);
        NSString *why = [self stopTextOf:&snap];
        _bpStop.stringValue = !snap.paused ? @"Running." : why.length ? [@"Stopped: " stringByAppendingString:why] : @"Paused.";
    }
}

- (void)refreshWatches
{
    const int n = intvwatch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        char line[200];
        intvwatch_format(_dbg, _symtab, i, line, sizeof(line));
        [lines addObject:@(line)];
    }
    if (_wtList.picked >= n)
        _wtList.picked = -1;
    setPickText(_wtList, lines);
}

- (void)addBreak:(id)sender
{
    (void)sender;
    const int type = (int)_bpType.indexOfSelectedItem;
    const char *text = _bpAddr.stringValue.UTF8String;
    uint16_t lo = 0, hi = 0;
    intvdebug_bp entry;
    memset(&entry, 0, sizeof(entry));

    if (type == 0 ? !intvsym_parse_addr(_symtab, text, &lo)
                  : !intvsym_parse_range(_symtab, text, &lo, &hi, NULL)) {
        _status.stringValue = [NSString stringWithFormat:@"No such symbol, variable or address: %@", _bpAddr.stringValue];
        return;
    }
    if (type == 0)
        hi = lo;
    entry.type = type == 0 ? INTVDEBUG_BP_EXEC : type == 1 ? INTVDEBUG_BP_READ
              : type == 2 ? INTVDEBUG_BP_WRITE : INTVDEBUG_BP_ACCESS;
    entry.lo = lo;
    entry.hi = hi;
    snprintf(entry.condition, sizeof(entry.condition), "%s", _bpCondition.stringValue.UTF8String);
    if (intvdebug_bp_add(_dbg, &entry) < 0) {
        _status.stringValue = entry.condition[0] ? @"The table is full, or the condition does not parse"
                                              : @"The breakpoint table is full";
        return;
    }
    _bpAddr.stringValue = @"";
    _bpCondition.stringValue = @"";
    _seenSerial = 0;
}

/* Replaces the picked entry's condition with the condition field's text. */
- (void)bpSetCondition:(id)sender
{
    (void)sender;
    const int sel = _bpList.picked;
    if (sel < 0 || sel >= (int)_bpIds.count) {
        _status.stringValue = @"Pick a breakpoint in the list first";
        return;
    }
    if (!intvdebug_bp_set_condition(_dbg, _bpIds[(NSUInteger)sel].intValue, _bpCondition.stringValue.UTF8String)) {
        _status.stringValue = [NSString stringWithFormat:@"The condition does not parse: %@", _bpCondition.stringValue];
        return;
    }
    _bpCondition.stringValue = @"";
    _seenSerial = 0;
}

- (void)bpToggle:(id)sender
{
    (void)sender;
    const int sel = _bpList.picked;
    intvdebug_bp bps[INTVDEBUG_MAX_BREAKPOINTS];
    const int n = intvdebug_bp_list(_dbg, bps, INTVDEBUG_MAX_BREAKPOINTS);
    if (sel >= 0 && sel < n) {
        intvdebug_bp_enable(_dbg, bps[sel].id, !bps[sel].enabled);
        _seenSerial = 0;
    }
}

- (void)bpRemove:(id)sender
{
    (void)sender;
    const int sel = _bpList.picked;
    if (sel >= 0 && sel < (int)_bpIds.count) {
        intvdebug_bp_remove(_dbg, _bpIds[(NSUInteger)sel].intValue);
        _bpList.picked = -1;
        _seenSerial = 0;
    }
}

- (void)bpHits:(id)sender
{
    (void)sender;
    intvdebug_bp_clear_hits(_dbg);
    _seenSerial = 0;
}

- (void)bpClear:(id)sender
{
    (void)sender;
    intvdebug_breakpoint_clear_all(_dbg);
    _bpList.picked = -1;
    _seenSerial = 0;
}

- (void)addWatch:(id)sender
{
    (void)sender;
    if (intvwatch_add(_wtEntry.stringValue.UTF8String) < 0) {
        _status.stringValue = @"Type an address, symbol or BASIC variable (x, #score, a(3))";
        return;
    }
    _wtEntry.stringValue = @"";
    _seenSerial = 0;
    [self refreshWatches];
}

- (void)wtMemory:(id)sender
{
    (void)sender;
    uint16_t lo, hi;
    if (_wtList.picked >= 0 && intvwatch_range(_symtab, _wtList.picked, &lo, &hi)) {
        [_tabs selectTabViewItemWithIdentifier:@"memory"];
        [self showInMemoryLo:lo hi:hi];
    }
}

- (void)wtBreak:(id)sender
{
    (void)sender;
    uint16_t first, last;
    if (_wtList.picked < 0 || !intvwatch_range(_symtab, _wtList.picked, &first, &last))
        return;
    [self breakOnChangeFrom:first to:last];
}

/* A write breakpoint on first..last that fires when the value written differs from
 * the old one (the core's `value != old`, at the right word width). */
- (void)breakOnChangeFrom:(uint16_t)first to:(uint16_t)last
{
    intvdebug_bp entry;
    memset(&entry, 0, sizeof(entry));
    entry.type = INTVDEBUG_BP_WRITE;
    entry.lo = first;
    entry.hi = last;
    snprintf(entry.condition, sizeof(entry.condition), "value != old");
    if (intvdebug_bp_add(_dbg, &entry) >= 0)
        _status.stringValue = @"Breaking when it changes";
    _seenSerial = 0;
}

- (void)wtRemove:(id)sender
{
    (void)sender;
    if (_wtList.picked >= 0) {
        intvwatch_remove(_wtList.picked);
        _wtList.picked = -1;
        _seenSerial = 0;
        [self refreshWatches];
    }
}

- (void)searchBackward:(BOOL)backward
{
    uint16_t pat[64], mask, found;
    int flags;
    const int n = intvsearch_parse((int)_srchKind.indexOfSelectedItem, _srchEntry.stringValue.UTF8String,
                                   pat, &mask, &flags);
    if (!n) {
        _srchNote.stringValue = @"Type hex words (02A0 0268) or some text.";
        return;
    }
    if (!intvdebug_is_paused(_dbg)) {
        _srchNote.stringValue = @"Pause the machine to search its memory.";
        return;
    }
    const uint16_t from = _memHl ? (backward ? _memHlLo : (uint16_t)(_memHlLo + 1)) : _memBase;
    if (intvsearch_next(_dbg, pat, n, mask, flags, from, backward ? 1 : 0, &found)) {
        _srchNote.stringValue = [NSString stringWithFormat:@"Found at $%04X", found];
        [self showInMemoryLo:found hi:(uint16_t)(found + n - 1)];
    } else {
        _srchNote.stringValue = @"Not found.";
    }
}

- (void)searchNext:(id)sender { (void)sender; [self searchBackward:NO]; }
- (void)searchPrev:(id)sender { (void)sender; [self searchBackward:YES]; }

/* ---- Cart, Trace and History tabs, and peeling tabs off ------------------------- */

/* On screen: the tab view's selected page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || _tabs.selectedTabViewItem == _items[tab];
}

- (NSView *)buildCartTab
{
    NSScrollView *scroll;
    _cartText = monoView(&scroll);
    NSStackView *root = [NSStackView stackViewWithViews:@[ scroll ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [scroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    return root;
}

/* Controllers, PSG I/O ports, the ECS keyboard, Intellivoice and JLP. */
- (NSString *)ioText:(const intvdebug_io *)ports
{
    NSMutableString *text = [NSMutableString string];
    [text appendString:@"\nControllers and PSG I/O\n"];
    for (int pair = 0; pair < 2; pair++) {
        const intvdebug_pad *pad = &ports->pad[pair];
        if (!pad->present) {
            [text appendFormat:@"  Pair %d (%s)  not present\n", pair, pair ? "ECS $00FE-$00FF" : "$01FE-$01FF"];
            continue;
        }
        [text appendFormat:@"  Pair %d (%s)  left port %02X%s  right port %02X%s\n", pair, pair ? "ECS $00FE-$00FF" : "$01FE-$01FF",
                        pad->side[0], pad->output[0] ? " (out)" : "", pad->side[1], pad->output[1] ? " (out)" : ""];
        NSMutableString *held = [NSMutableString string];
        for (int idx = 0; idx < 18; idx++) {
            if (pad->left[idx])
                [held appendFormat:@" L%d", idx];
            if (pad->right[idx])
                [held appendFormat:@" R%d", idx];
        }
        [text appendFormat:@"    inputs held:%@\n", held.length ? held : @" none"];
        if (pair == 1) {
            [text appendString:@"    ECS keyboard rows "];
            for (int idx = 0; idx < 8; idx++)
                [text appendFormat:@"%02X ", pad->keys[idx] & 0xFF];
            [text appendString:@"\n"];
        }
    }
    [text appendFormat:@"\nECS %s", ports->ecs ? "present" : "absent"];
    if (ports->ecs)
        [text appendFormat:@"  ROM windows $2xxx %d, $7xxx %d, $Exxx %d", ports->ecs_page[0], ports->ecs_page[1], ports->ecs_page[2]];
    [text appendFormat:@"\nIntellivoice %s", ports->ivoice ? "present" : "absent"];
    if (ports->ivoice)
        [text appendFormat:@"  LRQ %d  ALD %d  PC %04X  %s  FIFO %u/%u", ports->ivoice_lrq, ports->ivoice_ald, ports->ivoice_pc,
                        ports->ivoice_halted ? "halted" : "running", ports->ivoice_fifo_head, ports->ivoice_fifo_tail];
    [text appendFormat:@"\nJLP %s", ports->jlp ? "present" : "absent"];
    if (ports->jlp)
        [text appendFormat:@"  accelerators %s  flash %s%@  mailbox hole %s", ports->jlp_accel ? "on" : "off",
                        ports->jlp_flash ? "open" : "none",
                        ports->jlp_flash ? [NSString stringWithFormat:@" (%u bytes)", ports->jlp_flash_bytes] : @"",
                        ports->jlp_mb_hole ? "yes" : "no"];
    [text appendFormat:@"\nLocutus mapper %s   FujiNet mailbox %s\n", ports->locutus ? "yes" : "no",
                    ports->fujinet_mailbox ? "mapped" : "no"];
    return text;
}

- (void)refreshCart
{
    intvdebug_cart cart;
    intvdebug_region reg[64];
    NSMutableString *text = [NSMutableString string];

    intvdebug_cart_info(_dbg, &cart);
    [text appendString:@"Cartridge\n"];
    [text appendFormat:@"  File     %s\n", cart.path[0] ? cart.path : "(none)"];
    if (cart.name[0])
        [text appendFormat:@"  Title    %s%@\n", cart.name, cart.year ? [NSString stringWithFormat:@", %d", cart.year] : @""];
    [text appendFormat:@"  FujiNet  %s\n", cart.fujinet_mailbox ? "mailbox mapped at $9C00" : "no mailbox"];
    [text appendFormat:@"  ECS      %s\n", cart.ecs ? "present" : "no"];
    [text appendFormat:@"  JLP      %s\n", cart.jlp ? "present" : "no"];
    {
        intvdebug_io ports;
        if (intvdebug_io_get(_dbg, &ports))
            [text appendString:[self ioText:&ports]];
    }
    [text appendString:@"\nMemory map\n"];
    const int n = intvdebug_memory_map(_dbg, reg, 64);
    if (!n)
        [text appendString:@"  Pause the machine to read its memory map.\n"];
    for (int i = 0; i < n; i++)
        [text appendFormat:@"  %04X-%04X  %c%c  %s\n", reg[i].lo, reg[i].hi, reg[i].readable ? 'r' : '-',
                        reg[i].writable ? 'w' : '-', reg[i].name];
    setText(_cartText, text, NO);
}

- (NSView *)buildHistTab:(int)idx
{
    _hRec[idx] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[idx] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[idx].toolTip = @"Record every instruction the CPU runs (the machine runs a little slower)";
    _hFilter[idx] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_hFilter[idx] addItemsWithTitles:@[ @"All instructions", @"Branches only", @"In range" ]];
    _hFilter[idx].target = self;
    _hFilter[idx].action = @selector(histChanged:);
    _hRange[idx] = [[NSTextField alloc] init];
    _hRange[idx].placeholderString = @"range: lo-hi or a symbol";
    _hRange[idx].target = self;
    _hRange[idx].action = @selector(histChanged:);
    _hNote[idx] = [self label:@""];
    _hNote[idx].textColor = NSColor.secondaryLabelColor;
    NSStackView *bar = [NSStackView stackViewWithViews:@[
        _hRec[idx], _hFilter[idx], _hRange[idx], [self button:@"Clear" action:@selector(histClear:)], _hNote[idx]
    ]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSScrollView *scroll;
    _hText[idx] = monoView(&scroll);
    NSStackView *root = [NSStackView stackViewWithViews:@[ bar, scroll ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [bar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [scroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    return root;
}

- (void)histRec:(id)sender
{
    intvdebug_history_enable(_dbg, ((NSButton *)sender).state == NSControlStateValueOn);
    _seenSerial = 0;
}

- (void)histChanged:(id)sender
{
    (void)sender;
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    _seenSerial = 0;
}

- (void)histClear:(id)sender
{
    (void)sender;
    intvdebug_history_clear(_dbg);
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    _seenSerial = 0;
}

- (void)refreshHist:(int)idx force:(BOOL)force
{
    static intvdebug_hist buf[4000];
    const int kShown = 400;
    const BOOL live = idx == 0;
    const BOOL paused = intvdebug_is_paused(_dbg);
    const BOOL on = intvdebug_history_enabled(_dbg);
    const uint64_t count = intvdebug_history_count(_dbg);
    uint16_t lo = 0, hi = 0xFFFF;
    int filter = 0;

    for (int pair = 0; pair < 2; pair++) {   /* both Record buttons follow the engine */
        _hRec[pair].state = on ? NSControlStateValueOn : NSControlStateValueOff;
        _hRec[pair].bezelColor = on ? accentColor() : nil;
        _hRec[pair].contentTintColor = on ? accentInk() : nil;
    }
    if (!force && count == _hSeen[idx] && (live || paused))
        return;
    _hSeen[idx] = count;
    if (!live && !paused) {
        _hNote[idx].stringValue = @"Pause the machine to read its history.";
        return;
    }
    switch (_hFilter[idx].indexOfSelectedItem) {
    case 1: filter = INTVDEBUG_HIST_BRANCHES; break;
    case 2:
        if (intvsym_parse_range(_symtab, _hRange[idx].stringValue.UTF8String, &lo, &hi, NULL))
            filter = INTVDEBUG_HIST_RANGE;
        break;
    }
    const int n = intvdebug_history_copy(_dbg, buf, 4000);
    int hit[400], shown = 0;
    for (int i = n - 1; i >= 0 && shown < kShown; i--)   /* the newest matches, scanning back */
        if (intvdebug_history_match(&buf[i], filter, lo, hi))
            hit[shown++] = i;
    NSMutableString *t = [NSMutableString string];
    if (!on && n == 0)
        [t appendString:@"History is off. Press Record, then let the machine run."];
    for (int pair = shown - 1; pair >= 0; pair--) {   /* oldest first */
        char line[256];
        intvdebug_history_format(&buf[hit[pair]], line, sizeof(line));
        [t appendFormat:@"%s\n", line];
    }
    setText(_hText[idx], t, YES);
    _hNote[idx].stringValue = [NSString stringWithFormat:@"%@%llu instructions recorded, last %d shown",
                                                          on ? @"Recording. " : @"", (unsigned long long)count, shown];
}

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    for (int t = 0; t < TAB_COUNT; t++)
        if (_tabs.selectedTabViewItem == _items[t]) {
            [self popOutTab:t];
            return;
        }
}

- (void)popOutTab:(int)tab
{
    if (_popped[tab] || _tabs.numberOfTabViewItems <= 1)   /* one tab always stays */
        return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 580)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [NSString stringWithFormat:@"Intellivision Debugger – %s", kTabNames[tab]];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSStackView *root = [NSStackView stackViewWithViews:@[ dockBtn, view ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [view.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    win.contentView = root;
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    _seenSerial = 0;
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

- (void)dockTab:(int)tab
{
    NSWindow *win = _popped[tab];
    if (!win)
        return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int pos = 0;
    _popped[tab] = nil;
    [view removeFromSuperview];
    item.view = view;
    for (int t = 0; t < tab; t++)   /* back where it was in the order */
        if (!_popped[t])
            pos++;
    [_tabs insertTabViewItem:item atIndex:pos];
    [_tabs selectTabViewItem:item];
    win.delegate = nil;
    [win close];
    _seenSerial = 0;
}

- (void)refreshAll:(BOOL)paused
{
    _runBtn.title = paused ? @"Continue (F5)" : @"Pause (F5)";
    /* the action on offer wears the accent: Continue while stopped */
    _runBtn.bezelColor = paused ? accentColor() : nil;
    _runBtn.contentTintColor = paused ? accentInk() : nil;
    {
        intvdebug_snapshot snap;
        intvdebug_stop_snapshot(_dbg, &snap);
        NSString *why = [self stopTextOf:&snap];
        if (why.length)
            why = [[[why substringToIndex:1] uppercaseString] stringByAppendingString:[why substringFromIndex:1]];
        _status.stringValue = snap.paused && why.length ? why : paused ? @"Paused" : @"Running";
    }
    [self refreshRegs];
    if (paused) {
        [self refreshDisasm];
        [self refreshMem];
    } else {
        _disasm.string = @"";
        _memView.string = @"";
    }
    [self refreshSound];
    /* Independent of run/pause -- the symbol list reflects what's loaded,
     * not machine state, and is cheap to skip when nothing changed (see
     * refreshSymbols's own comment). */
    [self refreshSymbols];
    [self refreshBasic:paused];
    [self refreshBreaks];
    [self refreshWatches];
    [self refreshCart];
    [self refreshHist:0 force:NO];
    [self refreshHist:1 force:NO];
    if ([self tabVisible:MT_VIDEO])
        [_video refreshForce:YES];
    if ([self fujiShowing])
        [self refreshFujinet:NO];
}

- (BOOL)fujiShowing
{
    return [self tabVisible:MT_FUJINET];
}

- (void)refreshFujinet:(BOOL)force
{
    static intvdebug_fuji_packet p[INTVDEBUG_FUJI_TRACE_MAX];
    static char state[2048];
    int n = intvdebug_fujinet_trace(_dbg, p, INTVDEBUG_FUJI_TRACE_MAX);

    _fujiTicks = 0;
    if (!_session || (!intvsession_fujinet_running(_session) && !intvsession_fujinet_external(_session)))
        _fujiNote.stringValue = @"FujiNet is not running.";
    else
        _fujiNote.stringValue = [NSString stringWithFormat:@"%@ FujiNet at %s, cartridge %@.",
                                 intvsession_fujinet_external(_session) ? @"External" : @"Built-in",
                                 intvsession_boip_endpoint(_session),
                                 intvsession_fujinet_link(_session) ? @"connected" : @"not connected"];
    intvdebug_fujinet_state(_dbg, state, (int)sizeof(state));
    setText(_fujiState, [NSString stringWithUTF8String:state] ?: @"", NO);

    if (!force && n == _fujiCount &&
        (n == 0 || p[n - 1].serial == _fujiLast))
        return;
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    const BOOL follow = force || atEnd(_fujiText);
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        [text appendFormat:@"%@%@\n     ", i ? @"\n" : @"",
                           [NSString stringWithUTF8String:p[i].text] ?: @""];
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof(p[i].data); k++)
            [text appendFormat:@"%02X ", p[i].data[k]];
        if (p[i].nbytes > (int)sizeof(p[i].data))
            [text appendString:@"…"];
    }
    if (n == 0)
        [text appendString:@"(no FujiBus packets since the trace was cleared)"];
    setText(_fujiText, text, follow);
}

/* Only rebuilds the list when the table's generation or the filter text has
 * changed since the last call -- a real game .sym can be a few hundred
 * entries, no reason to re-render it on every 0.1s tick. */
- (void)refreshSymbols
{
    unsigned gen = intvsymtab_generation(_symtab);
    NSString *filter = _symFilter.stringValue ?: @"";
    if (gen == _symSeenGen && [filter isEqualToString:_symFilterText])
        return;
    _symSeenGen = gen;
    _symFilterText = filter;

    NSMutableString *text = [NSMutableString string];
    int n = intvsymtab_count(_symtab);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        uint16_t addr;
        char name[64], note[64];
        if (!intvsymtab_enum(_symtab, i, &addr, name, sizeof(name), note,
                             sizeof(note)))
            continue;
        NSString *nsname = [NSString stringWithUTF8String:name];
        if (filter.length > 0 &&
            [nsname rangeOfString:filter
                          options:NSCaseInsensitiveSearch].location ==
                NSNotFound)
            continue;
        [text appendFormat:@"%04X  %-24s%s%s\n", addr, name,
                           note[0] ? "  ; " : "", note];
        shown++;
    }
    _symList.string = text;
    _symCount.stringValue = [NSString
        stringWithFormat:@"%d symbol%s%s", shown, shown == 1 ? "" : "s",
                        filter.length > 0 ? " (filtered)" : ""];
}

- (void)refreshDisasm
{
    intvdebug_dasm_line lines[DISASM_LINES];
    intvdebug_regs r;
    if (!intvdebug_regs_get(_dbg, &r))
        return;

    int n = intvdebug_disassemble(_dbg, r.pc, r.D, lines, DISASM_LINES);
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
    for (int i = 0; i < n; i++) {
        char symsuffix[96];
        intvsym_annotate_line(_symtab, &lines[i], symsuffix, sizeof(symsuffix));
        NSString *body = [NSString stringWithFormat:@"%04X  %-24s%s", lines[i].addr,
                                                    lines[i].text, symsuffix];
        appendLine(text, body, lines[i].addr == r.pc,
                   intvdebug_breakpoint_state(_dbg, lines[i].addr));
    }
    setAttrText(_disasm, text);
    [_disasm scrollRangeToVisible:NSMakeRange(0, 0)];   /* it always starts at the PC */
}

- (void)refreshRegs
{
    static uint16_t previous[REG_FIELDS];
    static BOOL havePrevious;
    intvdebug_regs regs;
    if (!intvdebug_regs_get(_dbg, &regs)) {
        for (int idx = 0; idx < REG_FIELDS; idx++) {
            _reg[idx].stringValue = @"";
            _reg[idx].enabled = NO;
        }
        _regInfo.stringValue = @"";
        havePrevious = NO;
        return;
    }
    const int flags[6] = {regs.S, regs.Z, regs.O, regs.C, regs.I, regs.D};
    for (int idx = 0; idx < REG_FIELDS; idx++) {
        const unsigned value = idx < 8 ? regs.r[idx] : (flags[idx - 8] ? 1u : 0u);
        const BOOL changed = havePrevious && previous[idx] != value;
        previous[idx] = (uint16_t)value;
        _reg[idx].enabled = YES;
        _reg[idx].textColor = changed ? accentLight() : NSColor.textColor;   /* moved since the last stop */
        if (_reg[idx].currentEditor)
            continue;   /* being typed in */
        _reg[idx].stringValue = idx < 8 ? [NSString stringWithFormat:@"%04X", value] : [NSString stringWithFormat:@"%u", value];
    }
    havePrevious = YES;
    _regInfo.stringValue = [NSString
        stringWithFormat:@"  INTR %d BUSRQ %d  %@%@  cycle %llu  frame %u", regs.intrq, regs.busrq,
                         regs.interruptible ? @"interruptible" : @"not interruptible",
                         regs.D ? @", SDBD pending" : @"", (unsigned long long)regs.cycles, regs.frame];
}

- (void)refreshMem
{
    uint16_t words[MEM_WORDS_PER_ROW * MEM_ROWS];
    int n = intvdebug_read(_dbg, _memBase, words,
                           MEM_WORDS_PER_ROW * MEM_ROWS);
    NSMutableString *text = [NSMutableString string];
    NSMutableArray<NSValue *> *marks = [NSMutableArray array];
    const char *lastRegion = NULL;
    for (int row = 0; row * MEM_WORDS_PER_ROW < n; row++) {
        uint16_t rowAddr = (uint16_t)(_memBase + row * MEM_WORDS_PER_ROW);
        const char *region = intvsym_region(rowAddr);

        [text appendFormat:@"%04X ", (unsigned)rowAddr];
        for (int col = 0; col < MEM_WORDS_PER_ROW; col++) {
            int idx = row * MEM_WORDS_PER_ROW + col;
            const uint16_t addr = (uint16_t)(rowAddr + col);
            if (idx >= n)
                continue;
            [text appendString:@" "];
            /* the scratchpad, GROM/GRAM and ECS RAM are 8 bits wide: two digits, not four */
            const BOOL narrow = intvdebug_word_width(_dbg, addr) == 8;
            if (_memHl && addr >= _memHlLo && addr <= _memHlHi)   /* addr variable, symbol or hit */
                [marks addObject:[NSValue valueWithRange:NSMakeRange(text.length, narrow ? 2 : 4)]];
            if (narrow)
                [text appendFormat:@"%02X  ", words[idx] & 0xFF];
            else
                [text appendFormat:@"%04X", words[idx]];
        }
        /* Only label the first row of addr run in the same region -- see
         * symbols.c's k_regions and the GNOME/Windows/KDE frontends' own
         * refresh_mem/refreshMem. */
        if (region && region != lastRegion)
            [text appendFormat:@"  ; %s", region];
        lastRegion = region;
        [text appendString:@"\n"];
    }
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc]
        initWithString:text
            attributes:@{ NSFontAttributeName : [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
                          NSForegroundColorAttributeName : NSColor.textColor }];
    for (NSValue *span in marks) {
        [styled addAttribute:NSBackgroundColorAttributeName value:accentColor() range:span.rangeValue];
        [styled addAttribute:NSForegroundColorAttributeName value:accentInk() range:span.rangeValue];
    }
    setAttrText(_memView, styled);
}

/* The two AY-3-8914 PSGs: the chip's registers, the voices they decode to, the
 * noise and the envelope. Only a stopped machine can be read. */
- (void)refreshSound
{
    NSMutableString *text = [NSMutableString string];
    if (!intvdebug_is_paused(_dbg)) {
        setText(_soundText, @"Pause the machine to read the sound chips.", NO);
        return;
    }
    for (int unit = 0; unit < 2; unit++) {
        intvdebug_psg psg;
        if (!intvdebug_sound_get(_dbg, unit, &psg) || !psg.present) {
            if (unit == 1)
                [text appendString:@"ECS PSG: not present\n"];
            continue;
        }
        [text appendFormat:@"%@ (%s)\n", unit ? @"ECS PSG" : @"Base PSG", unit ? "$00F0" : "$01F0"];
        [text appendString:@"  Registers  "];
        for (int idx = 0; idx < 14; idx++)
            [text appendFormat:@"%02X ", psg.reg[idx] & 0xFF];
        [text appendString:@"\n"];
        for (int voice = 0; voice < 3; voice++)
            [text appendFormat:@"  Voice %c  period %4d  %8.1f Hz  tone %@  noise %@  level %2d%@  square %d\n",
                            'A' + voice, psg.period[voice], psg.hz[voice], psg.tone_on[voice] ? @"on " : @"off",
                            psg.noise_on[voice] ? @"on " : @"off", psg.level[voice], psg.envelope[voice] ? @" (envelope)" : @"",
                            psg.channel_state[voice]];
        [text appendFormat:@"  Noise      period %d\n", psg.noise_period];
        [text appendFormat:@"  Envelope   period %d  shape %X  level %d\n\n", psg.env_period, psg.env_shape,
                        psg.env_level];
    }
    setText(_soundText, text, NO);
}

@end
