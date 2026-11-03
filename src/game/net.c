/*
 * Network play (net.h; docs/NETWORK.md).
 *
 * Packets (little-endian), all small enough for PSP ad hoc and 3DS local
 * wireless; any of them may be lost, so everything is either repeated until
 * answered or sent again in the next packet anyway:
 *   DISCOVER  joiner -> everyone     "which rooms are there?"
 *   ROOM      host -> joiner         room name, state, version check
 *   JOIN      joiner -> host         (repeated until WELCOME)
 *   WELCOME   host -> joiner         accepted or why not
 *   KEEP      both, in the room      still here (every 250 ms)
 *   CHUNK     host -> joiner         a piece of a snapshot package
 *   GOT       joiner -> host         which pieces have arrived
 *   INPUT     both, in the game      this player's inputs for the frames the other
 *                                    side has not confirmed (up to 40), what it has
 *                                    of the other's, a memory checksum
 *   BYE       both                   leaving
 *
 * Frames: inputs sampled at the end of frame f are used at the end of frame
 * f + delay on both machines (delay: NET_DELAY frames, 50 ms). Frame f's inputs
 * of both players are needed to finish frame f; a machine that does not have them
 * waits (and shows why after a moment).
 */
#include "net.h"
#include "game.h"
#include "gameview.h"
#include "save.h"
#include "core/genesis.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_PROTO       1
#define NET_BUILD       2           /* change when anything changes what the game does */
#define NET_DELAY       3
#define RING            16384       /* frames of inputs kept (4.5 minutes) */
#define SNAP_EVERY      120         /* frames between the host's snapshots */
#define TIMEOUT_MS      5000
#define CHUNK           1000
#define MAX_CHUNKS      2048
#define HASH_EVERY      32

enum { P_DISCOVER = 1, P_ROOM, P_JOIN, P_WELCOME, P_KEEP, P_CHUNK, P_GOT, P_INPUT, P_BYE };
enum { W_OK = 0, W_FULL, W_VERSION };
enum { K_START = 0, K_REJOIN, K_RESYNC };

int g_net_resume;
extern int g_gv_windows, g_gv_net_player;

static int role, phase;
static char room[NET_NAME_MAX + 1];
static uint32_t session, compat;
static PlatNetAddr peer;
static int have_peer;
static uint32_t heard_ms, kept_ms, joined_ms;
static char message[96];
static int saved_windows = -1;
static int log_on = -1;

/* lockstep */
static uint16_t in_val[2][RING];
static uint32_t in_tag[2][RING];          /* frame + 1 when the entry holds that frame */
static uint32_t nf;                       /* last frame whose inputs were used */
static uint32_t my_last;                  /* last frame of ours sampled */
static uint32_t peer_contig;              /* the other's inputs we have, all frames up to here */
static uint32_t peer_has_mine;            /* ... of ours the other has */
static int delay = NET_DELAY;
static uint32_t hash_frame[64], hash_val[64];
static uint32_t ignore_hashes_to;
static int in_stall;
/* keeping step (pads_after): how far the other machine is ahead of this one as
 * seen here, averaged (1/16 frames), and as the other sees us */
static int seen_ahead16, their_seen_ahead16;

/* host: the latest snapshot taken at the top of the main loop */
static uint8_t *snap;
static size_t snap_len, snap_cap;
static uint32_t snap_frame;
static uint16_t snap_pad[2], snap_menu;
static int snap_ok;

/* packages (snapshot + inputs), compressed, in chunks */
static struct {
    uint8_t *data;
    uint32_t len, id;
    int chunks, active, got_count;
    uint8_t got[MAX_CHUNKS];
    uint32_t sent[MAX_CHUNKS];
} pkg;

/* joiner: what the package said, for net_resumed() */
static uint16_t resume_pad[2], resume_menu;

/* rooms found */
#define MAX_ROOMS 8
static NetRoom rooms[MAX_ROOMS];
static int nrooms;
static uint32_t discover_ms;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (log_on < 0) log_on = getenv("TJE_NET_LOG") != NULL;
    if (!log_on) return;
    va_start(ap, fmt);
    fprintf(stderr, "net: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- packets ------------------------------------------------------------------ */

typedef struct { uint8_t b[PLAT_NET_MAX_PACKET]; int n; } Out;
typedef struct { const uint8_t *b; int n, pos, bad; } In;

static void o8(Out *o, uint32_t v) { if (o->n < (int)sizeof o->b) o->b[o->n++] = (uint8_t)v; }
static void o16(Out *o, uint32_t v) { o8(o, v); o8(o, v >> 8); }
static void o32(Out *o, uint32_t v) { o16(o, v); o16(o, v >> 16); }
static void obytes(Out *o, const void *p, int n) { while (n-- > 0) o8(o, *(const uint8_t *)p), p = (const uint8_t *)p + 1; }
static uint32_t i8(In *in) { if (in->pos >= in->n) { in->bad = 1; return 0; } return in->b[in->pos++]; }
static uint32_t i16(In *in) { uint32_t v = i8(in); return v | i8(in) << 8; }
static uint32_t i32(In *in) { uint32_t v = i16(in); return v | i16(in) << 16; }

static void head(Out *o, int type)
{
    o->n = 0;
    o8(o, 'T');
    o8(o, 'J');
    o8(o, NET_PROTO);
    o8(o, (uint32_t)type);
    o32(o, session);
}

static int loss_pct = -1;
static void send_to(const PlatNetAddr *to, const Out *o)
{
    if (loss_pct < 0) loss_pct = getenv("TJE_NET_LOSS") ? atoi(getenv("TJE_NET_LOSS")) : 0;
    if (loss_pct > 0 && rand() % 100 < loss_pct) return;     /* (tests: a poor connection) */
    plat_net_send(to, o->b, o->n);
}
static void send_peer(const Out *o) { if (have_peer) send_to(&peer, o); }

/* ---- the version check ------------------------------------------------------------ */

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n)
{
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static uint32_t make_compat(void)
{
    uint32_t h = 2166136261u, a, v[3] = {NET_PROTO, NET_BUILD, (uint32_t)genesis_snapshot(NULL, 0)};
    h = fnv(h, (const uint8_t *)v, sizeof v);
    for (a = 0; a < 0x80000u; a += 4) {        /* the ROM */
        uint32_t w = rd32(a);
        h = fnv(h, (const uint8_t *)&w, 4);
    }
    return h;
}

/* ---- state -------------------------------------------------------------------- */

int net_role(void) { return role; }
int net_phase(void) { return phase; }
const char *net_room_name(void) { return room; }
int net_local_player(void) { return role == NET_HOST ? 0 : role == NET_JOIN ? 1 : -1; }
int net_in_game(void)
{
    return (role == NET_HOST && (phase == NET_H_PLAYING || phase == NET_H_LOST)) ||
           (role == NET_JOIN && phase == NET_J_PLAYING);
}
int net_progress(void)
{
    return pkg.chunks ? pkg.got_count * 100 / pkg.chunks : 0;
}

int net_take_message(char *out, int size)
{
    if (!message[0]) return 0;
    snprintf(out, (size_t)size, "%s", message);
    message[0] = 0;
    return 1;
}

static void windows_on(void)
{
    if (saved_windows < 0) saved_windows = g_gv_windows;
    g_gv_windows = 1;                       /* (the same on both machines: it changes how the
                                               game splits the screen - transitions.c) */
    g_gv_net_player = net_local_player();
}

static void windows_off(void)
{
    if (saved_windows >= 0) g_gv_windows = saved_windows;
    saved_windows = -1;
    g_gv_net_player = -1;
    gameview_set_message(NULL);
}

static void pkg_clear(void)
{
    free(pkg.data);
    memset(&pkg, 0, sizeof pkg);
}

static void reset_lockstep(void)
{
    memset(in_tag, 0, sizeof in_tag);
    memset(hash_frame, 0xFF, sizeof hash_frame);
    nf = my_last = peer_contig = peer_has_mine = 0;
    ignore_hashes_to = 0;
}

static void end_session(void)
{
    role = NET_NONE;
    have_peer = 0;
    pkg_clear();
    snap_ok = 0;
    windows_off();
    g_fast_forward = 0;
    seen_ahead16 = their_seen_ahead16 = 0;
    plat_pacing(0, 0);
}

void net_leave(const char *why)
{
    if (role != NET_NONE && have_peer) {
        Out o;
        int k;
        head(&o, P_BYE);
        for (k = 0; k < 3; k++) send_peer(&o);
    }
    if (why) snprintf(message, sizeof message, "%s", why);
    logf_("leave (%s)", why ? why : "-");
    end_session();
}

/* ---- inputs ------------------------------------------------------------------- */

static int have(int p, uint32_t f) { return in_tag[p][f % RING] == f + 1; }
static void put(int p, uint32_t f, uint16_t v) { in_val[p][f % RING] = v; in_tag[p][f % RING] = f + 1; }
static uint16_t get(int p, uint32_t f) { return have(p, f) ? in_val[p][f % RING] : 0; }

static void advance_contig(void)
{
    int other = !net_local_player();
    while (have(other, peer_contig + 1)) peer_contig++;
}

/* game buttons (9 bits, Back included) and the menu controls in 15 bits */
static uint16_t pack(uint16_t pad, uint16_t menu)
{
    return (uint16_t)((pad & 0x1FF) | ((menu & 0xF) << 9) | ((menu & MENU_OK) ? 1 << 13 : 0) |
                      ((menu & MENU_BACK) ? 1 << 14 : 0));
}
static uint16_t unpack_pad(uint16_t v) { return v & 0x1FF; }
static uint16_t unpack_menu(uint16_t v)
{
    return (uint16_t)(((v >> 9) & 0xF) | ((v & (1 << 13)) ? MENU_OK : 0) | ((v & (1 << 14)) ? MENU_BACK : 0));
}

static uint32_t ram_hash(void)
{
    return fnv(2166136261u, g_ram, sizeof g_ram);
}

static void send_inputs(void)
{
    Out o;
    int me = net_local_player();
    uint32_t first = peer_has_mine + 1, f, n;
    int k = (int)((nf / HASH_EVERY) & 63);
    if (my_last < first) first = my_last + 1;
    n = my_last + 1 - first;
    if (n > 40) n = 40;
    head(&o, P_INPUT);
    o8(&o, (uint32_t)me);
    o32(&o, first);
    o8(&o, n);
    for (f = first; f < first + n; f++) o16(&o, get(me, f));
    o32(&o, peer_contig);
    /* the latest checksum (the host compares) */
    o32(&o, hash_frame[k]);
    o32(&o, hash_val[k]);
    o16(&o, (uint32_t)(uint16_t)(int16_t)seen_ahead16);
    send_peer(&o);
}

/* ---- packages: snapshot + inputs ------------------------------------------------ */

/* zero runs packed: byte c < 0x80: c+1 bytes follow as they are; c >= 0x80: a run
 * of ((c & 0x7F) << 8 | next) + 1 zero bytes */
static size_t pack_zeros(const uint8_t *src, size_t n, uint8_t *dst)
{
    size_t i = 0, o = 0;
    while (i < n) {
        size_t z = 0;
        while (i + z < n && src[i + z] == 0 && z < 0x8000) z++;
        if (z >= 3) {
            dst[o++] = (uint8_t)(0x80 | ((z - 1) >> 8));
            dst[o++] = (uint8_t)(z - 1);
            i += z;
        } else {
            size_t start = i, len = 0;
            while (i < n && len < 128) {
                if (i + 2 < n && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 0) break;
                i++, len++;
            }
            dst[o++] = (uint8_t)(len - 1);
            memcpy(dst + o, src + start, len);
            o += len;
        }
    }
    return o;
}

static size_t unpack_zeros(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t c = src[i++];
        if (c & 0x80) {
            size_t z;
            if (i >= n) return 0;
            z = ((size_t)(c & 0x7F) << 8 | src[i++]) + 1;
            if (o + z > cap) return 0;
            memset(dst + o, 0, z);
            o += z;
        } else {
            size_t len = (size_t)c + 1;
            if (i + len > n || o + len > cap) return 0;
            memcpy(dst + o, src + i, len);
            i += len, o += len;
        }
    }
    return o;
}

static void w32(uint8_t **p, uint32_t v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); (*p)[2] = (uint8_t)(v >> 16); (*p)[3] = (uint8_t)(v >> 24); *p += 4; }
static void w16(uint8_t **p, uint32_t v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); *p += 2; }
static uint32_t r32(const uint8_t **p) { uint32_t v = (*p)[0] | (*p)[1] << 8 | (*p)[2] << 16 | (uint32_t)(*p)[3] << 24; *p += 4; return v; }
static uint16_t r16(const uint8_t **p) { uint16_t v = (uint16_t)((*p)[0] | (*p)[1] << 8); *p += 2; return v; }

/* The host's package: its latest snapshot, then every input since it of both
 * players (Earl's up to `join_to`). */
static void make_package(int kind, uint32_t join_to)
{
    uint32_t nh = my_last > snap_frame ? my_last - snap_frame : 0;
    uint32_t nj = join_to > snap_frame ? join_to - snap_frame : 0, f;
    size_t raw_len = 64 + (nh + nj) * 2 + snap_len, plen;
    uint8_t *raw = (uint8_t *)malloc(raw_len), *p = raw, *packed;
    static uint32_t ids;
    if (!raw) return;
    w32(&p, 0x424E4A54u);
    w32(&p, (uint32_t)kind);
    w32(&p, snap_frame);
    w32(&p, (uint32_t)delay);
    w16(&p, snap_pad[0]);
    w16(&p, snap_pad[1]);
    w16(&p, snap_menu);
    w32(&p, nh);
    w32(&p, nj);
    for (f = 1; f <= nh; f++) w16(&p, get(0, snap_frame + f));
    for (f = 1; f <= nj; f++) w16(&p, get(1, snap_frame + f));
    w32(&p, (uint32_t)snap_len);
    memcpy(p, snap, snap_len);
    p += snap_len;
    packed = (uint8_t *)malloc(raw_len + raw_len / 128 + 16);
    if (!packed) { free(raw); return; }
    plen = pack_zeros(raw, (size_t)(p - raw), packed);
    free(raw);
    pkg_clear();
    pkg.data = packed;
    pkg.len = (uint32_t)plen;
    pkg.id = ++ids ^ (plat_ms() << 8);
    pkg.chunks = (int)((plen + CHUNK - 1) / CHUNK);
    if (pkg.chunks > MAX_CHUNKS) { pkg_clear(); return; }
    pkg.active = 1;
    peer_has_mine = my_last < snap_frame + nh ? my_last : snap_frame + nh;
    logf_("package %d: frames %u.. (%u + %u inputs), %u bytes in %d pieces", kind, snap_frame, nh, nj,
          pkg.len, pkg.chunks);
}

static void send_chunks(void)
{
    uint32_t now = plat_ms();
    int i, sent = 0;
    if (!pkg.active || role != NET_HOST || !have_peer) return;
    for (i = 0; i < pkg.chunks && sent < 24; i++) {
        Out o;
        uint32_t off = (uint32_t)i * CHUNK, n = pkg.len - off < CHUNK ? pkg.len - off : CHUNK;
        if (pkg.got[i] || (pkg.sent[i] && now - pkg.sent[i] < 200)) continue;
        head(&o, P_CHUNK);
        o32(&o, pkg.id);
        o32(&o, pkg.len);
        o16(&o, (uint32_t)i);
        obytes(&o, pkg.data + off, (int)n);
        send_peer(&o);
        pkg.sent[i] = now ? now : 1;
        sent++;
    }
}

/* Joiner: the whole package is here. Restart the machine from it (does not return). */
static void apply_package(void)
{
    size_t cap = genesis_snapshot(NULL, 0) + 64 + 2u * 2u * RING, n;
    uint8_t *raw = (uint8_t *)malloc(cap);
    const uint8_t *p;
    uint32_t kind, fs, nh, nj, slen, f, keep_from = 0, keep_to = 0;
    static uint16_t keep[RING];
    if (!raw) return;
    n = unpack_zeros(pkg.data, pkg.len, raw, cap);
    p = raw;
    if (n < 40 || r32(&p) != 0x424E4A54u) { free(raw); net_leave("The game sent was damaged."); return; }
    kind = r32(&p);
    fs = r32(&p);
    delay = (int)r32(&p);
    resume_pad[0] = r16(&p);
    resume_pad[1] = r16(&p);
    resume_menu = r16(&p);
    nh = r32(&p);
    nj = r32(&p);
    if ((size_t)(p - raw) + (nh + nj) * 2 + 4 > n || nh >= RING || nj >= RING) { free(raw); net_leave("The game sent was damaged."); return; }
    /* (a mended game: our own inputs after those in the package stay ours) */
    if (kind == K_RESYNC && my_last > fs + nj) {
        keep_from = fs + nj + 1;
        keep_to = my_last;
        if (keep_to - keep_from >= RING) keep_from = keep_to - RING + 1;
        for (f = keep_from; f <= keep_to; f++) keep[f % RING] = get(1, f);
    }
    reset_lockstep();
    for (f = 1; f <= nh; f++) put(0, fs + f, r16(&p));
    for (f = 1; f <= nj; f++) put(1, fs + f, r16(&p));
    nf = fs;
    my_last = fs + nj;
    peer_contig = fs;
    advance_contig();
    peer_has_mine = my_last;
    if (keep_to) {
        for (f = keep_from; f <= keep_to; f++) put(1, f, keep[f % RING]);
        my_last = keep_to;
    }
    slen = r32(&p);
    if ((size_t)(p - raw) + slen > n || !genesis_snapshot_valid(p, slen)) { free(raw); net_leave("The game sent does not match this version."); return; }
    logf_("restarting from frame %u (kind %u)", fs, kind);
    pkg_clear();
    in_stall = 0;
    phase = NET_J_PLAYING;
    windows_on();
    g_net_resume = 1;
    {
        /* (the restart does not return: this buffer goes with the next package) */
        static uint8_t *last;
        free(last);
        last = raw;
    }
    genesis_restart_snapshot(p, slen);     /* copies it; does not return */
}

/* ---- receiving ---------------------------------------------------------------- */

static void on_input(In *in)
{
    int p = (int)i8(in), k;
    uint32_t first = i32(in), n = i8(in), f, ack, hf, hv;
    if (p != !net_local_player()) return;
    for (f = 0; f < n; f++) {
        uint16_t v = (uint16_t)i16(in);
        if (in->bad) return;
        /* (ahead of what we keep, or long gone) */
        if (first + f > nf + RING / 2 || first + f + RING / 2 < nf) continue;
        if (!have(p, first + f)) put(p, first + f, v);
    }
    ack = i32(in);
    hf = i32(in);
    hv = i32(in);
    if (in->bad) return;
    their_seen_ahead16 = (int16_t)i16(in);
    if (in->bad) their_seen_ahead16 = 0;
    if (ack > peer_has_mine && ack <= my_last) peer_has_mine = ack;
    advance_contig();
    /* host: compare the joiner's checksum with ours for that frame */
    if (role == NET_HOST && hf != 0xFFFFFFFFu && hf > ignore_hashes_to) {
        k = (int)((hf / HASH_EVERY) & 63);
        if (hash_frame[k] == hf && hash_val[k] != hv && !pkg.active && snap_ok) {
            logf_("checksums differ at frame %u (%08X here, %08X there): mending", hf, hash_val[k], hv);
            make_package(K_RESYNC, peer_contig);
            ignore_hashes_to = nf + delay + 1;
        }
    }
}

static void on_chunk(In *in)
{
    uint32_t id = i32(in), len = i32(in), i = i16(in), n = (uint32_t)(in->n - in->pos);
    if (in->bad || role != NET_JOIN || len == 0 || len > MAX_CHUNKS * CHUNK) return;
    if (!pkg.active || pkg.id != id) {
        pkg_clear();
        pkg.data = (uint8_t *)malloc(len);
        if (!pkg.data) return;
        pkg.id = id;
        pkg.len = len;
        pkg.chunks = (int)((len + CHUNK - 1) / CHUNK);
        pkg.active = 1;
        if (phase != NET_J_PLAYING) phase = NET_J_RECEIVING;
    }
    if ((int)i >= pkg.chunks || pkg.got[i]) return;
    if (i * CHUNK + n > len || n > CHUNK) return;
    memcpy(pkg.data + i * CHUNK, in->b + in->pos, n);
    pkg.got[i] = 1;
    pkg.got_count++;
}

static void send_got(void)
{
    Out o;
    int i;
    head(&o, P_GOT);
    o32(&o, pkg.id);
    o16(&o, (uint32_t)pkg.chunks);
    for (i = 0; i < pkg.chunks; i += 8) {
        int b = 0, k;
        for (k = 0; k < 8 && i + k < pkg.chunks; k++) if (pkg.got[i + k]) b |= 1 << k;
        o8(&o, (uint32_t)b);
    }
    send_peer(&o);
}

static void on_got(In *in)
{
    uint32_t id = i32(in);
    int n = (int)i16(in), i;
    if (role != NET_HOST || !pkg.active || id != pkg.id || n != pkg.chunks) return;
    for (i = 0; i < n; i += 8) {
        int b = (int)i8(in), k;
        if (in->bad) return;
        for (k = 0; k < 8 && i + k < n; k++)
            if ((b >> k & 1) && !pkg.got[i + k]) { pkg.got[i + k] = 1; pkg.got_count++; }
    }
    if (pkg.got_count == pkg.chunks) {
        logf_("package delivered");
        pkg_clear();
        /* (checksums the joiner sent before it restarted are still on their way) */
        ignore_hashes_to = nf + 64;
    }
}

static void send_room(const PlatNetAddr *to)
{
    Out o;
    head(&o, P_ROOM);
    obytes(&o, room, NET_NAME_MAX);
    o8(&o, phase == NET_H_WAITING ? NET_ROOM_OPEN : phase == NET_H_LOST ? NET_ROOM_REJOIN : NET_ROOM_FULL);
    o32(&o, compat);
    send_to(to, &o);
}

static void welcome(const PlatNetAddr *to, int result)
{
    Out o;
    head(&o, P_WELCOME);
    o8(&o, (uint32_t)result);
    o8(&o, (uint32_t)delay);
    send_to(to, &o);
}

static void on_join(const PlatNetAddr *from, In *in)
{
    uint32_t c = i32(in);
    if (in->bad || role != NET_HOST) return;
    if (c != compat) { welcome(from, W_VERSION); return; }
    if (have_peer && plat_net_addr_equal(from, &peer)) { welcome(from, W_OK); return; }   /* (again) */
    if (phase == NET_H_WAITING || phase == NET_H_LOST) {
        peer = *from;
        have_peer = 1;
        heard_ms = plat_ms();
        welcome(from, W_OK);
        logf_("a player joined (%s)", phase == NET_H_LOST ? "back in the game" : "room");
        if (phase == NET_H_WAITING) { phase = NET_H_JOINED; return; }
        /* back into the game: Earl's missing inputs up to the frame he can start
         * sampling for are left empty, and the game goes to him */
        {
            uint32_t f, to = nf + 1 + (uint32_t)delay - 1;
            for (f = peer_contig + 1; f <= to; f++) if (!have(1, f)) put(1, f, 0);
            advance_contig();
            phase = NET_H_PLAYING;
            gameview_set_message(NULL);
            if (snap_ok) make_package(K_REJOIN, peer_contig);
        }
        return;
    }
    welcome(from, W_FULL);
}

static void on_packet(const PlatNetAddr *from, const uint8_t *b, int n)
{
    In in = {b, n, 8, 0};
    int type;
    uint32_t sid;
    if (n < 8 || b[0] != 'T' || b[1] != 'J' || b[2] != NET_PROTO) return;
    type = b[3];
    sid = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    switch (type) {
    case P_DISCOVER:
        if (role == NET_HOST) send_room(from);
        return;
    case P_ROOM:
        if (role == NET_NONE || (role == NET_JOIN && phase == NET_J_CONNECTING && !have_peer)) {
            NetRoom r;
            int i;
            memset(&r, 0, sizeof r);
            memcpy(r.name, b + 8, n >= 8 + NET_NAME_MAX ? NET_NAME_MAX : 0);
            r.name[NET_NAME_MAX] = 0;
            in.pos = 8 + NET_NAME_MAX;
            r.state = (int)i8(&in);
            r.compatible = i32(&in) == compat;
            r.addr = *from;
            r.seen = plat_ms();
            r.session = sid;
            if (in.bad) return;
            /* (the same room may answer on two routes: the local network and this
             * machine's own; the first one found is kept) */
            for (i = 0; i < nrooms && rooms[i].session != sid; i++) {}
            if (i == nrooms) { if (nrooms == MAX_ROOMS) return; nrooms++; }
            else r.addr = rooms[i].addr;
            rooms[i] = r;
        }
        return;
    case P_JOIN:
        on_join(from, &in);
        return;
    default:
        break;
    }
    /* the rest only from the other player of this session */
    if (!have_peer || !plat_net_addr_equal(from, &peer)) return;
    if (role == NET_JOIN && type == P_WELCOME) {
        int result = (int)i8(&in);
        if (in.bad || phase != NET_J_CONNECTING) return;
        if (result != W_OK) {
            net_leave(result == W_FULL ? "That room is full." : "That room has a different version of the game.");
            return;
        }
        session = sid;
        delay = (int)i8(&in);
        phase = NET_J_WAITING;
        heard_ms = plat_ms();
        logf_("joined");
        return;
    }
    if (sid != session) return;
    heard_ms = plat_ms();
    switch (type) {
    case P_KEEP:
        break;
    case P_CHUNK:
        on_chunk(&in);
        break;
    case P_GOT:
        on_got(&in);
        break;
    case P_INPUT:
        if (net_in_game()) on_input(&in);
        break;
    case P_BYE:
        if (role == NET_HOST) {
            have_peer = 0;
            pkg_clear();
            if (phase == NET_H_JOINED) phase = NET_H_WAITING;
            else if (phase == NET_H_PLAYING) phase = NET_H_LOST;
            logf_("the other player left");
        } else {
            net_leave(net_in_game() ? "ToeJam left the game." : "ToeJam closed the room.");
        }
        break;
    }
}

void net_poll(void)
{
    static uint8_t buf[PLAT_NET_MAX_PACKET + 64];
    PlatNetAddr from;
    int n, got_chunk = 0;
    uint32_t now;
    if (role == NET_NONE && !nrooms && !discover_ms) return;
    while ((n = plat_net_recv(&from, buf, sizeof buf)) > 0) {
        int was = pkg.got_count;
        on_packet(&from, buf, n);
        if (pkg.got_count != was && role == NET_JOIN) got_chunk = 1;
    }
    now = plat_ms();
    if (role == NET_JOIN && pkg.active) {
        if (got_chunk) send_got();
        if (pkg.got_count == pkg.chunks && pkg.chunks) {
            send_got();
            apply_package();          /* (does not return) */
            return;
        }
    }
    if (role == NET_HOST) send_chunks();
    if (role == NET_NONE) return;
    /* the other side silent too long */
    if (have_peer && now - heard_ms > TIMEOUT_MS) {
        if (role == NET_HOST) {
            have_peer = 0;
            pkg_clear();
            if (phase == NET_H_JOINED) phase = NET_H_WAITING;
            else if (phase == NET_H_PLAYING) phase = NET_H_LOST;
            logf_("lost the other player");
        } else {
            net_leave("Lost the connection to ToeJam.");
            return;
        }
    }
    if (role == NET_JOIN && phase == NET_J_CONNECTING && now - kept_ms > 300) {
        Out o;
        head(&o, P_JOIN);
        o32(&o, compat);
        send_peer(&o);
        kept_ms = now;
        if (now - joined_ms > 4000) { net_leave("The room did not answer."); return; }
    }
    if (!net_in_game() && have_peer && !(role == NET_JOIN && phase == NET_J_CONNECTING) && now - kept_ms > 250) {
        Out o;
        head(&o, P_KEEP);
        send_peer(&o);
        kept_ms = now;
    }
    /* (in a game, inputs are the keep-alive; while waiting, resent) */
    if (net_in_game() && in_stall && have_peer && now - kept_ms > 30) {
        send_inputs();
        kept_ms = now;
    }
}

/* ---- rooms ---------------------------------------------------------------------- */

static int opened(void)
{
    if (plat_net_open() != 0) {
        snprintf(message, sizeof message, "Network play is not available here.");
        return 0;
    }
    if (!compat) compat = make_compat();
    return 1;
}

int net_host(const char *name)
{
    if (role != NET_NONE) net_leave(NULL);
    if (!opened()) return -1;
    memset(room, 0, sizeof room);
    snprintf(room, sizeof room, "%s", name);
    session = (plat_ms() * 2654435761u) ^ (uint32_t)rand() ^ 0x5A5A0000u;
    role = NET_HOST;
    phase = NET_H_WAITING;
    have_peer = 0;
    delay = getenv("TJE_NET_DELAY") ? atoi(getenv("TJE_NET_DELAY")) : NET_DELAY;
    if (delay < 1) delay = 1;
    if (delay > 30) delay = 30;
    logf_("room \"%s\" open", room);
    return 0;
}

int net_scan(NetRoom *out, int max)
{
    uint32_t now = plat_ms();
    int i, k = 0;
    if (role != NET_NONE) return 0;
    if (!opened()) return 0;
    if (!discover_ms || now - discover_ms > 1000) {
        Out o;
        uint32_t keep = session;
        session = 0;
        head(&o, P_DISCOVER);
        session = keep;
        plat_net_broadcast(o.b, o.n);
        discover_ms = now ? now : 1;
    }
    net_poll();
    for (i = 0; i < nrooms; i++)
        if (now - rooms[i].seen > 3500) { rooms[i--] = rooms[--nrooms]; }
    for (i = 0; i < nrooms && k < max; i++)
        if (rooms[i].state != NET_ROOM_FULL) out[k++] = rooms[i];
    return k;
}

int net_join(const NetRoom *r)
{
    if (!r->compatible) {
        snprintf(message, sizeof message, "That room has a different version of the game.");
        return -1;
    }
    if (!opened()) return -1;
    role = NET_JOIN;
    phase = NET_J_CONNECTING;
    peer = r->addr;
    have_peer = 1;
    memcpy(room, r->name, sizeof room);
    session = 0;
    joined_ms = plat_ms();
    heard_ms = joined_ms;
    kept_ms = 0;
    nrooms = 0;
    discover_ms = 0;
    pkg_clear();
    return 0;
}

/* ---- the game ------------------------------------------------------------------- */

/* Before the port's input handling: in a network game this machine's controllers
 * (any of them) are its player's. */
static void pads_before(uint16_t pad[2], uint16_t *menu)
{
    int me;
    (void)menu;
    net_poll();
    if (!net_in_game()) return;
    me = net_local_player();
    pad[me] = (uint16_t)(pad[0] | pad[1]);
    pad[!me] = 0;
}

static void show_wait(uint32_t waited)
{
    char text[96];
    if (waited < 400 || g_fast_forward) return;
    if (role == NET_HOST && phase == NET_H_LOST)
        snprintf(text, sizeof text, "earl left - waiting for earl to come back. back - quit");
    else if (role == NET_HOST && pkg.active)
        snprintf(text, sizeof text, "earl is joining... %d%%", net_progress());
    else
        snprintf(text, sizeof text, role == NET_HOST ? "waiting for earl..." : "waiting for toejam...");
    gameview_set_message(text);
}

/* After it: this frame's inputs are sampled and sent, and the frame's inputs of
 * both players (from `delay` frames ago) are what the game sees. */
static unsigned idle_frames;   /* (TJE_NET_LOG) frames shown again while waiting */

static void pads_after(uint16_t pad[2], uint16_t *menu)
{
    int me;
    uint32_t f, t0, k;
    if (!net_in_game()) return;
    me = net_local_player();
    f = nf + 1;
    if (!have(me, f + (uint32_t)delay)) {
        put(me, f + (uint32_t)delay, pack(pad[me], *menu));
        if (f + (uint32_t)delay > my_last) my_last = f + (uint32_t)delay;
    }
    if (f % HASH_EVERY == 0) {
        k = (f / HASH_EVERY) & 63;
        hash_frame[k] = f;
        hash_val[k] = ram_hash();
        if (getenv("TJE_NET_HASHLOG")) {
            static FILE *hf;
            if (!hf) hf = fopen(getenv("TJE_NET_HASHLOG"), "w");
            if (hf) { fprintf(hf, "%u %08X\n", f, hash_val[k]); fflush(hf); }
        }
    }
    send_inputs();
    kept_ms = plat_ms();
    t0 = kept_ms;
    in_stall = 1;
    while (!have(0, f) || !have(1, f)) {
        uint16_t m;
        net_poll();                               /* (a joiner may restart here) */
        if (!net_in_game()) {                     /* the session ended */
            in_stall = 0;
            gameview_set_message(NULL);
            save_quit_to_menu();                  /* (does not return) */
            return;
        }
        /* (usually the other machine is only a moment late: wait for its inputs
         * without showing a frame again, for a frame's time) */
        if (plat_ms() - t0 < 17) {
            plat_sleep_ms(1);
            continue;
        }
        show_wait(plat_ms() - t0);
        m = genesis_idle_frame();
        idle_frames++;
        if (role == NET_HOST && phase == NET_H_LOST && (m & MENU_BACK)) {
            net_leave(NULL);
            in_stall = 0;
            save_quit_to_menu();
            return;
        }
    }
    in_stall = 0;
    gameview_set_message(NULL);
    {
        /* (TJE_NET_LOG: how smooth it is) */
        static unsigned waits, waited_ms, ffs;
        uint32_t w = plat_ms() - t0;
        if (w > 1) { waits++; waited_ms += w; }
        if (g_fast_forward) ffs++;
        if (f % 300 == 0) {
            logf_("frames %u-%u: waited %u times (%u ms), %u frames shown twice, %u not shown; the other is %d frames ahead",
                  f - 299, f, waits, waited_ms, idle_frames, ffs, (int)(peer_contig - (uint32_t)delay) - (int)f);
            waits = waited_ms = ffs = idle_frames = 0;
        }
    }
    pad[0] = unpack_pad(get(0, f));
    pad[1] = unpack_pad(get(1, f));
    *menu = (uint16_t)(unpack_menu(get(0, f)) | unpack_menu(get(1, f)));
    nf = f;
    /* Keeping step with the other machine. Each sees the other's frame a little
     * late (the inputs take a moment to arrive), so each also sends how far ahead
     * it sees the other: half the difference of the two views is how far ahead
     * the other really is, the delay cancelling out. A machine ahead makes its
     * frames up to 2 ms longer, one behind up to 2 ms shorter, until they are in
     * step: nothing shown twice or left out. Far behind (a slow moment, or just
     * restarted from a snapshot): frames run without being shown. */
    {
        int ahead = (int)(peer_contig - (uint32_t)delay - nf), off16, adj = 0;
        seen_ahead16 += (ahead * 16 - seen_ahead16) / 8;
        off16 = (seen_ahead16 - their_seen_ahead16) / 2;
        if (off16 > 4 || off16 < -4) adj = -off16 * 2000 / 16;
        if (adj > 2000) adj = 2000;
        if (adj < -2000) adj = -2000;
        plat_pacing(1, adj);
        g_fast_forward = have(0, nf + 1) && have(1, nf + 1) && (ahead >= 4 || (g_fast_forward && ahead >= 2));
    }
}

void net_install(void)
{
    genesis_set_pad_hooks(pads_before, pads_after);
}

void net_loop_top(void)
{
    if (role != NET_HOST) return;
    if (phase == NET_H_JOINED || phase == NET_H_WAITING) {
        /* the game starts: everything from here is the same on both machines
         * (the other player gone already: the game waits for them) */
        uint32_t f;
        reset_lockstep();
        windows_on();
        for (f = 1; f <= (uint32_t)delay; f++) { put(0, f, 0); put(1, f, 0); }
        my_last = (uint32_t)delay;
        peer_contig = (uint32_t)delay;
        phase = have_peer ? NET_H_PLAYING : NET_H_LOST;
        snap_ok = 0;
    } else if (phase != NET_H_PLAYING && phase != NET_H_LOST) {
        return;
    }
    if (!snap_ok || nf - snap_frame >= SNAP_EVERY) {
        size_t n = genesis_snapshot(NULL, 0);
        if (n > snap_cap) {
            uint8_t *b = (uint8_t *)realloc(snap, n);
            if (!b) return;
            snap = b;
            snap_cap = n;
        }
        if (genesis_snapshot(snap, snap_cap) != n) return;
        snap_len = n;
        snap_frame = nf;
        snap_pad[0] = g_pad[0];
        snap_pad[1] = g_pad[1];
        snap_menu = g_menu_pad;
        if (!snap_ok) {
            snap_ok = 1;
            if (nf == 0 && have_peer) make_package(K_START, (uint32_t)delay);
        }
    }
}

/* the machine has restarted from the package: the inputs it had then */
void net_resumed(void)
{
    g_pad[0] = resume_pad[0];
    g_pad[1] = resume_pad[1];
    g_menu_pad = resume_menu;
    windows_on();
}
