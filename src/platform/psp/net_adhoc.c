/*
 * Network play on PSP: ad hoc (platform.h, plat_net_*).
 *
 * Every copy of the game joins one ad hoc group ("TJEARL01": the first there
 * makes it) and talks in datagrams (PDP) on one port; an address is a PSP's MAC
 * and the port. Broadcasts go to FF:FF:FF:FF:FF:FF. Rooms, joining and play are
 * the game's own protocol on top (src/game/net.c), as over UDP on PC.
 */
#include "platform/platform.h"

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <stdio.h>
#include <string.h>

#define NET_PORT  0x1E52
#define GROUP     "TJEARL01"

static int pdp = -1, modules, inited, connecting;
static unsigned char my_mac[6];

static void addr_make(PlatNetAddr *a, const unsigned char *mac, unsigned short port)
{
    memset(a, 0, sizeof *a);
    memcpy(a->b, mac, 6);
    a->b[6] = (uint8_t)(port >> 8);
    a->b[7] = (uint8_t)port;
}

static int open_adhoc(void)
{
    struct productStruct product;
    if (pdp >= 0 || connecting) return 0;
    if (!modules) {
        if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0) return -1;
        if (sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC) < 0) return -1;
        modules = 1;
    }
    if (!inited) {
        if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) < 0) return -2;
        if (sceNetAdhocInit() < 0) { sceNetTerm(); return -3; }
        memset(&product, 0, sizeof product);
        strcpy(product.product, "ULUS99999");
        if (sceNetAdhocctlInit(32 * 1024, 0x20, &product) < 0) { sceNetAdhocTerm(); sceNetTerm(); return -4; }
        inited = 1;
    }
    /* the group: join it (made if nobody is in it yet). That takes a moment: the
     * menus carry on meanwhile (ready() finishes it), nothing is sent or received
     * before. */
    if (sceNetAdhocctlConnect(GROUP) < 0) return -5;
    connecting = 1;
    return 0;
}

static unsigned send_fails;

static int ready(void)
{
    int state = 0;
    if (pdp >= 0) {
        /* (the group can drop - out of range, interference: a few times a second
         * it is looked at, and if it has gone it is joined again; the game's own
         * protocol then finds the other machine again) */
        static uint32_t looked;
        uint32_t now = sceKernelGetSystemTimeLow();
        if (now - looked < 250000) return 1;
        looked = now;
        if (sceNetAdhocctlGetState(&state) >= 0 && state == 1) return 1;
        fprintf(stderr, "net: ad hoc group lost (state %d, %u sends failed): joining it again\n", state, send_fails);
        sceNetAdhocPdpDelete(pdp, 0);
        pdp = -1;
        sceNetAdhocctlDisconnect();
        if (sceNetAdhocctlConnect(GROUP) >= 0) connecting = 1;
        return 0;
    }
    if (!connecting || sceNetAdhocctlGetState(&state) < 0 || state != 1) return 0;
    connecting = 0;
    if (send_fails) fprintf(stderr, "net: ad hoc group joined again\n");
    send_fails = 0;
    sceNetGetLocalEtherAddr(my_mac);
    pdp = sceNetAdhocPdpCreate(my_mac, NET_PORT, 0x4000, 0);
    if (pdp < 0) {
        printf("net: ad hoc port not made (%d)\n", pdp);
        pdp = -1;
        sceNetAdhocctlDisconnect();
        return 0;
    }
    return 1;
}

int plat_net_open(void)
{
    int r = open_adhoc();
    if (r < 0) printf("net: ad hoc not started (%d)\n", r);
    return r;
}

void plat_net_close(void)
{
    if (pdp >= 0) sceNetAdhocPdpDelete(pdp, 0);
    if (pdp >= 0 || connecting) sceNetAdhocctlDisconnect();
    pdp = -1;
    connecting = 0;
}

int plat_net_send(const PlatNetAddr *to, const void *data, int len)
{
    unsigned char mac[6];
    if (!ready()) return -1;
    memcpy(mac, to->b, 6);
    {
        int r = sceNetAdhocPdpSend(pdp, mac, (unsigned short)(to->b[6] << 8 | to->b[7]), (void *)data, (unsigned)len, 0, 1);
        if (r < 0) {
            if (!send_fails++) fprintf(stderr, "net: a send failed (%08X)\n", (unsigned)r);
            return -1;
        }
        return 0;
    }
}

int plat_net_broadcast(const void *data, int len)
{
    static unsigned char all[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (!ready()) return -1;
    return sceNetAdhocPdpSend(pdp, all, NET_PORT, (void *)data, (unsigned)len, 0, 1) >= 0 ? 0 : -1;
}

int plat_net_recv(PlatNetAddr *from, void *buf, int cap)
{
    unsigned char mac[6];
    unsigned short port = 0;
    int len = cap;
    if (!ready()) return 0;
    if (sceNetAdhocPdpRecv(pdp, mac, &port, buf, &len, 0, 1) < 0 || len <= 0) return 0;
    if (!memcmp(mac, my_mac, 6)) return 0;   /* (our own broadcast) */
    addr_make(from, mac, port);
    return len;
}

int plat_net_addr_equal(const PlatNetAddr *a, const PlatNetAddr *b) { return !memcmp(a->b, b->b, 8); }

void plat_net_addr_text(const PlatNetAddr *a, char *out, int size)
{
    snprintf(out, (size_t)size, "%02X:%02X:%02X:%02X:%02X:%02X", a->b[0], a->b[1], a->b[2], a->b[3], a->b[4], a->b[5]);
}
