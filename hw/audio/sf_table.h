/*
 * libmad - MPEG audio decoder library
 * Copyright (C) 2000-2004 Underbit Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <mad.h>

static const mad_fixed_t sf_table[64] = {
  MAD_F(0x20000000),
  MAD_F(0x1965fea5),
  MAD_F(0x1428a2fa),
  MAD_F(0x10000000),
  MAD_F(0x0cb2ff53),
  MAD_F(0x0a14517d),
  MAD_F(0x08000000),
  MAD_F(0x06597fa9),

  MAD_F(0x050a28be),
  MAD_F(0x04000000),
  MAD_F(0x032cbfd5),
  MAD_F(0x0285145f),
  MAD_F(0x02000000),
  MAD_F(0x01965fea),
  MAD_F(0x01428a30),
  MAD_F(0x01000000),

  MAD_F(0x00cb2ff5),
  MAD_F(0x00a14518),
  MAD_F(0x00800000),
  MAD_F(0x006597fb),
  MAD_F(0x0050a28c),
  MAD_F(0x00400000),
  MAD_F(0x0032cbfd),
  MAD_F(0x00285146),

  MAD_F(0x00200000),
  MAD_F(0x001965ff),
  MAD_F(0x001428a3),
  MAD_F(0x00100000),
  MAD_F(0x000cb2ff),
  MAD_F(0x000a1451),
  MAD_F(0x00080000),
  MAD_F(0x00065980),

  MAD_F(0x00050a29),
  MAD_F(0x00040000),
  MAD_F(0x00032cc0),
  MAD_F(0x00028514),
  MAD_F(0x00020000),
  MAD_F(0x00019660),
  MAD_F(0x0001428a),
  MAD_F(0x00010000),

  MAD_F(0x0000cb30),
  MAD_F(0x0000a145),
  MAD_F(0x00008000),
  MAD_F(0x00006598),
  MAD_F(0x000050a3),
  MAD_F(0x00004000),
  MAD_F(0x000032cc),
  MAD_F(0x00002851),

  MAD_F(0x00002000),
  MAD_F(0x00001966),
  MAD_F(0x00001429),
  MAD_F(0x00001000),
  MAD_F(0x00000cb3),
  MAD_F(0x00000a14),
  MAD_F(0x00000800),
  MAD_F(0x00000659),

  MAD_F(0x0000050a),
  MAD_F(0x00000400),
  MAD_F(0x0000032d),
  MAD_F(0x00000285),
  MAD_F(0x00000200),
  MAD_F(0x00000196),
  MAD_F(0x00000143),
  MAD_F(0x00000000),
};
