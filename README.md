# l-term

An incredibly fast Wayland terminal emulator built in C.

I made it for myself so there are a lot of unchangable things, but i think u can try to simply give this code to ai and ask it to change smth u need.

Why its in 1 file? Because im too lazy for splitting and structuring it.

Is there any point in moving from alacritty to this? Kinda, first of i dont like toml configs, here used .json one, second it uses much less ram, for example: alacritty on my instance uses 130-200mb of ram, while l-term only 30mb.

Whats that binary? Its for x86 architecture, anyway this term takes around 1 sec to build so :shrug:.

Will it work on mac or anything besides linux+wayland? Probably no, i dont have mac to test it, idc about X11 users :kissing:.

Where did i test it and where it ll 100% work totally fine? Artix, but ig any Arch based system ll be fine 100%. I guess l-term ll work on 90% of linux distros.

Can i ask for some new features? Nah, if u want smth, do it by urself :smile:.

What are the features? Ehh, .json config in ur ~/.config/l/ folder, there s search on alt+/ aaand its very fast. Yea thats kinda it :p

## Screenshots

<img width="1343" height="1074" alt="image" src="https://github.com/user-attachments/assets/c3cad495-d13a-4764-849c-0985d52903ef" />

Idk what else u expected to see here, its a terminal :)

## Dependencies

- wayland
- wayland-protocols
- wayland-cursor
- libxkbcommon
- cairo
- pango
- libvterm
- jsmn (header-only)

## Installation

```bash
git clone https://github.com/mc0vafla/l.git
cd l
make
