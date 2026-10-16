// ============================================================================
// YesOS Desktop - virtio-input: keyboard, tablet and mouse.
// Rewritten from the lab's drivers/virtio_input.c: BARs from the core's PCI
// allocator (the lab had its own fixed window), rings with their natural
// layout (the lab's were packed: see the note in core/drivers/virtio_blk.c),
// the axis range read from the device, and keys turned into UTF-8 for the
// console instead of PS/2 scancodes.
// ============================================================================

#include "input.h"
#include "display.h"
#include "pci.h"
#include "ports.h"
#include "kprintf.h"
#include "memory.h"
#include "shell.h"
#include "vfs.h"
#include "desktop.h"

#define VIRTIO_VENDOR             0x1AF4
#define VIRTIO_DEV_INPUT          0x1052   // 0x1040 + 18

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define COMMON_DFSELECT           0x00
#define COMMON_DF                 0x04
#define COMMON_GFSELECT           0x08
#define COMMON_GF                 0x0C
#define COMMON_STATUS             0x14
#define COMMON_Q_SELECT           0x16
#define COMMON_Q_SIZE             0x18
#define COMMON_Q_ENABLE           0x1C
#define COMMON_Q_NOFF             0x1E
#define COMMON_Q_DESCLO           0x20
#define COMMON_Q_DESCHI           0x24
#define COMMON_Q_AVAILLO          0x28
#define COMMON_Q_AVAILHI          0x2C
#define COMMON_Q_USEDLO           0x30
#define COMMON_Q_USEDHI           0x34

#define STATUS_ACKNOWLEDGE        1
#define STATUS_DRIVER             2
#define STATUS_DRIVER_OK          4
#define STATUS_FEATURES_OK        8

#define VRING_DESC_F_WRITE        2

// Device configuration (virtio 1.x, 5.8.4): select, subsel, size, data at +8.
#define CFG_SELECT                0
#define CFG_SUBSEL                1
#define CFG_SIZE                  2
#define CFG_DATA                  8
#define CFG_ID_NAME               0x01
#define CFG_EV_BITS               0x11
#define CFG_ABS_INFO              0x12

// Linux input event codes
#define EV_SYN                    0x00
#define EV_KEY                    0x01
#define EV_REL                    0x02
#define EV_ABS                    0x03
#define SYN_REPORT                0
#define ABS_X                     0x00
#define ABS_Y                     0x01
#define REL_X                     0x00
#define REL_Y                     0x01
#define REL_WHEEL                 0x08
#define BTN_LEFT                  0x110
#define BTN_RIGHT                 0x111
#define BTN_MIDDLE                0x112
#define BTN_GEAR_DOWN             0x150
#define BTN_GEAR_UP               0x151

#define KEY_BACKSPACE             14
#define KEY_TAB                   15
#define KEY_ENTER                 28
#define KEY_LEFTCTRL              29
#define KEY_LEFTSHIFT             42
#define KEY_RIGHTSHIFT            54
#define KEY_LEFTALT               56
#define KEY_SPACE                 57
#define KEY_CAPSLOCK              58
#define KEY_KPENTER               96
#define KEY_RIGHTCTRL             97
#define KEY_RIGHTALT              100
#define KEY_HOME                  102
#define KEY_UP                    103
#define KEY_LEFT                  105
#define KEY_RIGHT                 106
#define KEY_END                   107
#define KEY_DOWN                  108
#define KEY_DELETE                111
#define KEY_PAGEUP                104
#define KEY_ESC                   1
#define KEY_F2                    60
#define KEY_F5                    63
#define KEY_F7                    65
#define KEY_PAGEDOWN              109

#define LAYOUT_FILE               "/input.cfg"

#define QUEUE_SIZE                64

struct vring_desc { U64 addr; U32 len; U16 flags; U16 next; };
struct vring_used_elem { U32 id; U32 len; };
struct input_event { U16 type; U16 code; U32 value; };
_Static_assert(sizeof(struct vring_desc) == 16, "virtq descriptor is 16 bytes");
_Static_assert(sizeof(struct input_event) == 8, "virtio_input_event is 8 bytes");

typedef struct {
    struct vring_desc desc[QUEUE_SIZE] __attribute__((aligned(16)));
    struct { U16 flags; volatile U16 idx; U16 ring[QUEUE_SIZE]; U16 used_event; } avail __attribute__((aligned(2)));
    struct { U16 flags; volatile U16 idx; struct vring_used_elem ring[QUEUE_SIZE]; U16 avail_event; } used __attribute__((aligned(4)));
    struct input_event events[QUEUE_SIZE];
} Queue;

typedef struct {
    InputKind kind;
    U8        slot;
    char      name[48];
    uintptr_t common, notify, cfg;
    U16       qsize, last_used;
    I32       abs_min[2], abs_max[2];
    I32       pend_x, pend_y;      // tablet: axis values until the next SYN_REPORT
    Bool      moved;
    Queue*    q;
} Device;

static InputState g;
static Device     s_dev[INPUT_MAX_DEVICES];
static Queue      s_queue[INPUT_MAX_DEVICES] __attribute__((aligned(4096)));

InputState* input_state(U0) { return &g; }

// ---- keyboard layouts ----------------------------------------------------------
// Code points per Linux key code: [normal, shift] (US) and [normal, shift,
// AltGr] (Spanish). Letters are handled apart (both layouts are QWERTY).

#define DEAD_ACUTE      0xF001
#define DEAD_DIAERESIS  0xF002
#define DEAD_GRAVE      0xF003
#define DEAD_CIRCUMFLEX 0xF004

static const char s_letters[128] = {
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't', [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g', [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b', [49] = 'n', [50] = 'm',
};

static const U16 s_us[128][2] = {
    [2] = {'1', '!'}, [3] = {'2', '@'}, [4] = {'3', '#'}, [5] = {'4', '$'}, [6] = {'5', '%'},
    [7] = {'6', '^'}, [8] = {'7', '&'}, [9] = {'8', '*'}, [10] = {'9', '('}, [11] = {'0', ')'},
    [12] = {'-', '_'}, [13] = {'=', '+'}, [26] = {'[', '{'}, [27] = {']', '}'},
    [39] = {';', ':'}, [40] = {'\'', '"'}, [41] = {'`', '~'}, [43] = {'\\', '|'},
    [51] = {',', '<'}, [52] = {'.', '>'}, [53] = {'/', '?'}, [86] = {'\\', '|'},
};

static const U16 s_es[128][3] = {
    [2] = {'1', '!', '|'}, [3] = {'2', '"', '@'}, [4] = {'3', 0xB7, '#'}, [5] = {'4', '$', '~'},
    [6] = {'5', '%', 0}, [7] = {'6', '&', 0xAC}, [8] = {'7', '/', 0}, [9] = {'8', '(', 0},
    [10] = {'9', ')', 0}, [11] = {'0', '=', 0}, [12] = {'\'', '?', 0}, [13] = {0xA1, 0xBF, 0},
    [26] = {DEAD_GRAVE, DEAD_CIRCUMFLEX, '['}, [27] = {'+', '*', ']'},
    [39] = {0xF1, 0xD1, 0}, [40] = {DEAD_ACUTE, DEAD_DIAERESIS, '{'}, [41] = {0xBA, 0xAA, '\\'},
    [43] = {0xE7, 0xC7, '}'}, [51] = {',', ';', 0}, [52] = {'.', ':', 0}, [53] = {'-', '_', 0},
    [86] = {'<', '>', 0},
};

static const U16 s_keypad[128] = {
    [55] = '*', [71] = '7', [72] = '8', [73] = '9', [74] = '-', [75] = '4', [76] = '5', [77] = '6',
    [78] = '+', [79] = '1', [80] = '2', [81] = '3', [82] = '0', [83] = '.', [98] = '/',
};

// Dead key + vowel. Rows: acute, diaeresis, grave, circumflex; columns a e i o u.
static const U8 s_compose[4][5] = {
    {0xE1, 0xE9, 0xED, 0xF3, 0xFA},
    {0xE4, 0xEB, 0xEF, 0xF6, 0xFC},
    {0xE0, 0xE8, 0xEC, 0xF2, 0xF9},
    {0xE2, 0xEA, 0xEE, 0xF4, 0xFB},
};
static const U16 s_dead_alone[4] = {0xB4, 0xA8, '`', '^'};   // the accent by itself

static Bool s_shift_l, s_shift_r, s_ctrl, s_alt, s_altgr, s_caps;
static U16  s_dead;                 // pending dead key, 0 if none

static U0 type_cp(U32 cp) {
    g.chars_typed++;
    if (desktop_key(cp)) return;                    // the focused window's program took it
    char buf[4];
    U32 n;
    if (cp < 0x80) { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    for (U32 i = 0; i < n; i++) shell_process_char(buf[i]);
}

// A key that is not a character: to the focused window's program if it
// takes it, else to the console (as a character or an escape sequence).
static U0 send_key(U32 wkey, const char* console) {
    s_dead = 0;
    if (desktop_key(wkey)) return;
    while (*console) shell_process_char(*console++);
}

static U32 compose(U16 dead, U32 cp) {
    static const char vowels[] = "aeiou";
    for (U32 v = 0; v < 5; v++) {
        if (cp == (U32)vowels[v]) return s_compose[dead - DEAD_ACUTE][v];
        if (cp == (U32)vowels[v] - 32) return s_compose[dead - DEAD_ACUTE][v] - 0x20;
    }
    return 0;
}

static U0 key_press(U16 code) {
    g.key_presses++;
    switch (code) {
    case KEY_LEFTSHIFT:  s_shift_l = TRUE; return;
    case KEY_RIGHTSHIFT: s_shift_r = TRUE; return;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: s_ctrl = TRUE; return;
    case KEY_LEFTALT:    s_alt = TRUE; return;
    case KEY_RIGHTALT:   s_altgr = TRUE; return;
    case KEY_CAPSLOCK:   s_caps = !s_caps; return;
    case KEY_ENTER: case KEY_KPENTER: send_key(WKEY_ENTER, "\r"); return;
    case KEY_BACKSPACE:  send_key(WKEY_BACKSPACE, "\b"); return;
    // Editing keys: for the console, the escape sequences a terminal sends.
    case KEY_UP:       send_key(WKEY_UP, "\x1b[A"); return;
    case KEY_DOWN:     send_key(WKEY_DOWN, "\x1b[B"); return;
    case KEY_RIGHT:    send_key(WKEY_RIGHT, "\x1b[C"); return;
    case KEY_LEFT:     send_key(WKEY_LEFT, "\x1b[D"); return;
    case KEY_HOME:     send_key(WKEY_HOME, "\x1b[H"); return;
    case KEY_END:      send_key(WKEY_END, "\x1b[F"); return;
    case KEY_DELETE:   send_key(WKEY_DELETE, "\x1b[3~"); return;
    case KEY_PAGEUP:   send_key(WKEY_PAGEUP, ""); return;
    case KEY_PAGEDOWN: send_key(WKEY_PAGEDOWN, ""); return;
    case KEY_ESC:      send_key(WKEY_ESCAPE, ""); return;   // nothing for the console
    case KEY_F2:       send_key(WKEY_F2, ""); return;
    case KEY_F5:       send_key(WKEY_F5, ""); return;
    case KEY_F7:       send_key(WKEY_F7, ""); return;
    default: break;
    }
    if (s_ctrl || s_alt || code >= 128) return;
    Bool shift = s_shift_l || s_shift_r;
    U32 cp = 0;
    if (code == KEY_SPACE || code == KEY_TAB) cp = ' ';
    else if (s_keypad[code]) cp = s_keypad[code];
    else if (s_letters[code]) {
        if (s_altgr) cp = (g.layout == LAYOUT_ES && code == 18) ? 0x20AC : 0;   // AltGr+E: euro sign
        else cp = (U32)s_letters[code] - ((shift != s_caps) ? 32 : 0);
    } else if (g.layout == LAYOUT_US) {
        if (!s_altgr) cp = s_us[code][shift ? 1 : 0];
    } else {
        cp = s_es[code][s_altgr ? 2 : shift ? 1 : 0];
        if (s_caps && !s_altgr && (code == 39 || code == 43)) cp = s_es[code][shift ? 0 : 1];   // ñ, ç
    }
    if (!cp) return;
    if (cp >= DEAD_ACUTE && cp <= DEAD_CIRCUMFLEX) {
        if (s_dead) {                               // two dead keys: both accents by themselves
            type_cp(s_dead_alone[s_dead - DEAD_ACUTE]);
            s_dead = 0;
            type_cp(s_dead_alone[cp - DEAD_ACUTE]);
        } else {
            s_dead = (U16)cp;
        }
        return;
    }
    if (s_dead) {
        U16 dead = s_dead;
        s_dead = 0;
        U32 c = compose(dead, cp);
        if (c) { type_cp(c); return; }
        type_cp(s_dead_alone[dead - DEAD_ACUTE]);
        if (cp == ' ') return;                      // dead key + space: the accent alone
    }
    type_cp(cp);
}

static U0 key_release(U16 code) {
    switch (code) {
    case KEY_LEFTSHIFT:  s_shift_l = FALSE; break;
    case KEY_RIGHTSHIFT: s_shift_r = FALSE; break;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: s_ctrl = FALSE; break;
    case KEY_LEFTALT:    s_alt = FALSE; break;
    case KEY_RIGHTALT:   s_altgr = FALSE; break;
    default: break;
    }
}

// ---- pointer -----------------------------------------------------------------------

static U0 screen_size(I32* w, I32* h) {
    DisplayState* d = display_state();
    *w = d->ready ? (I32)d->width : DISPLAY_DEFAULT_W;
    *h = d->ready ? (I32)d->height : DISPLAY_DEFAULT_H;
}

static U0 pointer_to(I32 x, I32 y) {
    I32 w, h;
    screen_size(&w, &h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > w - 1) x = w - 1;
    if (y > h - 1) y = h - 1;
    g.x = x;
    g.y = y;
    display_pointer(x, y, TRUE);
    desktop_pointer(x, y, g.buttons);
}

// Absolute axis value to a pixel: min -> 0, max -> size - 1.
static I32 abs_to_px(const Device* dev, U32 axis, I32 v, I32 size) {
    I64 lo = dev->abs_min[axis], hi = dev->abs_max[axis];
    if (hi <= lo) return 0;
    if (v < lo) v = (I32)lo;
    if (v > hi) v = (I32)hi;
    return (I32)(((I64)v - lo) * (size - 1) / (hi - lo));
}

static U0 button(U32 b, Bool down) {
    if (down) {
        g.buttons |= 1U << b;
        g.clicks[b]++;
        g.click_x = g.x;
        g.click_y = g.y;
    } else {
        g.buttons &= ~(1U << b);
    }
    desktop_pointer(g.x, g.y, g.buttons);
}

static U0 handle_event(Device* dev, const struct input_event* ev) {
    g.events++;
    if (ev->type == EV_KEY) {
        if (ev->code == BTN_LEFT || ev->code == BTN_RIGHT || ev->code == BTN_MIDDLE) {
            button(ev->code - BTN_LEFT, ev->value != 0);
        } else if (ev->code == BTN_GEAR_UP || ev->code == BTN_GEAR_DOWN) {
            if (ev->value) g.wheel += ev->code == BTN_GEAR_UP ? 1 : -1;
        } else if (ev->value) {
            key_press(ev->code);                    // 1 press, 2 auto-repeat
        } else {
            key_release(ev->code);
        }
    } else if (ev->type == EV_ABS && ev->code <= ABS_Y) {
        if (ev->code == ABS_X) dev->pend_x = (I32)ev->value;
        else dev->pend_y = (I32)ev->value;
        dev->moved = TRUE;
    } else if (ev->type == EV_REL) {
        I32 v = (I32)ev->value;
        if (ev->code == REL_X) pointer_to(g.x + v, g.y);
        else if (ev->code == REL_Y) pointer_to(g.x, g.y + v);
        else if (ev->code == REL_WHEEL) g.wheel += v;
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT && dev->moved) {
        I32 w, h;
        screen_size(&w, &h);
        dev->moved = FALSE;
        pointer_to(abs_to_px(dev, 0, dev->pend_x, w), abs_to_px(dev, 1, dev->pend_y, h));
    }
}

U0 input_poll(U0) {
    for (U32 d = 0; d < g.devices; d++) {
        Device* dev = &s_dev[d];
        Queue* q = dev->q;
        Bool returned = FALSE;
        while (dev->last_used != q->used.idx) {
            asm volatile("fence rw,rw" ::: "memory");
            U32 id = q->used.ring[dev->last_used % dev->qsize].id;
            dev->last_used++;
            if (id >= dev->qsize) continue;
            struct input_event ev = q->events[id];
            q->avail.ring[q->avail.idx % dev->qsize] = (U16)id;   // give the buffer back
            asm volatile("fence rw,rw" ::: "memory");
            q->avail.idx++;
            returned = TRUE;
            handle_event(dev, &ev);
        }
        if (returned) {
            asm volatile("fence rw,rw" ::: "memory");
            mmio_write16(dev->notify, 0);
        }
    }
}

// ---- device setup --------------------------------------------------------------------

static U32 cfg_query(const Device* dev, U8 select, U8 subsel) {
    mmio_write8(dev->cfg + CFG_SELECT, select);
    mmio_write8(dev->cfg + CFG_SUBSEL, subsel);
    return mmio_read8(dev->cfg + CFG_SIZE);
}

static Bool start_device(const PciDevice* pd) {
    if (g.devices >= INPUT_MAX_DEVICES) return FALSE;
    Device* dev = &s_dev[g.devices];
    MemSet(dev, 0, sizeof(*dev));
    dev->slot = pd->slot;
    dev->q = &s_queue[g.devices];
    MemSet(dev->q, 0, sizeof(Queue));
    uintptr_t notify_base = 0;
    U32 notify_mult = 0;

    if (!(mmio_read16(pd->ecam + PCI_REG_STATUS) & PCI_STATUS_CAP_LIST)) return FALSE;
    for (U8 cap = mmio_read8(pd->ecam + PCI_REG_CAP_PTR) & ~3; cap; cap = mmio_read8(pd->ecam + cap + 1) & ~3) {
        if (mmio_read8(pd->ecam + cap) != PCI_CAP_ID_VNDR) continue;
        U8 type = mmio_read8(pd->ecam + cap + 3), bar = mmio_read8(pd->ecam + cap + 4);
        if (bar >= 6 || !pd->bar[bar]) continue;
        uintptr_t at = pd->bar[bar] + mmio_read32(pd->ecam + cap + 8);
        if (type == VIRTIO_PCI_CAP_COMMON_CFG) dev->common = at;
        else if (type == VIRTIO_PCI_CAP_DEVICE_CFG) dev->cfg = at;
        else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG) { notify_base = at; notify_mult = mmio_read32(pd->ecam + cap + 16); }
    }
    if (!dev->common || !dev->cfg || !notify_base) {
        kprintf("[INPUT] PCI %02x.0: virtio capabilities missing\n", pd->slot);
        return FALSE;
    }

    // Identify: name, and which event types it sends.
    U32 n = cfg_query(dev, CFG_ID_NAME, 0);
    if (n > sizeof(dev->name) - 1) n = sizeof(dev->name) - 1;
    for (U32 i = 0; i < n; i++) dev->name[i] = (char)mmio_read8(dev->cfg + CFG_DATA + i);
    dev->name[n] = '\0';
    Bool has_abs = cfg_query(dev, CFG_EV_BITS, EV_ABS) != 0;
    Bool has_rel = cfg_query(dev, CFG_EV_BITS, EV_REL) != 0;
    dev->kind = has_abs ? INPUT_TABLET : has_rel ? INPUT_MOUSE : INPUT_KEYBOARD;
    for (U32 axis = 0; axis < 2 && has_abs; axis++) {
        dev->abs_max[axis] = 32767;                 // QEMU's range, if the device does not say
        if (cfg_query(dev, CFG_ABS_INFO, (U8)axis) >= 8) {
            dev->abs_min[axis] = (I32)mmio_read32(dev->cfg + CFG_DATA);
            dev->abs_max[axis] = (I32)mmio_read32(dev->cfg + CFG_DATA + 4);
        }
    }

    // Handshake: reset, ACKNOWLEDGE | DRIVER, VIRTIO_F_VERSION_1 only.
    uintptr_t c = dev->common;
    mmio_write8(c + COMMON_STATUS, 0);
    while (mmio_read8(c + COMMON_STATUS) != 0) {}
    mmio_write8(c + COMMON_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    mmio_write32(c + COMMON_DFSELECT, 1);
    if (!(mmio_read32(c + COMMON_DF) & 1)) {
        kprintf("[INPUT] PCI %02x.0: device without VIRTIO_F_VERSION_1\n", pd->slot);
        return FALSE;
    }
    mmio_write32(c + COMMON_GFSELECT, 1);
    mmio_write32(c + COMMON_GF, 1);
    mmio_write32(c + COMMON_GFSELECT, 0);
    mmio_write32(c + COMMON_GF, 0);
    mmio_write8(c + COMMON_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(mmio_read8(c + COMMON_STATUS) & STATUS_FEATURES_OK)) {
        kprintf("[INPUT] PCI %02x.0: features refused\n", pd->slot);
        return FALSE;
    }

    // Event queue 0, every buffer handed to the device.
    mmio_write16(c + COMMON_Q_SELECT, 0);
    U16 qmax = mmio_read16(c + COMMON_Q_SIZE);
    if (qmax == 0) return FALSE;
    dev->qsize = qmax < QUEUE_SIZE ? qmax : QUEUE_SIZE;
    mmio_write16(c + COMMON_Q_SIZE, dev->qsize);
    Queue* q = dev->q;
    for (U16 i = 0; i < dev->qsize; i++) {
        q->desc[i].addr = (U64)(uintptr_t)&q->events[i];
        q->desc[i].len = sizeof(struct input_event);
        q->desc[i].flags = VRING_DESC_F_WRITE;
        q->avail.ring[i] = i;
    }
    q->avail.idx = dev->qsize;
    uintptr_t desc = (uintptr_t)q->desc, avail = (uintptr_t)&q->avail, used = (uintptr_t)&q->used;
    mmio_write32(c + COMMON_Q_DESCLO, (U32)desc);
    mmio_write32(c + COMMON_Q_DESCHI, (U32)(desc >> 32));
    mmio_write32(c + COMMON_Q_AVAILLO, (U32)avail);
    mmio_write32(c + COMMON_Q_AVAILHI, (U32)(avail >> 32));
    mmio_write32(c + COMMON_Q_USEDLO, (U32)used);
    mmio_write32(c + COMMON_Q_USEDHI, (U32)(used >> 32));
    dev->notify = notify_base + (uintptr_t)mmio_read16(c + COMMON_Q_NOFF) * notify_mult;
    mmio_write16(c + COMMON_Q_ENABLE, 1);
    asm volatile("fence rw,rw" ::: "memory");
    mmio_write8(c + COMMON_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    mmio_write16(dev->notify, 0);

    g.devices++;
    if (dev->kind == INPUT_KEYBOARD) g.keyboards++;
    else g.pointers++;
    return TRUE;
}

Bool input_init(U0) {
    MemSet(&g, 0, sizeof(g));
    I32 w, h;
    screen_size(&w, &h);
    g.x = w / 2;
    g.y = h / 2;
    PciDevice pd;
    for (U32 i = 0; i < INPUT_MAX_DEVICES && pci_find_id(VIRTIO_VENDOR, VIRTIO_DEV_INPUT, i, &pd); i++) {
        if (!pci_enable(&pd)) {
            kprintf("[INPUT] PCI %02x.0: BARs not assigned\n", pd.slot);
            continue;
        }
        start_device(&pd);
    }
    if (g.pointers) display_pointer(g.x, g.y, TRUE);
    // The layout chosen last time, kept on the RedSea disk.
    size_t n = 0;
    U8* cfg = vfs_read_file_exact(LAYOUT_FILE, &n);
    if (cfg) {
        if (n >= 9 && MemCmp(cfg, "layout es", 9) == 0) g.layout = LAYOUT_ES;
        g.layout_saved = TRUE;
        Free(cfg);
    }
    return g.devices > 0;
}

// ---- console: `input` ----------------------------------------------------------------

static const char* kind_name(InputKind k) {
    return k == INPUT_KEYBOARD ? "keyboard" : k == INPUT_TABLET ? "tablet" : "mouse";
}

static Bool input_run(const char* cmd) {
    if (StrCmp(cmd, "input") == 0 || StrCmp(cmd, "input status") == 0) {
        kprintf("Input: %u virtio-input device(s), keyboard layout %s\n", g.devices,
                g.layout == LAYOUT_ES ? "es" : "us");
        for (U32 d = 0; d < g.devices; d++) {
            Device* dev = &s_dev[d];
            kprintf("  %u: %s, %s, PCI %02x.0", d, dev->name, kind_name(dev->kind), dev->slot);
            if (dev->kind == INPUT_TABLET)
                kprintf(", x %d..%d, y %d..%d", (I64)dev->abs_min[0], (I64)dev->abs_max[0], (I64)dev->abs_min[1], (I64)dev->abs_max[1]);
            kprintf("\n");
        }
        kprintf("  Keys    : %u presses, %u characters typed\n", g.key_presses, g.chars_typed);
        kprintf("  Pointer : %d,%d, buttons %u, clicks left %u right %u middle %u, last click at %d,%d, wheel %d\n",
                (I64)g.x, (I64)g.y, (U64)g.buttons, g.clicks[0], g.clicks[1], g.clicks[2],
                (I64)g.click_x, (I64)g.click_y, g.wheel);
        kprintf("  Events  : %u\n", g.events);
        return TRUE;
    }
    if (StrCmp(cmd, "input layout us") == 0 || StrCmp(cmd, "input layout es") == 0) {
        g.layout = cmd[13] == 'e' ? LAYOUT_ES : LAYOUT_US;
        s_dead = 0;
        const char* line = g.layout == LAYOUT_ES ? "layout es\n" : "layout us\n";
        g.layout_saved = vfs_write_file(LAYOUT_FILE, line, StrLen(line));
        kprintf("[INPUT] keyboard layout %s%s\n", g.layout == LAYOUT_ES ? "es (Spanish)" : "us",
                g.layout_saved ? ", saved in " LAYOUT_FILE : " (could not be saved: no disk?)");
        return TRUE;
    }
    if (shell_is_prefix("input", cmd) && (cmd[5] == ' ' || cmd[5] == '\0')) {
        kprintf("usage: input [status] | input layout us|es\n");
        return TRUE;
    }
    return FALSE;
}

static U0 input_help(U0) {
    kprintf("  Input\n");
    kprintf("    input [status]   input layout us|es\n");
}

static const ShellExtension s_input_shell = { input_run, input_help, NULL, NULL };

U0 input_shell_register(U0) {
    shell_register(&s_input_shell);
}
