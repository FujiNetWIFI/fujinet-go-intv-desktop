/*
 * Debugger window (AppKit): disassembly with click-to-toggle breakpoints,
 * read-only registers, a memory view, and an STIC tab (BACKTAB, all 8
 * MOBs, GRAM/GROM card sheet with pager, palette, decoded state) -- the
 * same content and scope as the GNOME/KDE/Windows debugger windows, built
 * from plain AppKit views (an NSTabView, NSTextView/NSTextField/NSButton
 * children, and NSImageView for the STIC pictures).
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
 * via NSOpenPanel, matching the GNOME/KDE/Windows ports' own UI. No
 * register/memory editing either: intvdebug has no register-write call.
 *
 * NOT BUILT OR RUN-VERIFIED -- see ../main.m's file header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"
#import "../help/HelpWindow.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "intvdebug.h"
#include "intvstic.h"
#include "memtools.h"
#include "srcmap.h"
#include "symbols.h"

#define DISASM_LINES 32
#define MEM_ROWS 16
#define MEM_WORDS_PER_ROW 8
#define CARD_ROWS_PER_PAGE 8 /* 128 cards/page at INTVSTIC_CARDS_PER_ROW=16 */
#define FUJI_TICKS 5 /* the FujiNet tab, every 0.5s while it shows */

/* Same shared-scratch sizing lesson learned the hard way in the Windows
 * port (frontends/windows/debugger/dbg_window.c's own file comment): size
 * to the LARGEST of the four STIC renders (the palette sheet), not just
 * the cards sheet -- a buffer sized only for the cards sheet would
 * silently overrun on the BACKTAB and palette renders. */
#define STIC_SCRATCH_A (INTVSTIC_BACKTAB_W * INTVSTIC_BACKTAB_H * 4)
#define STIC_SCRATCH_B (INTVSTIC_CARDS_PER_ROW * 8 * CARD_ROWS_PER_PAGE * 8 * 4)
#define STIC_SCRATCH_C (INTVSTIC_PAL_CELL * 16 * INTVSTIC_PAL_CELL * 4)
#define STIC_SCRATCH_BYTES \
    (STIC_SCRATCH_A > STIC_SCRATCH_B \
         ? (STIC_SCRATCH_A > STIC_SCRATCH_C ? STIC_SCRATCH_A : STIC_SCRATCH_C) \
         : (STIC_SCRATCH_B > STIC_SCRATCH_C ? STIC_SCRATCH_B : STIC_SCRATCH_C))

static DebuggerWindow *g_debugger;

/* The tabs, in order; INTV_DEBUGGER_TAB picks one by number or by the start
 * of its name (any case), for tests and captures. */
static const char *const kTabNames[] = {"CPU", "BASIC", "Memory", "Breakpoints", "Watches", "STIC", "Symbols", "FujiNet"};
#define TAB_COUNT ((int)(sizeof(kTabNames) / sizeof(kTabNames[0])))

static int tabFromEnv(void)
{
    const char *tab = getenv("INTV_DEBUGGER_TAB");
    if (!tab || !*tab)
        return -1;
    char *end;
    const long n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return n >= 0 && n < TAB_COUNT ? (int)n : -1;
    for (int i = 0; i < TAB_COUNT; i++)
        if (strncasecmp(tab, kTabNames[i], strlen(tab)) == 0)
            return i;
    return -1;
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

@interface DebuggerWindow () <NSWindowDelegate, NSTabViewDelegate>
- (instancetype)initWithSession:(intvsession *)session;
@end

@implementation DebuggerWindow {
    intvsession *_session;
    intvdebug *_dbg;
    intvsymtab *_symtab;
    intvsrcmap *_srcmap;
    NSWindow *_window;

    /* Memory tab: the highlighted words, and the search */
    BOOL _memHl;
    uint16_t _memHlLo, _memHlHi;
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry;
    NSTextField *_srchNote;

    /* Breakpoints tab */
    NSPopUpButton *_bpType, *_bpCond;
    NSTextField *_bpAddr, *_bpValue, *_bpStop;
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

    NSButton *_runBtn;
    NSTextField *_status;
    DasmTextView *_disasm;
    NSTextView *_regs;

    NSTextField *_memAddr;
    NSTextView *_memView;
    uint16_t _memBase;
    NSTextField *_breakAddr;

    SymTextView *_symList;
    NSTextField *_symFilter;
    NSTextField *_symCount;
    unsigned _symSeenGen;
    NSString *_symFilterText;

    NSImageView *_backtabView;
    NSImageView *_mobView[INTVSTIC_MOB_COUNT];
    NSTextView *_mobInfo;
    NSImageView *_cardsView;
    NSTextField *_cardsRangeLabel;
    int _cardFirst;
    int _cardTotal;
    NSImageView *_palView;
    NSTextView *_sticState;

    uint8_t *_scratch; /* STIC_SCRATCH_BYTES, reused across all 4 renders */

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
    int _wasPaused; /* seeded -1: force the first tick to refresh */
}

/* RGBA8888 (red first). Every intvstic_render_* output is either fully
 * opaque (backtab/cards/palette -- alpha dropped via kCGImageAlphaNoneSkip
 * Last) or, for MOBs, carries real per-pixel transparency (straight, not
 * premultiplied -- kCGImageAlphaLast). */
static NSImage *imageFromRGBA(const uint8_t *rgba, int w, int h, BOOL hasAlpha)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(NULL, rgba, (size_t)w * h * 4, NULL);
    CGBitmapInfo info = hasAlpha
                            ? (CGBitmapInfo)kCGImageAlphaLast
                            : (CGBitmapInfo)kCGImageAlphaNoneSkipLast;
    CGImageRef cg = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4,
                                  space, info, provider, NULL, false,
                                  kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    NSImage *img = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(w, h)];
    CGImageRelease(cg);
    return img;
}

/* The current line is a bar in the accent (the icon's green) with white text;
 * a breakpoint is a red dot in the gutter's first column. */
static NSColor *accentColor(void)
{
    return [NSColor colorWithSRGBRed:0x1E / 255.0 green:0x49 / 255.0 blue:0x12 / 255.0 alpha:1];
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
    NSMutableAttributedString *a = [[NSMutableAttributedString alloc]
        initWithString:line
            attributes:@{ NSFontAttributeName : font, NSForegroundColorAttributeName : NSColor.textColor }];
    if (pc) {
        [a addAttribute:NSBackgroundColorAttributeName value:accentColor() range:NSMakeRange(0, line.length)];
        [a addAttribute:NSForegroundColorAttributeName value:NSColor.whiteColor range:NSMakeRange(1, line.length - 1)];
    }
    if (bp)   /* enabled: red; disabled: a hollow grey dot */
        [a addAttribute:NSForegroundColorAttributeName
                  value:bp == 1 ? breakpointRed() : NSColor.systemGrayColor
                  range:NSMakeRange(0, 1)];
    [out appendAttributedString:a];
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
    _scratch = calloc(1, STIC_SCRATCH_BYTES);
    _fujiCount = -1;
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
    return self;
}

- (void)onTick
{
    uint64_t serial = intvdebug_stop_serial(_dbg);
    BOOL paused = intvdebug_is_paused(_dbg);
    if (serial != _seenSerial || paused != _wasPaused) {
        _seenSerial = serial;
        _wasPaused = paused;
        [self refreshAll:paused];
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

- (NSImageView *)pictureView:(int)w height:(int)h
{
    NSImageView *v = [[NSImageView alloc]
        initWithFrame:NSMakeRect(0, 0, w * 2, h * 2)];
    v.imageScaling = NSImageScaleProportionallyUpOrDown;
    v.wantsLayer = YES;
    v.layer.magnificationFilter = kCAFilterNearest;
    [v.widthAnchor constraintEqualToConstant:w * 2].active = YES;
    [v.heightAnchor constraintEqualToConstant:h * 2].active = YES;
    return v;
}

- (NSView *)buildCpuTab
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
    NSStackView *root = [NSStackView stackViewWithViews:@[ memRow, searchRow, memScroll ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [memRow.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [searchRow.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [memScroll.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
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

- (NSView *)buildSticTab
{
    _backtabView = [self pictureView:INTVSTIC_BACKTAB_W
                              height:INTVSTIC_BACKTAB_H];
    NSStackView *backtabCol = [NSStackView stackViewWithViews:@[
        [self label:@"BACKTAB"], _backtabView
    ]];
    backtabCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    backtabCol.alignment = NSLayoutAttributeLeading;

    NSMutableArray<NSView *> *mobGridRows = [NSMutableArray array];
    for (int row = 0; row < 2; row++) {
        NSMutableArray<NSView *> *rowViews = [NSMutableArray array];
        for (int col = 0; col < 4; col++) {
            int i = row * 4 + col;
            _mobView[i] = [self pictureView:16 height:16];
            [rowViews addObject:_mobView[i]];
        }
        NSStackView *rowStack = [NSStackView stackViewWithViews:rowViews];
        rowStack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        rowStack.spacing = 4;
        [mobGridRows addObject:rowStack];
    }
    NSStackView *mobGrid = [NSStackView stackViewWithViews:mobGridRows];
    mobGrid.orientation = NSUserInterfaceLayoutOrientationVertical;
    mobGrid.spacing = 4;

    NSScrollView *mobInfoScroll;
    _mobInfo = monoView(&mobInfoScroll);
    [mobInfoScroll.heightAnchor constraintEqualToConstant:130].active = YES;

    NSStackView *mobCol = [NSStackView stackViewWithViews:@[
        [self label:@"MOBs"], mobGrid, mobInfoScroll
    ]];
    mobCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    mobCol.alignment = NSLayoutAttributeLeading;
    mobCol.spacing = 6;

    _cardsView = [self pictureView:INTVSTIC_CARDS_PER_ROW * 8
                             height:CARD_ROWS_PER_PAGE * 8];
    NSButton *prevBtn = [self button:@"< Prev" action:@selector(cardsPrev:)];
    NSButton *nextBtn = [self button:@"Next >" action:@selector(cardsNext:)];
    _cardsRangeLabel = [self label:@""];
    NSStackView *pagerRow = [NSStackView
        stackViewWithViews:@[ prevBtn, nextBtn, _cardsRangeLabel ]];
    pagerRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSStackView *cardsCol = [NSStackView stackViewWithViews:@[
        [self label:@"GRAM/GROM cards"], _cardsView, pagerRow
    ]];
    cardsCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    cardsCol.alignment = NSLayoutAttributeLeading;

    _palView = [self pictureView:INTVSTIC_PAL_CELL * 16
                           height:INTVSTIC_PAL_CELL];
    NSStackView *palCol = [NSStackView stackViewWithViews:@[
        [self label:@"Palette"], _palView
    ]];
    palCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    palCol.alignment = NSLayoutAttributeLeading;

    NSScrollView *stateScroll;
    _sticState = monoView(&stateScroll);
    NSStackView *stateCol = [NSStackView stackViewWithViews:@[
        [self label:@"Registers & mode"], stateScroll
    ]];
    stateCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    stateCol.alignment = NSLayoutAttributeLeading;

    NSStackView *leftCol = [NSStackView stackViewWithViews:@[
        backtabCol, cardsCol
    ]];
    leftCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    leftCol.alignment = NSLayoutAttributeLeading;
    leftCol.spacing = 12;

    NSStackView *rightCol = [NSStackView stackViewWithViews:@[
        mobCol, palCol, stateCol
    ]];
    rightCol.orientation = NSUserInterfaceLayoutOrientationVertical;
    rightCol.alignment = NSLayoutAttributeLeading;
    rightCol.spacing = 12;

    NSStackView *root =
        [NSStackView stackViewWithViews:@[ leftCol, rightCol ]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 16;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
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

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1080, 780)
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
    NSStackView *toolbar = [NSStackView stackViewWithViews:@[
        _runBtn, stepBtn, stepOverBtn, stepOutBtn, clearBpBtn, loadSymsBtn,
        clearSymsBtn, _status, helpBtn
    ]];
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 8;

    NSTabView *tabs = [[NSTabView alloc] init];
    NSTabViewItem *cpuTab = [[NSTabViewItem alloc] initWithIdentifier:@"cpu"];
    cpuTab.label = @(kTabNames[0]);
    cpuTab.view = [self buildCpuTab];
    _basicItem = [[NSTabViewItem alloc] initWithIdentifier:@"basic"];
    _basicItem.label = @(kTabNames[1]);
    _basicItem.view = [self buildBasicTab];
    NSTabViewItem *memTab = [[NSTabViewItem alloc] initWithIdentifier:@"memory"];
    memTab.label = @(kTabNames[2]);
    memTab.view = [self buildMemoryTab];
    NSTabViewItem *bpTab = [[NSTabViewItem alloc] initWithIdentifier:@"breakpoints"];
    bpTab.label = @(kTabNames[3]);
    bpTab.view = [self buildBreaksTab];
    NSTabViewItem *wtTab = [[NSTabViewItem alloc] initWithIdentifier:@"watches"];
    wtTab.label = @(kTabNames[4]);
    wtTab.view = [self buildWatchesTab];
    NSTabViewItem *sticTab =
        [[NSTabViewItem alloc] initWithIdentifier:@"stic"];
    sticTab.label = @(kTabNames[5]);
    sticTab.view = [self buildSticTab];
    NSTabViewItem *symTab =
        [[NSTabViewItem alloc] initWithIdentifier:@"symbols"];
    symTab.label = @(kTabNames[6]);
    symTab.view = [self buildSymbolsTab];
    _fujiItem = [[NSTabViewItem alloc] initWithIdentifier:@"fujinet"];
    _fujiItem.label = @(kTabNames[7]);
    _fujiItem.view = [self buildFujinetTab];
    [tabs addTabViewItem:cpuTab];
    [tabs addTabViewItem:_basicItem];
    [tabs addTabViewItem:memTab];
    [tabs addTabViewItem:bpTab];
    [tabs addTabViewItem:wtTab];
    [tabs addTabViewItem:sticTab];
    [tabs addTabViewItem:symTab];
    [tabs addTabViewItem:_fujiItem];
    _tabs = tabs;
    tabs.delegate = self;
    if (tabFromEnv() >= 0)
        [tabs selectTabViewItemAtIndex:tabFromEnv()];
    [self updateHelpContext];

    /* The registers sit under every tab. */
    NSScrollView *regsScroll;
    _regs = monoView(&regsScroll);
    [regsScroll.heightAnchor constraintEqualToConstant:44].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[ toolbar, tabs, regsScroll ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                        constant:-16].active = YES;
    [tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                     constant:-16].active = YES;
    [regsScroll.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                           constant:-16].active = YES;
    [tabs.heightAnchor constraintGreaterThanOrEqualToConstant:660].active =
        YES;

    _window.contentView = root;
    [_window setContentSize:NSMakeSize(1080, 780)];
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
    return _tabs.selectedTabViewItem == _basicItem;
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

- (void)cardsPrev:(id)sender
{
    (void)sender;
    int page = INTVSTIC_CARDS_PER_ROW * CARD_ROWS_PER_PAGE;
    _cardFirst -= page;
    if (_cardFirst < 0)
        _cardFirst = 0;
    _seenSerial = 0;
    if (intvdebug_is_paused(_dbg))
        [self refreshStic];
}

- (void)cardsNext:(id)sender
{
    (void)sender;
    int page = INTVSTIC_CARDS_PER_ROW * CARD_ROWS_PER_PAGE;
    int lastPage = _cardTotal > 0 ? ((_cardTotal - 1) / page) * page : 0;
    _cardFirst += page;
    if (_cardFirst > lastPage)
        _cardFirst = lastPage;
    _seenSerial = 0;
    if (intvdebug_is_paused(_dbg))
        [self refreshStic];
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
    (void)sender;
    intvdebug_set_engaged(_dbg, 0);
    [_window orderOut:nil];
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
    if (line != _basPcLine) {
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

/* ---- Breakpoints, Watches, memory search ------------------------------------- */

/* One line on why the machine stopped. */
- (NSString *)stopText:(const intvdebug_stop_info *)st
{
    switch (st->reason) {
    case INTVDEBUG_STOP_WATCH_WRITE:
        return [NSString stringWithFormat:@"wrote $%04X = $%02X (was $%02X), now at $%04X", st->addr,
                                          st->value & 0xFF, st->old_value & 0xFF, st->pc];
    case INTVDEBUG_STOP_WATCH_READ:
        return [NSString stringWithFormat:@"read $%04X, now at $%04X", st->addr, st->pc];
    case INTVDEBUG_STOP_BREAKPOINT:
        return [NSString stringWithFormat:@"breakpoint at $%04X", st->pc];
    }
    return @"";
}

/* Text for a pick list, the picked line drawn with a background. */
static void setPickText(PickTextView *view, NSArray<NSString *> *lines)
{
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
    NSFont *font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    int i = 0;
    for (NSString *l in lines) {
        NSMutableAttributedString *a = [[NSMutableAttributedString alloc]
            initWithString:[l stringByAppendingString:@"\n"]
                attributes:@{ NSFontAttributeName : font, NSForegroundColorAttributeName : NSColor.textColor }];
        if (i == view.picked) {
            [a addAttribute:NSBackgroundColorAttributeName value:accentColor() range:NSMakeRange(0, a.length)];
            [a addAttribute:NSForegroundColorAttributeName value:NSColor.whiteColor range:NSMakeRange(0, a.length)];
        }
        [text appendAttributedString:a];
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
    _bpCond = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_bpCond addItemsWithTitles:@[ @"any value", @"changed", @"equals", @"not equal" ]];
    _bpCond.toolTip = @"A condition on the value written (Write and Access)";
    _bpValue = [[NSTextField alloc] init];
    _bpValue.placeholderString = @"value (hex)";
    [_bpValue.widthAnchor constraintEqualToConstant:100].active = YES;
    NSStackView *form = [NSStackView stackViewWithViews:@[
        _bpType, _bpAddr, _bpCond, _bpValue, [self button:@"Add" action:@selector(addBreak:)]
    ]];
    form.orientation = NSUserInterfaceLayoutOrientationHorizontal;

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
        intvdebug_stop_info st;
        const BOOL paused = intvdebug_is_paused(_dbg);
        NSString *why = paused && intvdebug_last_stop(_dbg, &st) ? [self stopText:&st] : @"";
        _bpStop.stringValue = !paused ? @"Running." : why.length ? [@"Stopped: " stringByAppendingString:why] : @"Paused.";
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
    intvdebug_bp bp;
    memset(&bp, 0, sizeof(bp));

    if (type == 0 ? !intvsym_parse_addr(_symtab, text, &lo)
                  : !intvsym_parse_range(_symtab, text, &lo, &hi, NULL)) {
        _status.stringValue = [NSString stringWithFormat:@"No such symbol, variable or address: %@", _bpAddr.stringValue];
        return;
    }
    if (type == 0)
        hi = lo;
    bp.type = type == 0 ? INTVDEBUG_BP_EXEC : type == 1 ? INTVDEBUG_BP_READ
              : type == 2 ? INTVDEBUG_BP_WRITE : (INTVDEBUG_BP_READ | INTVDEBUG_BP_WRITE);
    bp.lo = lo;
    bp.hi = hi;
    bp.cond = (int)_bpCond.indexOfSelectedItem;
    {
        const char *v = _bpValue.stringValue.UTF8String;
        if (*v == '$')
            v++;
        bp.value = (uint16_t)strtoul(v, NULL, 16);
    }
    bp.mask = lo >= 0x0100 && hi <= 0x01EF ? 0x00FF : 0xFFFF;   /* the scratchpad is 8 bits */
    if (intvdebug_bp_add(_dbg, &bp) < 0) {
        _status.stringValue = @"The breakpoint table is full";
        return;
    }
    _bpAddr.stringValue = @"";
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
    uint16_t lo, hi;
    if (_wtList.picked < 0 || !intvwatch_range(_symtab, _wtList.picked, &lo, &hi))
        return;
    intvdebug_bp bp;
    memset(&bp, 0, sizeof(bp));
    bp.type = INTVDEBUG_BP_WRITE;
    bp.lo = lo;
    bp.hi = hi;
    bp.cond = INTVDEBUG_COND_CHANGED;
    bp.mask = lo >= 0x0100 && hi <= 0x01EF ? 0x00FF : 0xFFFF;
    if (intvdebug_bp_add(_dbg, &bp) >= 0)
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

- (void)refreshAll:(BOOL)paused
{
    _runBtn.title = paused ? @"Continue (F5)" : @"Pause (F5)";
    /* the action on offer wears the accent: Continue while stopped */
    _runBtn.bezelColor = paused ? accentColor() : nil;
    _runBtn.contentTintColor = paused ? NSColor.whiteColor : nil;
    {
        intvdebug_stop_info st;
        NSString *why = nil;
        if (paused && intvdebug_last_stop(_dbg, &st) && st.reason != INTVDEBUG_STOP_BREAKPOINT)
            why = [self stopText:&st];
        _status.stringValue = why.length ? [@"Paused: " stringByAppendingString:why]
                                         : paused ? @"Paused" : @"Running";
    }
    if (paused) {
        [self refreshRegs];
        [self refreshDisasm];
        [self refreshMem];
        [self refreshStic];
    } else {
        _disasm.string = @"";
        _regs.string = @"";
        _memView.string = @"";
    }
    /* Independent of run/pause -- the symbol list reflects what's loaded,
     * not machine state, and is cheap to skip when nothing changed (see
     * refreshSymbols's own comment). */
    [self refreshSymbols];
    [self refreshBasic:paused];
    [self refreshBreaks];
    [self refreshWatches];
    if ([self fujiShowing])
        [self refreshFujinet:NO];
}

- (BOOL)fujiShowing
{
    return _tabs.selectedTabViewItem == _fujiItem;
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
}

- (void)refreshRegs
{
    intvdebug_regs r;
    if (!intvdebug_regs_get(_dbg, &r))
        return;
    _regs.string = [NSString
        stringWithFormat:@"R0 %04X  R1 %04X  R2 %04X  R3 %04X  R4 %04X  R5 %04X  R6(SP) %04X  R7(PC) %04X\n"
                         @"S%d C%d O%d Z%d I%d  D%d",
                         r.r[0], r.r[1], r.r[2], r.r[3], r.r[4], r.r[5], r.r[6], r.r[7],
                         r.S, r.C, r.O, r.Z, r.I, r.D];
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
            const uint16_t a = (uint16_t)(rowAddr + col);
            if (idx >= n)
                continue;
            [text appendString:@" "];
            if (_memHl && a >= _memHlLo && a <= _memHlHi)   /* a variable, symbol or hit */
                [marks addObject:[NSValue valueWithRange:NSMakeRange(text.length, 4)]];
            [text appendFormat:@"%04X", words[idx]];
        }
        /* Only label the first row of a run in the same region -- see
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
    for (NSValue *v in marks) {
        [styled addAttribute:NSBackgroundColorAttributeName value:accentColor() range:v.rangeValue];
        [styled addAttribute:NSForegroundColorAttributeName value:NSColor.whiteColor range:v.rangeValue];
    }
    setAttrText(_memView, styled);
}

- (void)refreshStic
{
    intvstic_snapshot snap;
    char text[2048];

    intvstic_snapshot_get(_dbg, &snap);

    intvstic_render_backtab(&snap, _scratch);
    _backtabView.image = imageFromRGBA(_scratch, INTVSTIC_BACKTAB_W,
                                       INTVSTIC_BACKTAB_H, NO);

    NSMutableString *mobtext = [NSMutableString
        stringWithString:@"## X   Y  CARD CLR SZx SZy VIS PRI GRAM COLL\n"];
    for (int i = 0; i < INTVSTIC_MOB_COUNT; i++) {
        intvstic_mob mob;
        intvstic_mob_get(&snap, i, &mob);
        intvstic_render_mob(&snap, i, _scratch);
        _mobView[i].image = imageFromRGBA(_scratch, 16, 16, YES);
        [mobtext appendFormat:@"%d  %3d %3d  %3d  %2d  %dx  %dx  %d   %d   "
                              @"%d    %03X\n",
                              i, mob.x, mob.y, mob.card, mob.color,
                              mob.x_size, mob.y_stretch, mob.visible,
                              mob.priority, mob.gram, mob.collision];
    }
    _mobInfo.string = mobtext;

    int total = intvstic_card_count(&snap);
    _cardTotal = total;
    if (_cardFirst >= total)
        _cardFirst = 0;
    intvstic_render_cards(&snap, _cardFirst, CARD_ROWS_PER_PAGE, _scratch);
    _cardsView.image = imageFromRGBA(_scratch, INTVSTIC_CARDS_PER_ROW * 8,
                                     CARD_ROWS_PER_PAGE * 8, NO);
    {
        int page = INTVSTIC_CARDS_PER_ROW * CARD_ROWS_PER_PAGE;
        int lastShown =
            (_cardFirst + page < total ? _cardFirst + page : total) - 1;
        _cardsRangeLabel.stringValue = [NSString
            stringWithFormat:@"cards %d–%d of %d", _cardFirst,
                            lastShown, total];
    }

    intvstic_render_palette(_dbg, _scratch);
    _palView.image = imageFromRGBA(_scratch, INTVSTIC_PAL_CELL * 16,
                                   INTVSTIC_PAL_CELL, NO);

    intvstic_format_state(&snap, text, (int)sizeof(text));
    _sticState.string = [NSString stringWithUTF8String:text];
}

@end
