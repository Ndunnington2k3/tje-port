/* Network Play on the Who screen (lan_menu.c). */
#ifndef TJE_LAN_MENU_H
#define TJE_LAN_MENU_H

/* Returns 0 when this machine hosts a network game about to start (both players),
 * -1 to go back to the Who screen. A machine that joins a game does not return. */
int lan_menu(void);
/* A message with OK. */
void lan_message(const char *text);
/* The saved-games list (menus.c); two_players: only two-player games can be
 * chosen. Loading does not return. */
void load_game_menu(int two_players);

#endif
