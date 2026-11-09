# l-term

An incredibly fast Wayland terminal built in C.

Why its in 1 file? Because im too lazy for splitting and structuring it.

Should 100% work on arch based distros. And probably wont work anywhere besides linux+wayland.

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
