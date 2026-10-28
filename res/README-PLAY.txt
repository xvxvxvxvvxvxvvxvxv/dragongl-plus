DRAGONGL+ - THE LONG DESCENT INTO THE DRAGON'S MAW
https://github.com/xvxvxvxvvxvxvvxvxv/dragongl-plus
A fork of Dragon GL, (C) 2026 Nicola Taibi - GPL-3.0-or-later - https://github.com/nicolataibi/dragongl

=============================================================================
 ENGLISH
=============================================================================

HOW TO START
  Windows : double-click  DragonGL-Plus.exe
  Linux   : double-click  DragonGL-Plus  (or run ./DragonGL-Plus from a terminal)

Nothing needs to be installed: the game, its music and its fonts are all
inside the executable. You only need a graphics driver with OpenGL 3.3 or
Vulkan 1.3 (any PC from the last ~12 years).

PLAY SOLO
  Title screen -> "Play Solo" -> create a hero -> "Begin the Descent".
  The game starts a private world on your computer automatically.
  Your heroes and worlds are saved in:
    Windows : %APPDATA%\DragonGL\solo
    Linux   : ~/.config/dragongl/solo

PLAY WITH FRIENDS
  One player runs the server:
    Windows : dragongl_server.exe -P 8080 -p <server password>
    Linux   : ./dragongl_server -P 8080 -p <server password>
  (open TCP port 8080 on the firewall/router). Everyone else chooses
  "Join a Server" and enters the address, port and server password.

CONTROLS (all of them are also listed in game: press F1 or H)
  Left click        move / attack / talk / pick up      Right drag   rotate camera
  WASD / arrows     move (relative to the camera)        Q / E        rotate camera
  Mouse wheel       zoom (Home: reset camera)            Space        attack target
  F                 interact (doors, merchants, stairs)  G            pick up
  I / C / K / M     inventory / character / spells / map
  1-4               belt potions                         5-0          spell hotbar
  Enter             chat & commands                      Tab          next target
  R rest   T search   L look   N show all names          Esc          menu
  F12               screenshot

GRAPHICS
  The game picks OpenGL by default; Vulkan can be selected in
  Settings -> Graphics (or start with:  DragonGL-Plus vk ). If a renderer is
  not available the game falls back to the other one automatically.

=============================================================================
 ITALIANO
=============================================================================

COME AVVIARE
  Windows : doppio clic su  DragonGL-Plus.exe
  Linux   : doppio clic su  DragonGL-Plus  (oppure ./DragonGL-Plus da terminale)

Non serve installare nulla: gioco, musiche e font sono dentro l'eseguibile.
Basta un driver grafico con OpenGL 3.3 o Vulkan 1.3.

GIOCARE IN SOLITARIO
  Schermata iniziale -> "Play Solo" -> crea un eroe -> "Begin the Descent".
  Il gioco avvia da solo un mondo privato sul tuo computer. Salvataggi in:
    Windows : %APPDATA%\DragonGL\solo
    Linux   : ~/.config/dragongl/solo

GIOCARE CON GLI AMICI
  Un giocatore avvia il server:
    Windows : dragongl_server.exe -P 8080 -p <password del server>
    Linux   : ./dragongl_server -P 8080 -p <password del server>
  (aprire la porta TCP 8080). Gli altri scelgono "Join a Server" e
  inseriscono indirizzo, porta e password del server.

COMANDI: vedi la sezione inglese qui sopra, oppure premi F1 / H nel gioco.

=============================================================================
 THIRD-PARTY COMPONENTS
=============================================================================
  See the licenses/ folder: GLFW (zlib), volk (MIT), Vulkan headers
  (Apache-2.0/MIT), miniaudio (MIT-0), stb (public domain), wepoll
  (BSD-2), Cinzel and Alegreya Sans fonts (SIL OFL 1.1).
