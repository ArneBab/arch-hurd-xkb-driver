/* pc-kbd.c - The PC Keyboard input driver.
   Copyright (C) 2002, 2004, 2005 Free Software Foundation, Inc.
   Written by Marcus Brinkmann.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include <errno.h>
#include <assert.h>
#include <string.h>
#include <iconv.h>
#include <sys/mman.h>
#include <argp.h>

#include <device/device.h>
#include <cthreads.h>

#include <hurd/console.h>
#include <hurd/cons.h>

#include "driver.h"
#include "mach-inputdev.h"


/* The default name of the node of the repeater.  */
#define DEFAULT_REPEATER_NODE	"kbd"

/* The keyboard device in the kernel.  */
static device_t kbd_dev;

/* The converter.  */
static iconv_t cd;

/* The status of various LEDs.  */
struct {
  int scroll_lock : 1;
  int num_lock : 1;
  int caps_lock : 1;
} led_state;

/* Forward declaration.  */
static struct input_ops pc_kbd_ops;

/* The name of the repeater node.  */
static char *repeater_node;

/* The repeater node.  */
static consnode_t cnode;

/* A list of scan codes generated by the keyboard, in the set 2 encoding.  */
enum scancode
  {
    SC_F9            = 0x01,
    SC_F5            = 0x03,
    SC_F3            = 0x04,
    SC_F1            = 0x05,
    SC_F2            = 0x06,
    SC_F12           = 0x07,
    SC_F10           = 0x09,
    SC_F8            = 0x0A,
    SC_F6            = 0x0B,
    SC_F4            = 0x0C,
    SC_TAB           = 0x0D,
    SC_BACKQUOTE     = 0x0E,	/* ` */
    SC_LEFT_ALT      = 0x11,
    SC_LEFT_SHIFT    = 0x12,
    SC_LEFT_CTRL     = 0x14,
    SC_Q             = 0x15,
    SC_1             = 0x16,
    SC_Z             = 0x1A,
    SC_S             = 0x1B,
    SC_A             = 0x1C,
    SC_W             = 0x1D,
    SC_2             = 0x1E,
    SC_C             = 0x21,
    SC_X             = 0x22,
    SC_D             = 0x23,
    SC_E             = 0x24,
    SC_4             = 0x25,
    SC_3             = 0x26,
    SC_SPACE         = 0x29,
    SC_V             = 0x2A,
    SC_F             = 0x2B,
    SC_T             = 0x2C,
    SC_R             = 0x2D,
    SC_5             = 0x2E,
    SC_N             = 0x31,
    SC_B             = 0x32,
    SC_H             = 0x33,
    SC_G             = 0x34,
    SC_Y             = 0x35,
    SC_6             = 0x36,
    SC_M             = 0x3A,
    SC_J             = 0x3B,
    SC_U             = 0x3C,
    SC_7             = 0x3D,
    SC_8             = 0x3E,
    SC_COMMA         = 0x41,	/* , */
    SC_K             = 0x42,
    SC_I             = 0x43,
    SC_O             = 0x44,
    SC_0             = 0x45,
    SC_9             = 0x46,
    SC_PERIOD        = 0x49,	/* . */
    SC_SLASH         = 0x4A,	/* / */
    SC_L             = 0x4B,
    SC_SEMICOLON     = 0x4C,	/* ; */
    SC_P             = 0x4D,
    SC_MINUS         = 0x4E,	/* - */
    SC_APOSTROPHE    = 0x52,	/* ' */
    SC_LEFT_BRACKET  = 0x54,	/* [ */
    SC_EQUAL         = 0x55,	/* = */
    SC_CAPSLOCK      = 0x58,
    SC_RIGHT_SHIFT   = 0x59,
    SC_ENTER         = 0x5A,
    SC_RIGHT_BRACKET = 0x5B,	/* ] */
    SC_BACKSLASH     = 0x5D,	/* \ */
    SC_BACKSPACE     = 0x66,
    SC_PAD_1         = 0x69,
    SC_PAD_4         = 0x6B,
    SC_PAD_7         = 0x6C,
    SC_PAD_0         = 0x70,
    SC_PAD_DECIMAL   = 0x71,
    SC_PAD_2         = 0x72,
    SC_PAD_5         = 0x73,
    SC_PAD_6         = 0x74,
    SC_PAD_8         = 0x75,
    SC_ESC           = 0x76,
    SC_NUMLOCK       = 0x77,
    SC_F11           = 0x78,
    SC_PAD_PLUS      = 0x79,
    SC_PAD_3         = 0x7A,
    SC_PAD_MINUS     = 0x7B,
    SC_PAD_ASTERISK  = 0x7C,
    SC_PAD_9         = 0x7D,
    SC_SCROLLLOCK    = 0x7E,
    SC_F7            = 0x83,
    SC_EXTENDED1     = 0xE0,	/* One code follows.  */
    SC_EXTENDED2     = 0xE1,	/* Two codes follow (only used for Pause).  */
    SC_ERROR         = 0xFF,	/* Too many keys held down.  */
    SC_FLAG_UP       = 0xF000	/* ORed to basic scancode.  */
  };

/* In set 2 function keys don't have a logical order.  This macro can
   determine if a function key was pressed.  */
#define IS_FUNC_KEY(c) ((sc >= SC_F9 && sc <= SC_F4) ||    \
			sc == SC_F7 || sc == SC_F11)

/* Codes which can follow SC_EXTENDED1.  */
enum scancode_x1
  {
    SC_X1_RIGHT_ALT  = 0x11,
    SC_X1_PRTSC      = 0x12,
/*    SC_X1_PRTSC      = 0x7C,  */
    SC_X1_RIGHT_CTRL = 0x14,
    SC_X1_LEFT_GUI   = 0x1F,
    SC_X1_RIGHT_GUI  = 0x27,
    SC_X1_APPS       = 0x2F,
    SC_X1_POWER      = 0x37,
    SC_X1_SLEEP      = 0x3F,
    SC_X1_PAD_SLASH  = 0x4A,
    SC_X1_PAD_ENTER  = 0x5A,
    SC_X1_WAKEUP     = 0x5E,
    SC_X1_END        = 0x69,
    SC_X1_LEFT       = 0x6B,
    SC_X1_HOME       = 0x6C,
    SC_X1_INS        = 0x70,
    SC_X1_DEL        = 0x71,
    SC_X1_DOWN       = 0x72,
    SC_X1_RIGHT      = 0x74,
    SC_X1_UP         = 0x75,
    SC_X1_PGDN       = 0x7A,
    SC_X1_PGUP       = 0x7D
  };

/* Codes which can follow SC_EXTENDED2.  */
enum scancode_x2
  {
    SC_X2_BREAK      = 0x1477,
  };


/* Scancode to Unicode mapping.  The empty string stands for the NULL
   character.  */
char *sc_to_kc[][7] =
  {
    /*None, Shift,    Ctrl,   LAlt, S+LAlt,  C+LAlt, RAlt  */
    {    0,    0,       0,      0,      0,         0,     0 },
    { CONS_KEY_F9,  0,      0,      0,  0,         0,     0 }, /* SC_F9.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    { CONS_KEY_F5, CONS_KEY_F17, 0, 0,  0,         0,     0 }, /* SC_F5.  */
    { CONS_KEY_F3, CONS_KEY_F15, 0, 0,  0,         0,     0 }, /* SC_F3.  */
    { CONS_KEY_F1, CONS_KEY_F13, 0, 0,  0,         0,     0 }, /* SC_F1.  */
    { CONS_KEY_F2, CONS_KEY_F14, 0, 0,  0,         0,     0 }, /* SC_F2.  */
    { CONS_KEY_F12, 0,      0,      0,  0,         0,     0 }, /* SC_F12.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    { CONS_KEY_F10, 0,      0,      0,  0,         0,     0 }, /* SC_F10.  */
    { CONS_KEY_F8, CONS_KEY_F20, 0, 0,  0,         0,     0 }, /* SC_F8.  */
    { CONS_KEY_F6, CONS_KEY_F18, 0, 0,  0,         0,     0 }, /* SC_F6.  */
    { CONS_KEY_F4, CONS_KEY_F16, 0, 0,  0,         0,     0 }, /* SC_F4.  */
    { "\t", "\t",    "\t", "\e\t", "\e\t",    "\e\t",  "\t" }, /* SC_TAB.  */
    {  "`",  "~",       0,  "\e`",  "\e~",         0,     0 }, /* SC_BACKQUOTE.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 }, /* SC_LEFT_ALT.  XXX */
    {    0,    0,       0,      0,      0,         0,     0 }, /* SC_LEFT_SHIFT.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 }, /* SC_LEFT_CTRL.  XXX */
    {  "q",  "Q",  "\x11",  "\eq",  "\eQ",  "\e\x11",   "q" }, /* SC_Q.  */
    {  "1",  "!",       0,  "\e1",  "\e!",         0,   "1" }, /* SC_1.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {  "z",  "Z",  "\x1a",  "\ez",  "\eZ",  "\e\x1a",   "z" }, /* SC_Z.  */
    {  "s",  "S",  "\x13",  "\es",  "\eS",  "\e\x13",   "s" }, /* SC_S.  */
    {  "a",  "A",  "\x01",  "\ea",  "\eA",  "\e\x01",   "a" }, /* SC_A.  */
    {  "w",  "W",  "\x17",  "\ew",  "\eW",  "\e\x17",   "w" }, /* SC_W.  */
    {  "2",  "@",      "",  "\e2",  "\e@",         0,   "2" }, /* SC_2.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {  "c",  "C",  "\x03",  "\ec",  "\eC",  "\e\x03",  "\xc2\xa2" }, /* SC_C.  */
    {  "x",  "X",  "\x18",  "\ex",  "\eX",  "\e\x18",   "x" }, /* SC_X.  */
    {  "d",  "D",  "\x04",  "\ed",  "\eD",  "\e\x04",   "d" }, /* SC_D.  */
    {  "e",  "E",  "\x05",  "\ee",  "\eE",  "\e\x05","\xe2\x82\xac" }, /* SC_E.  */
    {  "4",  "$",  "\x1c",  "\e4",  "\e$",  "\e\x1c",   "4" }, /* SC_4.  */
    {  "3",  "#",    "\e",  "\e3",  "\e#",         0,   "3" }, /* SC_3.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {  " ",  " ",      "",  "\e ",  "\e ",  /*XXX*/0,   " " }, /* SC_SPACE.  */
    {  "v",  "V",  "\x16",  "\ev",  "\eV",  "\e\x16",   "v" }, /* SC_V.  */
    {  "f",  "F",  "\x06",  "\ef",  "\eF",  "\e\x06",   "f" }, /* SC_F.  */
    {  "t",  "T",  "\x14",  "\et",  "\eT",  "\e\x14",   "t" }, /* SC_T.  */
    {  "r",  "R",  "\x12",  "\er",  "\eR",  "\e\x12",   "r" }, /* SC_R.  */
    {  "5",  "%",  "\x1d",  "\e5",  "\e%",         0,   "5" }, /* SC_5.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {  "n",  "N",  "\x0e",  "\en",  "\eN",  "\e\x0e",   "n" }, /* SC_N.  */
    {  "b",  "B",  "\x02",  "\eb",  "\eB",  "\e\x02",   "b" }, /* SC_B.  */
    {  "h",  "H",  "\x08",  "\eh",  "\eH",  "\e\x08",   "h" }, /* SC_H.  */
    {  "g",  "G",  "\x07",  "\eg",  "\eG",  "\e\x07",   "g" }, /* SC_G.  */
    {  "y",  "Y",  "\x19",  "\ey",  "\eY",  "\e\x19",   "y" }, /* SC_Y.  */
    {  "6",  "^",  "\x1e",  "\e6",  "\e^",         0,   "6" }, /* SC_6.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {  "m",  "M",  "\x0d",  "\em",  "\eM",  "\e\x0d",   "m" }, /* SC_M.  */
    {  "j",  "J",  "\x0a",  "\ej",  "\eJ",  "\e\x0a",   "j" }, /* SC_J.  */
    {  "u",  "U",  "\x15",  "\eu",  "\eU",  "\e\x15",   "u" }, /* SC_U.  */
    {  "7",  "&",  "\x1f",  "\e7",  "\e&",  "\e\x1f",   "7" }, /* SC_7.  */
    {  "8",  "*",  "\x7f",  "\e8",  "\e*",         0,   "8" }, /* SC_8.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {  ",",  "<",       0,  "\e,",  "\e<",         0,     0 }, /* SC_COMMA.  */
    {  "k",  "K",  "\x0b",  "\ek",  "\eK",  "\e\x0b",   "k" }, /* SC_K.  */
    {  "i",  "I",  "\x09",  "\ei",  "\eI",  "\e\x09",   "i" }, /* SC_I.  */
    {  "o",  "O",  "\x0f",  "\eo",  "\eO",  "\e\x0f",   "o" }, /* SC_O.  */
    {  "0",  ")",       0,  "\e0",  "\e)",         0,   "0" }, /* SC_0.  */
    {  "9",  "(",       0,  "\e9",  "\e(",         0,   "9" }, /* SC_9.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {  ".",  ">",       0,  "\e.",  "\e>",         0,     0 }, /* SC_PERIOD.  */
    {  "/",  "?",  "\x7f",  "\e/",  "\e?",         0,     0 }, /* SC_SLASH.  */
    {  "l",  "L",  "\x0c",  "\el",  "\eL",  "\e\x0c",   "l" }, /* SC_L.  */
    {  ";",  ":",       0,  "\e;",  "\e:",         0,     0 }, /* SC_SEMICOLON.  */
    {  "p",  "P",  "\x10",  "\ep",  "\eP",  "\e\x10",   "p" }, /* SC_P.  */
    {  "-",  "_",  "\x1f",  "\e-",  "\e_",  "\e\x1f",   "-" }, /* SC_MINUS.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {  "'", "\"",  "\x07",  "\e'", "\e\"",         0,     0 }, /* SC_APOSTROPHE.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {  "[",  "{",    "\e",  "\e[",  "\e{",         0,     0 }, /* SC_LEFT_BRACKET.  */
    {  "=",  "+",       0,  "\e=",  "\e+",         0,   "=" }, /* SC_EQUAL.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 }, /* SC_CAPSLOCK.  */
    {    0,    0,       0,      0,      0,         0,     0 }, /* SC_RIGHT_SHIFT.  */
    {"\x0d","\x0d",  "\x0d","\e\x0d","\e\x0d","\e\x0d","\x0d" }, /* SC_ENTER.  */
    {  "]",  "}",  "\x1d",  "\e]",  "\e}",  "\e\x1d",   "~" }, /* SC_RIGHT_BRACKET.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    { "\\",  "|",  "\x1c", "\e\\",  "\e|",         0,     0 }, /* SC_BACKSLASH.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    { CONS_KEY_BACKSPACE, CONS_KEY_BACKSPACE, CONS_KEY_BACKSPACE,
      "\e" CONS_KEY_BACKSPACE, "\e" CONS_KEY_BACKSPACE,
      "\e" CONS_KEY_BACKSPACE, CONS_KEY_BACKSPACE }, /* SC_BACKSPACE.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    { CONS_KEY_END,   CONS_KEY_END,   CONS_KEY_END,  0, 0, 0, 0 }, /* SC_PAD_1.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    { CONS_KEY_LEFT,  CONS_KEY_LEFT,  CONS_KEY_LEFT, 0, 0, 0, 0 }, /* SC_PAD_4.  */
    { CONS_KEY_HOME,  CONS_KEY_HOME,  CONS_KEY_HOME, 0, 0, 0, 0 }, /* SC_PAD_7.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    { CONS_KEY_IC,    CONS_KEY_IC,    CONS_KEY_IC,   0, 0, 0, 0 }, /* SC_PAD_0.  */
    { CONS_KEY_DC,    CONS_KEY_DC,    CONS_KEY_DC,   0, 0, 0, 0 }, /* SC_PAD_DECIMAL.  */
    { CONS_KEY_DOWN,  CONS_KEY_DOWN,  CONS_KEY_DOWN, 0, 0, 0, 0 }, /* SC_PAD_2.  */
    {/* XXX */ "\e[G",      "\e[G",         "\e[G", 0, 0, 0, 0 }, /* SC_PAD_5.  */
    { CONS_KEY_RIGHT, CONS_KEY_RIGHT, CONS_KEY_RIGHT,0, 0, 0, 0 }, /* SC_PAD_6.  */
    { CONS_KEY_UP,    CONS_KEY_UP,    CONS_KEY_UP,   0, 0, 0, 0 }, /* SC_PAD_8.  */
    { "\e", "\e",    "\e", "\e\e", "\e\e",    "\e\e",  "\e" }, /* SC_ESC.  */
    { 0,   0,       0,      0,      0,  0,     0 }, /* SC_NUMLOCK.  */
    { CONS_KEY_F11, 0,      0,      0,      0,         0,     0 }, /* SC_F11.  */
    {  "+",    "+",      "+",   "+",    "+",       "+",   "+" }, /* SC_PAD_PLUS.  */
    { CONS_KEY_NPAGE, CONS_KEY_NPAGE, CONS_KEY_NPAGE,0, 0, 0, 0 }, /* SC_PAD_3.  */
    {  "-",    "-",      "-",   "-",    "-",       "-",   "-" }, /* SC_PAD_MINUS.  */
    {  "*",  "*",     "*",    "*",    "*",       "*",   "*" }, /* SC_PAD_ASTERISK.  XXX */
    { CONS_KEY_PPAGE, CONS_KEY_PPAGE, CONS_KEY_PPAGE,0, 0, 0, 0 }, /* SC_PAD_9.  */
    { 0,   0,       0,      0,      0,  0,     0 }, /* SC_SCROLLLOCK.  */
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    {    0,    0,       0,      0,      0,         0,     0 },
    { CONS_KEY_F7, CONS_KEY_F19, 0, 0,  0,         0,     0 }  /* SC_F7.  */
  };

char *sc_x1_to_kc[][7] =
  {
    /*  None,  Shift,    Ctrl,   LAlt, S+LAlt,  C+LAlt, RAlt  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_RIGHT_ALT.  */
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_PRTSC.  */
    { 0, 0, 0, 0, 0, 0, 0 },
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_RIGHT_CTRL.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_LEFT_GUI.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_RIGHT_GUI.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_APPS.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_POWER.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_SLEEP.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    {    "/",    "/",     "/",    "/",    "/",       "/",     0 }, /* SC_X1_PAD_SLASH.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
    {   "\n",   "\n",    "\n",   "\n",   "\n",      "\n",     0 }, /* SC_X1_PAD_ENTER.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
    {      0,      0,       0,      0,      0,         0,     0 }, /* SC_X1_WAKEUP.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { CONS_KEY_END,   CONS_KEY_END,   CONS_KEY_END,  CONS_KEY_END,
      CONS_KEY_END,   CONS_KEY_END,   CONS_KEY_END },		   /* SC_X1_END.  */
    { 0, 0, 0, 0, 0, 0, 0 },
    { CONS_KEY_LEFT,  CONS_KEY_LEFT,  CONS_KEY_LEFT, CONS_KEY_LEFT,
      CONS_KEY_LEFT,  CONS_KEY_LEFT,  CONS_KEY_LEFT },		   /* SC_X1_LEFT.  */
    { CONS_KEY_HOME,  CONS_KEY_HOME,  CONS_KEY_HOME, CONS_KEY_HOME,
      CONS_KEY_HOME,  CONS_KEY_HOME,  CONS_KEY_HOME },		   /* SC_X1_HOME.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 },
    { CONS_KEY_IC,    CONS_KEY_IC,    CONS_KEY_IC,   CONS_KEY_IC, 
      CONS_KEY_IC,    CONS_KEY_IC,    CONS_KEY_IC },		   /* SC_X1_INS.  */
    { CONS_KEY_DC,    CONS_KEY_DC,    CONS_KEY_DC,   CONS_KEY_DC,
      CONS_KEY_DC,    CONS_KEY_DC,    CONS_KEY_DC },		   /* SC_X1_DEL.  */
    { CONS_KEY_DOWN,  CONS_KEY_DOWN,  CONS_KEY_DOWN, CONS_KEY_DOWN,
      CONS_KEY_DOWN,  CONS_KEY_DOWN,  CONS_KEY_DOWN },		   /* SC_X1_DOWN.  */
    { 0, 0, 0, 0, 0, 0, 0 },
    { CONS_KEY_RIGHT, CONS_KEY_RIGHT, CONS_KEY_RIGHT, CONS_KEY_RIGHT,
      CONS_KEY_RIGHT, CONS_KEY_RIGHT, CONS_KEY_RIGHT },		   /* SC_X1_RIGHT.  */
    { CONS_KEY_UP,    CONS_KEY_UP,    CONS_KEY_UP,   CONS_KEY_UP,
      CONS_KEY_UP,    CONS_KEY_UP,    CONS_KEY_UP },		   /* SC_X1_UP.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { CONS_KEY_NPAGE, CONS_KEY_NPAGE, CONS_KEY_NPAGE, CONS_KEY_NPAGE,
      CONS_KEY_NPAGE, CONS_KEY_NPAGE, CONS_KEY_NPAGE },		   /* SC_X1_PGDN.  */
    { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 },
    { CONS_KEY_PPAGE, CONS_KEY_PPAGE, CONS_KEY_PPAGE, CONS_KEY_PPAGE,
      CONS_KEY_PPAGE, CONS_KEY_PPAGE, CONS_KEY_PPAGE }		   /* SC_X1_PGUP.  */
  };

char *sc_x2_to_kc[][7] =
  {
    /* We don't add all those zero entries here.  It's just one key,
       so it's special cased.  */
    { "\e[P", "\e[P",  "\e[P", "\e[P", "\e[P",    "\e[P","\e[P" }, /* SC_X1_BREAK.  */
  };


/* This is a conversion table from i8042 scancode set 1 to set 2.  */
enum scancode sc_set1_to_set2[] =
  {
    0x00,
    SC_ESC,
    SC_1,
    SC_2,
    SC_3,
    SC_4,
    SC_5,
    SC_6,
    SC_7,
    SC_8,
    SC_9,
    SC_0,
    SC_MINUS,
    SC_EQUAL,
    SC_BACKSPACE,
    SC_TAB,
    SC_Q,
    SC_W,
    SC_E,
    SC_R,
    SC_T,
    SC_Y,
    SC_U,
    SC_I,
    SC_O,
    SC_P,
    SC_LEFT_BRACKET,
    SC_RIGHT_BRACKET,
    SC_ENTER,
    SC_LEFT_CTRL,
    SC_A,
    SC_S,
    SC_D,
    SC_F,
    SC_G,
    SC_H,
    SC_J,
    SC_K,
    SC_L,
    SC_SEMICOLON,
    SC_APOSTROPHE,
    SC_BACKQUOTE,
    SC_LEFT_SHIFT,
    SC_BACKSLASH,
    SC_Z,
    SC_X,
    SC_C,
    SC_V,
    SC_B,
    SC_N,
    SC_M,
    SC_COMMA,
    SC_PERIOD,
    SC_SLASH,
    SC_RIGHT_SHIFT,
    SC_PAD_ASTERISK,
    SC_LEFT_ALT,
    SC_SPACE,
    SC_CAPSLOCK,
    SC_F1,
    SC_F2,
    SC_F3,
    SC_F4,
    SC_F5,
    SC_F6,
    SC_F7,
    SC_F8,
    SC_F9,
    SC_F10,
    SC_NUMLOCK,
    SC_SCROLLLOCK,
    SC_PAD_7,
    SC_PAD_8,
    SC_PAD_9,
    SC_PAD_MINUS,
    SC_PAD_4,
    SC_PAD_5,
    SC_PAD_6,
    SC_PAD_PLUS,
    SC_PAD_1,
    SC_PAD_2,
    SC_PAD_3,
    SC_PAD_0,
    SC_PAD_DECIMAL,
    0x00,	/* XXX SYSREQ */
    0x00,
    0x00,
    SC_F11,
    SC_F12,
  };

/* Conversion table for codes which can follow SC_EXTENDED1.  */
enum scancode sc_set1_to_set2_x1[] =
  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    SC_X1_PAD_ENTER,
    SC_X1_RIGHT_CTRL,
    0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    SC_X1_PAD_SLASH,
    0x00,
    SC_X1_PRTSC,
    SC_X1_RIGHT_ALT,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,	/* XXX SC_X1_BREAK */
    SC_X1_HOME,
    SC_X1_UP,
    SC_X1_PGUP,
    0x00,
    SC_X1_LEFT,
    0x00,
    SC_X1_RIGHT,
    0x00,
    SC_X1_END,
    SC_X1_DOWN,
    SC_X1_PGDN,
    SC_X1_INS,
    SC_X1_DEL
  };

static enum scancode
input_next ()
{
  kd_event data_buf;
  int up;
  enum scancode sc;

  do
    {
      /* io_buf_ptr_t is (char *), not (void *).  So I have a few
	 casts to quiet warnings.  */
      mach_msg_type_number_t data_cnt = sizeof (data_buf);
      error_t err = device_read_inband (kbd_dev, 0, -1, sizeof (kd_event),
					(void *) &data_buf, &data_cnt);

      /* XXX The error occured likely because KBD_DEV was closed, so
	 terminate.  */
      if (err)
	return 0;

      if (kbd_repeater_opened && data_buf.type == KEYBD_EVENT)
	{
	  kbd_repeat_key (&data_buf);
	  data_buf.type = 0;
	  continue;
	}
    }
  while (data_buf.type != KEYBD_EVENT);

  /* Some fixed codes which are the same in set 1 and set 2, and have
     the UP flag set.  */
  if (data_buf.value.sc == SC_EXTENDED1
      || data_buf.value.sc == SC_EXTENDED2
      || data_buf.value.sc == SC_ERROR)
    return data_buf.value.sc;

#define SC_SET1_FLAG_UP 0x80
  up = data_buf.value.sc & SC_SET1_FLAG_UP;
  sc = sc_set1_to_set2[data_buf.value.sc &~ SC_SET1_FLAG_UP];
  
  return sc | (up ? SC_FLAG_UP : 0);
}


static void
update_leds (void)
{
  error_t err;
  
  int led = (led_state.scroll_lock ? 1 : 0)
	| (led_state.num_lock ? 2 : 0)
	| (led_state.caps_lock ? 4 : 0);
      
  err = device_set_status (kbd_dev, KDSETLEDS, &led, 1);
  /* Just ignore the error, GNUMach 1.3 and older cannot set the
     keyboard LEDs.  */
}


/* The input loop.  */
static any_t
input_loop (any_t unused)
{
  while (1)
    {
      enum scancode fsc = input_next ();
      enum scancode sc = fsc & ~SC_FLAG_UP;
      int down = !(fsc & SC_FLAG_UP);
      char buf[100];
      size_t size = 0;
      int modifier = -1;

      static struct {
	wchar_t direct;
	unsigned int extended : 2;
	unsigned int left_shift : 1;
	unsigned int right_shift : 1;
	unsigned int caps_lock : 1;
	unsigned int caps_lock_pressed : 1;
	unsigned int left_ctrl : 1;
	unsigned int right_ctrl : 1;
	unsigned int left_alt : 1;
	unsigned int right_alt : 1;
	unsigned int num_lock : 1;
	unsigned int num_lock_pressed : 1;
      } state;

      if (!state.left_alt && !state.right_alt)
	{
	  if (state.left_ctrl || state.right_ctrl)
	    modifier = 2;
	  else if (state.left_shift || state.right_shift)
	    modifier = 1;
	  else
	    modifier = 0;
	}
      else if (state.left_alt)
	{
	  if (state.left_ctrl || state.right_ctrl)
	    modifier = 5;
	  if (state.left_shift || state.right_shift)
	    modifier = 4;
	  else
	    modifier = 3;
	}
      else if (state.right_alt)
	{
	  if (!state.left_ctrl && !state.right_ctrl
	      && !state.left_shift && !state.right_shift)
	    modifier = 6;
	}
      
      if (!state.extended)
	{
	  if (fsc == SC_EXTENDED1)
	    state.extended = 1;
	  else if (fsc == SC_EXTENDED2)
	    state.extended = 2;
	  else if (sc == SC_LEFT_SHIFT)
	    state.left_shift = down;
	  else if (sc == SC_RIGHT_SHIFT)
	    state.right_shift = down;
	  else if (sc == SC_CAPSLOCK)
	    {
	      if (down && !state.caps_lock_pressed)
		{
		  state.caps_lock = !state.caps_lock;
		  state.caps_lock_pressed = 1;
		  led_state.caps_lock = state.caps_lock;
		  update_leds ();
		}
	      else if (!down)
		state.caps_lock_pressed = 0;
	    }
	  else if (sc == SC_LEFT_CTRL)
	    state.left_ctrl = down;
	  else if (sc == SC_LEFT_ALT)
	    state.left_alt = down;
	  else if (state.left_alt && down && IS_FUNC_KEY (sc))
	    {
	      /* The virtual console to switch to.  */
	      int vc = 0;
	      
	      /* Check if a funtion key was pressed. 
		 Choose the virtual console corresponding to that key.  */
	      switch (sc)
		{
		case SC_F1:
		  vc = 1;
		  break;
		case SC_F2:
		  vc = 2;
		  break;
		case SC_F3:
		  vc = 3;
		  break;
		case SC_F4:
		  vc = 4;
		  break;
		case SC_F5:
		  vc = 5;
		  break;
		case SC_F6:
		  vc = 6;
		  break;
		case SC_F7:
		  vc = 7;
		  break;
		case SC_F8:
		  vc = 8;
		  break;
		case SC_F9:
		  vc = 9;
		  break;
		case SC_F10:
		  vc = 10;
		  break;
		case SC_F11:
		  vc = 11;
		  break;
		case SC_F12:
		  vc = 12;
		  break;
		  /* No function key was pressed, don't
		     switch to another vc.  */
		default:
		  vc = 0;
		}

	      if (vc)
		console_switch (vc, 0);
	    }
	  else if (state.left_alt && state.left_ctrl && down && sc == SC_BACKSPACE)
	    console_exit ();
	  else if (state.right_alt && down && sc == SC_PAD_0) /* XXX */
	    state.direct = (state.direct << 4) | 0x0;
	  else if (state.right_alt && down && sc == SC_PAD_1) /* XXX */
	    state.direct = (state.direct << 4) | 0x1;
	  else if (state.right_alt && down && sc == SC_PAD_2) /* XXX */
	    state.direct = (state.direct << 4) | 0x2;
	  else if (state.right_alt && down && sc == SC_PAD_3) /* XXX */
	    state.direct = (state.direct << 4) | 0x3;
	  else if (state.right_alt && down && sc == SC_PAD_4) /* XXX */
	    state.direct = (state.direct << 4) | 0x4;
	  else if (state.right_alt && down && sc == SC_PAD_5) /* XXX */
	    state.direct = (state.direct << 4) | 0x5;
	  else if (state.right_alt && down && sc == SC_PAD_6) /* XXX */
	    state.direct = (state.direct << 4) | 0x6;
	  else if (state.right_alt && down && sc == SC_PAD_7) /* XXX */
	    state.direct = (state.direct << 4) | 0x7;
	  else if (state.right_alt && down && sc == SC_PAD_8) /* XXX */
	    state.direct = (state.direct << 4) | 0x8;
	  else if (state.right_alt && down && sc == SC_PAD_9) /* XXX */
	    state.direct = (state.direct << 4) | 0x9;
	  else if (state.right_alt && down && sc == SC_NUMLOCK) /* XXX */
	    state.direct = (state.direct << 4) | 0xa;
	  else if (state.right_alt && down && sc == SC_PAD_ASTERISK) /* XXX */
	    state.direct = (state.direct << 4) | 0xc;
	  else if (state.right_alt && down && sc == SC_PAD_MINUS) /* XXX */
	    state.direct = (state.direct << 4) | 0xd;
	  else if (state.right_alt && down && sc == SC_PAD_PLUS) /* XXX */
	    state.direct = (state.direct << 4) | 0xe;
	  else if (sc == SC_NUMLOCK)
	    {
	      if (down && !state.num_lock_pressed)
		{
		  state.num_lock = !state.num_lock;
		  state.num_lock_pressed = 1;
		  led_state.num_lock = state.num_lock;
		  update_leds ();
		}
	      else if (!down)
		state.num_lock_pressed = 0;
	    }
	  else if (down && sc < sizeof (sc_to_kc)/sizeof (sc_to_kc[0]))
	    {
#if QUAERENDO_INVENIETIS
	      if (state.left_alt && state.right_alt
		  && sc_to_kc[sc][0][0] >= '0' && sc_to_kc[sc][0][0] <= '9'
		  && sc_to_kc[sc][0][1] == '\0')
		console_deprecated (sc_to_kc[sc][0][0] - '0');
	      else
#endif
		{
		  /* Special rule for caps lock.  */
		  if (modifier == 0 && state.caps_lock
		      && sc_to_kc[sc][modifier]
		      && sc_to_kc[sc][modifier][0] >= 'a'
		      && sc_to_kc[sc][modifier][0] <= 'z'
		      && sc_to_kc[sc][modifier][1] == '\0')
		    modifier = 1;
		  else if (state.num_lock && sc == SC_PAD_0)
		    {
		      modifier = 0;
		      sc = SC_0;
		    }
		  else if (state.num_lock && sc == SC_PAD_1)
		    {
		      modifier = 0;
		      sc = SC_1;
		    }
		  else if (state.num_lock && sc == SC_PAD_2)
		    {
		      modifier = 0;
		      sc = SC_2;
		    }
		  else if (state.num_lock && sc == SC_PAD_3)
		    {
		      modifier = 0;
		      sc = SC_3;
		    }
		  else if (state.num_lock && sc == SC_PAD_4)
		    {
		      modifier = 0;
		      sc = SC_4;
		    }
		  else if (state.num_lock && sc == SC_PAD_5)
		    {
		      modifier = 0;
		      sc = SC_5;
		    }
		  else if (state.num_lock && sc == SC_PAD_6)
		    {
		      modifier = 0;
		      sc = SC_6;
		    }
		  else if (state.num_lock && sc == SC_PAD_7)
		    {
		      modifier = 0;
		      sc = SC_7;
		    }
		  else if (state.num_lock && sc == SC_PAD_8)
		    {
		      modifier = 0;
		      sc = SC_8;
		    }
		  else if (state.num_lock && sc == SC_PAD_9)
		    {
		      modifier = 0;
		      sc = SC_9;
		    }
		  else if (state.num_lock && sc == SC_PAD_DECIMAL)
		    {
		      modifier = 0;
		      sc = SC_PERIOD;
		    }
		  
		  if (modifier >= 0 && sc_to_kc[sc][modifier])
		    {
		      if (!sc_to_kc[sc][modifier][0])
			{
			  /* Special meaning, emit NUL.  */
			  assert (size < 100);
			  buf[size++] = '\0';
			}
		      else
			{
			  assert (size
				  < 101 - strlen(sc_to_kc[sc][modifier]));
			  strcpy (&buf[size], sc_to_kc[sc][modifier]);
			  size += strlen (sc_to_kc[sc][modifier]);
			}
		    }
		}
	    }
	}
      else if (state.extended == 1)
	{
	  state.extended = 0;
	  if (sc == SC_X1_RIGHT_CTRL)
	    state.right_ctrl = down;
	  else if (sc == SC_X1_RIGHT_ALT)
	    {
	      state.right_alt = down;
	      
	      /* Handle the AltGR+Keypad direct input.  */
	      if (down)
		state.direct = (wchar_t) 0;
	      else
		{
		  if (state.direct != (wchar_t) 0)
		    {
		      char *buffer = &buf[size];
		      size_t left = sizeof (buf) - size;
		      char *inbuf = (char *) &state.direct;
		      size_t inbufsize = sizeof (wchar_t);
		      size_t nr;
		      
		      nr = iconv (cd, &inbuf, &inbufsize, &buffer, &left);
		      if (nr == (size_t) -1)
			{
			  if (errno == E2BIG)
			    console_error (L"Input buffer overflow");
			  else if (errno == EILSEQ)
			    console_error
			      (L"Input contained invalid byte sequence");
			  else if (errno == EINVAL)
			    console_error
			      (L"Input contained incomplete byte sequence");
			  else
			    console_error
			      (L"Input caused unexpected error");
			}
		      size = sizeof (buf) - left;
		    }
		}
	    }
	  else if (state.right_alt && down && sc == SC_X1_PAD_SLASH) /* XXX */
	    state.direct = (state.direct << 4) | 0xb;
	  else if (state.right_alt && down && sc == SC_X1_PAD_ENTER) /* XXX */
	    state.direct = (state.direct << 4) | 0xf;
	  else if (state.left_alt && down && sc == SC_X1_RIGHT) /* XXX */
	    console_switch (0, 1);
	  else if (state.left_alt && down && sc == SC_X1_LEFT) /* XXX */
	    console_switch (0, -1);
	  else if (state.left_alt && down && sc == SC_X1_UP) /* XXX */
	    console_scrollback (CONS_SCROLL_DELTA_LINES, 1);
	  else if (state.left_alt && down && sc == SC_X1_DOWN) /* XXX */
	    console_scrollback (CONS_SCROLL_DELTA_LINES, -1);
	  else if ((state.right_shift || state.left_shift)
		   && down && sc == SC_X1_PGUP) /* XXX */
	    console_scrollback (CONS_SCROLL_DELTA_SCREENS, 0.5);
	  else if ((state.right_shift || state.left_shift)
		   && down && sc == SC_X1_PGDN) /* XXX */
	    console_scrollback (CONS_SCROLL_DELTA_SCREENS, -0.5);
	  else if (down && sc < sizeof (sc_x1_to_kc)/sizeof (sc_x1_to_kc[0]))
	    {
	      if (modifier >= 0 && sc_x1_to_kc[sc][modifier])
		{
		  assert (size < 101 - strlen(sc_x1_to_kc[sc][modifier]));
		  strcpy (&buf[size], sc_x1_to_kc[sc][modifier]);
		  size += strlen (sc_x1_to_kc[sc][modifier]);
		}
	    }
	}
      else if (state.extended == 2)
	state.extended = 3;
      else if (state.extended == 3)
	state.extended = 0;
      
      if (size)
	console_input (buf, size);
    }
  return 0;
}




static const char doc[] = "PC Keyboard Driver";

static struct arguments
{
  char *xkbdir;
  char *keymapfile;
  char *keymap;
  char *composefile;
  int ctrlaltbs;
  int pos;
} arguments = { ctrlaltbs: 1 };

static const struct argp_option options[] =
  {
    {"xkbdir", 'x', "DIR", 0,
     "directory containing the XKB configuration files" },
    {"keymapfile", 'f', "FILE", 0,
     "file containing the keymap" },
    {"keymap", 'k', "SECTIONNAME", 0,
     "choose keymap"},
    {"compose", 'o', "COMPOSEFILE", 0,
     "Compose file to load (default none)"},
    {"ctrlaltbs", 'c', 0, 0,
     "CTRL + Alt + Backspace will exit the console client (default)."},
    {"no-ctrlaltbs", 'n', 0 , 0,
     "CTRL + Alt + Backspace will not exit the console client."},
    {"repeat",		'r', "NODE", OPTION_ARG_OPTIONAL,
     "Set a repeater translator on NODE (default: " DEFAULT_REPEATER_NODE ")"},
    { 0 }
  };

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  struct arguments *arguments = state->input;
  
  switch (key)
    {
    case 'x':
      arguments->xkbdir = arg;
      break;

    case 'f':
      arguments->keymapfile = arg;
      break;

    case 'k':
      arguments->keymap = arg;
      break;

    case 'o':
      arguments->composefile = arg;
      break;

    case 'c':
      arguments->ctrlaltbs = 1;
      break;

    case 'n':
      arguments->ctrlaltbs = 0;
      break;

    case 'r':
      repeater_node = arg ? arg: DEFAULT_REPEATER_NODE;
      break;
      
    case ARGP_KEY_END:
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }

  arguments->pos = state->next;
  return 0;
}

static struct argp argp = {options, parse_opt, 0, doc};

/* Initialize the PC keyboard driver.  */
static error_t
pc_kbd_init (void **handle, int no_exit, int argc, char *argv[], int *next)
{
  error_t err;

  /* Parse the arguments.  */
  arguments.pos = 1;
  err = argp_parse (&argp, argc, argv, ARGP_IN_ORDER | ARGP_NO_EXIT
		    | ARGP_SILENT, 0 , &arguments);
  *next += arguments.pos - 1;

  if (err && err != EINVAL)
    return err;


  return 0;
}


/* Start the PC keyboard driver.  */
static error_t
pc_kbd_start (void *handle)
{
  int data = KB_EVENT;
  error_t err;
  device_t device_master;

  cd = iconv_open ("UTF-8", "WCHAR_T");
  if (cd == (iconv_t) -1)
    return errno;

  err = get_privileged_ports (0, &device_master);
  if (err)
    {
      iconv_close (cd);
      return err;
    }

  err = device_open (device_master, D_READ, "kbd", &kbd_dev);

  mach_port_deallocate (mach_task_self (), device_master);
  if (err)
    {
      iconv_close (cd);
      return err;
    }

  err = device_set_status (kbd_dev, KDSKBDMODE, &data, 1);
  if (err)
    {
      device_close (kbd_dev);
      mach_port_deallocate (mach_task_self (), kbd_dev);
      iconv_close (cd);
      return err;
    }
  update_leds ();

  err = driver_add_input (&pc_kbd_ops, NULL);
  if (err)
    {
      data = KB_ASCII;
      device_set_status (kbd_dev, KDSKBDMODE, &data, 1);
      device_close (kbd_dev);
      mach_port_deallocate (mach_task_self (), kbd_dev);
      iconv_close (cd);
      return err;
    }

  if (repeater_node)
    kbd_setrepeater (repeater_node, &cnode);

  cthread_detach (cthread_fork (input_loop, NULL));

  return 0;
}

/* Deinitialize the PC keyboard driver.  */
static error_t
pc_kbd_fini (void *handle, int force)
{
  int data = KB_ASCII;

  driver_remove_input (&pc_kbd_ops, NULL);
  device_set_status (kbd_dev, KDSKBDMODE, &data, 1);
  device_close (kbd_dev);
  mach_port_deallocate (mach_task_self (), kbd_dev);
  iconv_close (cd);

  console_unregister_consnode (cnode);
  console_destroy_consnode (cnode);

  return 0;
}


/* Set the scroll lock status indication (Scroll LED) to ONOFF.  */
static error_t
pc_kbd_set_scroll_lock_status (void *handle, int onoff)
{
  led_state.scroll_lock = onoff;
  update_leds ();
  return 0;
}


struct driver_ops driver_pc_kbd_ops =
  {
    pc_kbd_init,
    pc_kbd_start,
    pc_kbd_fini
  };

static struct input_ops pc_kbd_ops =
  {
    pc_kbd_set_scroll_lock_status,
    NULL
  };
