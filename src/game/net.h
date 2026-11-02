/*
 * Network play (port feature): two copies of the game, ToeJam on the host, Earl
 * on the one that joins. docs/NETWORK.md has the whole design.
 *
 * Both machines run the same game, frame for frame, from the same starting
 * snapshot, on the same controller inputs (lockstep); each shows only its own
 * player's window. Only inputs travel during play (a few bytes a frame, each sent
 * several times, so a lost packet costs nothing). Memory checksums are compared;
 * a difference, or a player joining a game in progress, is mended by the host
 * sending a snapshot of the machine and the inputs since.
 */
#ifndef TJE_NET_H
#define TJE_NET_H

#include <stdint.h>
#include "platform/platform.h"

#define NET_NAME_MAX 16

enum { NET_NONE, NET_HOST, NET_JOIN };              /* net_role() */

/* Host phases (net_phase()) */
enum {
    NET_H_WAITING,      /* room open, nobody in it */
    NET_H_JOINED,       /* a player is in the room; the host is choosing the game */
    NET_H_PLAYING,      /* in the game */
    NET_H_LOST          /* in the game, the other player gone: waiting for them */
};
/* Joiner phases */
enum {
    NET_J_CONNECTING,   /* asked to join */
    NET_J_WAITING,      /* in the room, the host is choosing the game */
    NET_J_RECEIVING,    /* the game is coming (net_progress()) */
    NET_J_PLAYING
};

/* Rooms found on the local network (net_scan) */
enum { NET_ROOM_OPEN, NET_ROOM_REJOIN, NET_ROOM_FULL };
typedef struct {
    char name[NET_NAME_MAX + 1];
    int state;
    int compatible;     /* same game version and ROM */
    PlatNetAddr addr;
    uint32_t seen, session;
} NetRoom;

int net_role(void);
int net_phase(void);
/* The player this machine controls (0 ToeJam, 1 Earl); -1 outside network play. */
int net_local_player(void);
/* In a network game (inputs in lockstep). */
int net_in_game(void);
/* Percentage of the game snapshot received (joiner, NET_J_RECEIVING). */
int net_progress(void);
const char *net_room_name(void);

/* Host: open a room. 0 = ok. */
int net_host(const char *name);
/* Joiner: look for rooms (call every frame while the list is shown); returns how
 * many are in rooms[]. */
int net_scan(NetRoom *rooms, int max);
int net_join(const NetRoom *room);
/* Leave the room or game (tells the other side); `why` is kept for
 * net_take_message() (NULL: no message). */
void net_leave(const char *why);
/* A message about how the last session ended, once. */
int net_take_message(char *out, int size);

/* Hooks (installed by net_install): read packets, and the frame's inputs. */
void net_install(void);
void net_poll(void);
/* The top of the main game loop (save.c): sessions start here, snapshots are
 * taken here. */
void net_loop_top(void);
/* Set when the machine was restarted from a network snapshot: the main loop
 * goes on as it was (save.c skips its start-up). */
extern int g_net_resume;
void net_resumed(void);

#endif
