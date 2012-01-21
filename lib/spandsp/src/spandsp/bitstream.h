/*
 * SpanDSP - a series of DSP components for telephony
 *
 * bitstream.h - Bitstream composition and decomposition routines.
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2006 Steve Underwood
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: bitstream.h,v 1.6 2007/04/08 08:16:17 steveu Exp $
 */

/*! \file */

#if !defined(_SPANDSP_BITSTREAM_H_)
#define _SPANDSP_BITSTREAM_H_

/*! \page bitstream_page Bitstream composition and decomposition
\section bitstream_page_sec_1 What does it do?

\section bitstream_page_sec_2 How does it work?
*/

typedef struct
{
    unsigned int bitstream;
    unsigned int residue;
} bitstream_state_t;


#if defined(__cplusplus)
extern "C"
{
#endif

bitstream_state_t *bitstream_init(bitstream_state_t *s);

void bitstream_put(bitstream_state_t *s, uint8_t **c, unsigned int value, int bits);

void bitstream_flush(bitstream_state_t *s, uint8_t **c);

void bitstream_put2(bitstream_state_t *s, uint8_t **c, unsigned int value, int bits);

void bitstream_flush2(bitstream_state_t *s, uint8_t **c);

unsigned int bitstream_get(bitstream_state_t *s, const uint8_t **c, int bits);

unsigned int bitstream_get2(bitstream_state_t *s, const uint8_t **c, int bits);

#if defined(__cplusplus)
}
#endif

#endif
/*- End of file ------------------------------------------------------------*/
