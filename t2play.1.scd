t2play(1)

# NAME

t2play - lightweight panel

# DESCRIPTION

t2play is a simple, lightweight panel for Wayland compositors which support the
wlr-layer-shell and wlr-foreign-toplevel-management protocols.

Features:

- High customizability via a plain text configuration file, including options
  for colors of fonts, icons, borders and backgrounds.
- Builtin plugins for capabilities like taskbar, clock, battery and keyboard
  layout.

# SYNOPSIS

*t2play [OPTION...]*

# OPTIONS

*-c|--config <filename>*
	Specify config file
*-d|--debug*
	Enable full logging, including debug information
*-h|--help*
	Show help message and quit
*-o|--output <output>*
	Specify output (monitor)
*-V|--verbose*
	Enable more verbose logging

# CONFIGURATION

## Data types

*color*
	Hexadecimal RGB/RGBA value using syntax *#rrggbb* or *#rrggbbaa*

## Panel

*panel_items: string*
	Which plugins to be shown and their order (from left to right). Each
	letter refers to an item as defined below. Default is *STBKC*.
	- *S* Startmenu
	- *T* Taskbar
	- *B* Battery
	- *K* Keyboard layout
	- *C* Clock

*panel_breadth: integer*

## Taskbar

A *task* is the graphical artefact representing one application within the
taskbar.

*taskbar_padding: <integer>*++
*taskbar_spacing: <integer>*
	Taskbar padding and spacing of tasks within it. Default is 0
	for all.

	```
	taskbar
	╔═════════════════════════╗
	║      1           1      ║
	║   ┌─────┐     ┌─────┐   ║
	║ 1 │task │  2  │task │ 1 ║
	║   └─────┘     └─────┘   ║
	║      1           1      ║
	╚═════════════════════════╝
	1. padding
	2. spacing
	```

*task_padding: <integer>*
	The space between the edges of the task and the content within it

*task_background_color = <color>*
	Background color for non-selected tasks

*task_active_background_color = <color>*
	Background color for selected tasks

## Startmenu


