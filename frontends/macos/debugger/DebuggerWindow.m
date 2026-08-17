/*
 * Debugger window (AppKit): disassembly with click-to-toggle breakpoints,
 * read-only registers, a memory view, and an STIC tab (BACKTAB, all 8
 * MOBs, GRAM/GROM card sheet with pager, palette, decoded state) -- the
 * same content and scope as the GNOME/KDE/Windows debugger windows, built
 * from plain AppKit views (an NSTabView, NSTextView/NSTextField/NSButton
 * children, and NSImageView for the STIC pictures).
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

#include "intvdebug.h"
#include "intvstic.h"
#include "symbols.h"

#define DISASM_LINES 32
#define MEM_ROWS 16
#define MEM_WORDS_PER_ROW 8
#define CARD_ROWS_PER_PAGE 8 /* 128 cards/page at INTVSTIC_CARDS_PER_ROW=16 */

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

@interface DebuggerWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(intvsession *)session;
@end

@implementation DebuggerWindow {
    intvsession *_session;
    intvdebug *_dbg;
    intvsymtab *_symtab;
    NSWindow *_window;

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
    _symFilterText = @"";
    _scratch = calloc(1, STIC_SCRATCH_BYTES);
    [self buildWindow];

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
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 560, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
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

    NSScrollView *regsScroll;
    _regs = monoView(&regsScroll);
    [regsScroll.widthAnchor constraintEqualToConstant:260].active = YES;

    _memAddr = [[NSTextField alloc] init];
    _memAddr.placeholderString = @"Memory addr (hex)";
    _memAddr.target = self;
    _memAddr.action = @selector(gotoMem:);
    NSButton *memGo = [self button:@"Go" action:@selector(gotoMem:)];
    NSStackView *memRow =
        [NSStackView stackViewWithViews:@[ _memAddr, memGo ]];
    memRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSScrollView *memScroll;
    _memView = monoView(&memScroll);
    [memScroll.widthAnchor constraintEqualToConstant:260].active = YES;

    _breakAddr = [[NSTextField alloc] init];
    _breakAddr.placeholderString = @"Symbol or hex addr";
    _breakAddr.target = self;
    _breakAddr.action = @selector(breakAt:);
    NSButton *breakGo = [self button:@"Break" action:@selector(breakAt:)];
    NSStackView *breakRow =
        [NSStackView stackViewWithViews:@[ _breakAddr, breakGo ]];
    breakRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSStackView *side = [NSStackView stackViewWithViews:@[
        [self label:@"Registers"], regsScroll, [self label:@"Memory"], memRow,
        memScroll, [self label:@"Break at"], breakRow
    ]];
    side.orientation = NSUserInterfaceLayoutOrientationVertical;
    side.alignment = NSLayoutAttributeLeading;
    side.spacing = 6;

    NSSplitView *split = [[NSSplitView alloc] init];
    split.vertical = YES;
    [split addArrangedSubview:dasmScroll];
    [split addArrangedSubview:side];
    return split;
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

    NSStackView *toolbar = [NSStackView stackViewWithViews:@[
        _runBtn, stepBtn, stepOverBtn, stepOutBtn, clearBpBtn, loadSymsBtn,
        clearSymsBtn, _status
    ]];
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 8;

    NSTabView *tabs = [[NSTabView alloc] init];
    NSTabViewItem *cpuTab = [[NSTabViewItem alloc] initWithIdentifier:@"cpu"];
    cpuTab.label = @"CPU";
    cpuTab.view = [self buildCpuTab];
    NSTabViewItem *sticTab =
        [[NSTabViewItem alloc] initWithIdentifier:@"stic"];
    sticTab.label = @"STIC";
    sticTab.view = [self buildSticTab];
    NSTabViewItem *symTab =
        [[NSTabViewItem alloc] initWithIdentifier:@"symbols"];
    symTab.label = @"Symbols";
    symTab.view = [self buildSymbolsTab];
    [tabs addTabViewItem:cpuTab];
    [tabs addTabViewItem:sticTab];
    [tabs addTabViewItem:symTab];

    NSStackView *root = [NSStackView stackViewWithViews:@[ toolbar, tabs ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                        constant:-16].active = YES;
    [tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor
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

- (void)step:(id)sender
{
    (void)sender;
    intvdebug_step(_dbg);
}

- (void)stepOver:(id)sender
{
    (void)sender;
    intvdebug_step_over(_dbg);
}

- (void)stepOut:(id)sender
{
    (void)sender;
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

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint16_t addr;
    if (intvsym_parse_addr(_symtab, _memAddr.stringValue.UTF8String, &addr)) {
        _memBase = addr;
        _seenSerial = 0;
    } else {
        _status.stringValue = [NSString
            stringWithFormat:@"No such symbol or address: %@",
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

- (void)refreshAll:(BOOL)paused
{
    _runBtn.title = paused ? @"Continue (F5)" : @"Pause (F5)";
    _status.stringValue = paused ? @"Paused" : @"Running";
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
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char symsuffix[96];
        intvsym_annotate_line(_symtab, &lines[i], symsuffix,
                              sizeof(symsuffix));
        [text appendFormat:@"%c%c%04X  %-24s%s\n",
                           lines[i].addr == r.pc ? '>' : ' ',
                           intvdebug_breakpoint_is_set(_dbg, lines[i].addr)
                               ? '*'
                               : ' ',
                           lines[i].addr, lines[i].text, symsuffix];
    }
    _disasm.string = text;
}

- (void)refreshRegs
{
    intvdebug_regs r;
    if (!intvdebug_regs_get(_dbg, &r))
        return;
    static const char *const names[8] = {"R0", "R1", "R2", "R3",
                                         "R4", "R5", "R6(SP)", "R7(PC)"};
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < 8; i++)
        [text appendFormat:@"%-8s%04X\n", names[i], r.r[i]];
    [text appendFormat:@"\nS%d C%d O%d Z%d I%d D%d", r.S, r.C, r.O, r.Z, r.I,
                       r.D];
    _regs.string = text;
}

- (void)refreshMem
{
    uint16_t words[MEM_WORDS_PER_ROW * MEM_ROWS];
    int n = intvdebug_read(_dbg, _memBase, words,
                           MEM_WORDS_PER_ROW * MEM_ROWS);
    NSMutableString *text = [NSMutableString string];
    const char *lastRegion = NULL;
    for (int row = 0; row * MEM_WORDS_PER_ROW < n; row++) {
        uint16_t rowAddr = (uint16_t)(_memBase + row * MEM_WORDS_PER_ROW);
        const char *region = intvsym_region(rowAddr);

        [text appendFormat:@"%04X ", (unsigned)rowAddr];
        for (int col = 0; col < MEM_WORDS_PER_ROW; col++) {
            int idx = row * MEM_WORDS_PER_ROW + col;
            if (idx < n)
                [text appendFormat:@" %04X", words[idx]];
        }
        /* Only label the first row of a run in the same region -- see
         * symbols.c's k_regions and the GNOME/Windows/KDE frontends' own
         * refresh_mem/refreshMem. */
        if (region && region != lastRegion)
            [text appendFormat:@"  ; %s", region];
        lastRegion = region;
        [text appendString:@"\n"];
    }
    _memView.string = text;
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
