/*
 * Heatline in-game overlay: the one list of every metric the overlay can show.
 *
 * Plain C preprocessor so both sides read the same table: the C++ overlay
 * engine (the registry, the layout, the renderer) and Heatline's C collector
 * (which fills in the numbers). To add a metric: add one X(...) line here, then
 * fill its value in overlay_collect_metrics (main.c). Nothing else changes.
 *
 *   X(ID, "token", "label", 0xRRGGBBAA, decimals, "suffix", "widest")
 *
 *   ID       enumerator, OVM_<ID>; also the position in the value arrays
 *   token    what a layout spec calls it; no spaces, ',' or ';'
 *   label    drawn name, at most 7 characters
 *   color    default name colour
 *   decimals digits after the point
 *   suffix   unit written after the number ("" for none)
 *   widest   a typical widest value; reserves the number column's width so a
 *            changing value does not shift the cells beside it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COMMON_FPS_OVERLAY_METRIC_LIST_H
#define COMMON_FPS_OVERLAY_METRIC_LIST_H

#define OVERLAY_METRIC_LIST(X)                                                 \
  /* ---- game performance (Heatline's fps.c) ---- */                          \
  X(FPS,    "FPS",    "FPS",     0x4cd964ffU, 0, "",     "888")                \
  X(AVG,    "AVG",    "AVG",     0x33d6e0ffU, 0, "",     "888")                \
  X(P1,     "P1",     "P1",      0xffa733ffU, 0, "",     "888")                \
  X(P02,    "P0.2",   "P0.2",    0xff5a4fffU, 0, "",     "888")                \
  X(LOW1,   "LOW1",   "1%LOW",   0xffd60affU, 0, "",     "888")                \
  X(LOW02,  "LOW02",  "0.2%LOW", 0xff9f0affU, 0, "",     "888")                \
  X(FT,     "FT",     "FT",      0xbf5af2ffU, 1, "ms",   "88.8ms")             \
  X(FTWST,  "FTWST",  "FT MAX",  0xff6bd6ffU, 1, "ms",   "88.8ms")             \
  X(SAVG,   "SAVG",   "S.AVG",   0x30d158ffU, 0, "",     "888")                \
  X(SMIN,   "SMIN",   "S.MIN",   0x66d4cfffU, 0, "",     "888")                \
  X(SMAX,   "SMAX",   "S.MAX",   0xa4f4a4ffU, 0, "",     "888")                \
  X(STUT,   "STUT",   "STUT",    0xff453affU, 1, "%",    "88.8%")              \
  X(STUTN,  "STUTN",  "STUT N",  0xff6961ffU, 0, "",     "888")                \
  /* ---- game performance, "more metrics" (turn on in the dashboard) ---- */  \
  X(P5,     "P5",     "P5",      0x5ac8faffU, 0, "",     "888")                \
  X(P50,    "P50",    "P50",     0x5ac8faffU, 0, "",     "888")                \
  X(P95,    "P95",    "P95",     0x5ac8faffU, 0, "",     "888")                \
  X(P99,    "P99",    "P99",     0x5ac8faffU, 0, "",     "888")                \
  X(P01,    "P0.1",   "P0.1",    0x5ac8faffU, 0, "",     "888")                \
  X(LOW01,  "LOW01",  "0.1%LOW", 0x5ac8faffU, 0, "",     "888")                \
  X(IL1,    "IL1",    "1%INT",   0x5ac8faffU, 0, "",     "888")                \
  X(IL02,   "IL02",   "0.2%INT", 0x5ac8faffU, 0, "",     "888")                \
  X(IL01,   "IL01",   "0.1%INT", 0x5ac8faffU, 0, "",     "888")                \
  X(ASTD,   "ASTD",   "A.STD",   0x5ac8faffU, 1, "",     "88.8")               \
  X(FTMIN,  "FTMIN",  "FT MIN",  0xbf5af2ffU, 1, "ms",   "88.8ms")             \
  X(FTMAX,  "FTMAX",  "FT HI",   0xbf5af2ffU, 1, "ms",   "88.8ms")             \
  /* ---- temperatures ---- */                                                \
  X(CPUT,   "CPUT",   "CPU T",   0xff7a59ffU, 0, "C",    "88C")                \
  X(SOCT,   "SOCT",   "SOC T",   0xff9f6bffU, 0, "C",    "88C")                \
  X(HOTT,   "HOTT",   "HOT",     0xff4f4fffU, 0, "C",    "88C")                \
  /* ---- clocks ---- */                                                      \
  X(CPUMHZ, "CPUMHZ", "CPU MHz", 0x7c9cffffU, 0, "",     "8888")               \
  X(GPUMHZ, "GPUMHZ", "GPU MHz", 0x9b8cffffU, 0, "",     "8888")               \
  /* ---- power ---- */                                                       \
  X(CPUW,   "CPUW",   "CPU W",   0xffd60affU, 1, "W",    "88.8W")              \
  X(GPUW,   "GPUW",   "GPU W",   0xffe066ffU, 1, "W",    "888.8W")             \
  X(TOTW,   "TOTW",   "CPU+GPU", 0xffc300ffU, 1, "W",    "888.8W")             \
  X(CVOLT,  "CVOLT",  "CPU V",   0xf2c94cffU, 3, "V",    "8.888V")             \
  X(GVOLT,  "GVOLT",  "GPU V",   0xf2d36bffU, 3, "V",    "8.888V")             \
  X(CAMP,   "CAMP",   "CPU A",   0xe8b84affU, 1, "A",    "88.8A")              \
  X(GAMP,   "GAMP",   "GPU A",   0xe8c46affU, 1, "A",    "888.8A")             \
  /* ---- fan ---- */                                                         \
  X(FAN,    "FAN",    "FAN",     0x66d4cfffU, 0, "%",    "888%")               \
  /* ---- memory ---- */                                                      \
  X(RAMS,   "RAMS",   "SYSRAM",  0xbf5af2ffU, 1, "GB",   "88.8GB")             \
  X(RAMG,   "RAMG",   "GAMERAM", 0xd68cffffU, 1, "GB",   "88.8GB")             \
  /* ---- CPU load ---- */                                                    \
  X(LOAD,   "LOAD",   "CPU",     0x30d158ffU, 0, "%",    "888%")               \
  /* ---- CPU cores ---- */                                                   \
  X(C1MHZ, "C1MHZ", "C1 MHz", 0x7c9cffffU, 0, "",     "8888")                \
  X(C2MHZ, "C2MHZ", "C2 MHz", 0x7c9cffffU, 0, "",     "8888")                \
  X(C3MHZ, "C3MHZ", "C3 MHz", 0x7c9cffffU, 0, "",     "8888")                \
  X(C4MHZ, "C4MHZ", "C4 MHz", 0x7c9cffffU, 0, "",     "8888")                \
  X(C5MHZ, "C5MHZ", "C5 MHz", 0x7c9cffffU, 0, "",     "8888")                \
  X(C6MHZ, "C6MHZ", "C6 MHz", 0x7c9cffffU, 0, "",     "8888")                \
  X(C7MHZ, "C7MHZ", "C7 MHz", 0x7c9cffffU, 0, "",     "8888")                \
  X(C8MHZ, "C8MHZ", "C8 MHz", 0x7c9cffffU, 0, "",     "8888")                \
  X(C1LOAD,"C1LOAD","C1 LOAD",0x30d158ffU, 0, "%",    "888%")               \
  X(C2LOAD,"C2LOAD","C2 LOAD",0x30d158ffU, 0, "%",    "888%")               \
  X(C3LOAD,"C3LOAD","C3 LOAD",0x30d158ffU, 0, "%",    "888%")               \
  X(C4LOAD,"C4LOAD","C4 LOAD",0x30d158ffU, 0, "%",    "888%")               \
  X(C5LOAD,"C5LOAD","C5 LOAD",0x30d158ffU, 0, "%",    "888%")               \
  X(C6LOAD,"C6LOAD","C6 LOAD",0x30d158ffU, 0, "%",    "888%")               \
  X(C7LOAD,"C7LOAD","C7 LOAD",0x30d158ffU, 0, "%",    "888%")               \
  X(C8LOAD,"C8LOAD","C8 LOAD",0x30d158ffU, 0, "%",    "888%")               \
  /* ---- network ---- */                                                     \
  X(RX,     "RX",     "RX Mb/s", 0x64d2ffffU, 1, "",     "888.8")              \
  X(TX,     "TX",     "TX Mb/s", 0x40c8e0ffU, 1, "",     "888.8")              \
  X(LATGW,  "LATGW",  "ROUTER",  0xffb340ffU, 1, "ms",   "888.8ms")            \
  X(LATNET, "LATNET", "PING",    0xff9f0affU, 1, "ms",   "888.8ms")            \
  X(JITGW,  "JITGW",  "RT JIT",  0xffcc80ffU, 1, "ms",   "88.8ms")             \
  X(JITNET, "JITNET", "NET JIT", 0xffb74dffU, 1, "ms",   "88.8ms")

/* OVM_<ID> for every metric, then OVM_COUNT. Usable from C and C++. */
#define OVERLAY_METRIC_ENUM_ENTRY(id, token, label, color, dec, suffix, widest) \
  OVM_##id,

#ifdef __cplusplus
/* The enum itself is declared in overlay_metrics.hpp (as an enum class). */
#else
enum {
  OVERLAY_METRIC_LIST(OVERLAY_METRIC_ENUM_ENTRY)
  OVM_COUNT
};
#endif

#endif /* COMMON_FPS_OVERLAY_METRIC_LIST_H */
