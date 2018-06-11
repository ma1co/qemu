/*
 *  Copyright 2006,2007 Sony Corporation.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  version 2 of the  License.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "qemu/osdep.h"
#include "qemu/lz77.h"

static const int len_table[] = {
    3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16,
    32, 64
};

int lz77_inflate(unsigned char *src, int src_len, unsigned char *dst, int dst_len, unsigned char **sd)
{
    int type, u_i, l, bd;
    unsigned char *s = src, *d = dst;
    unsigned char *de = dst + dst_len;
    unsigned char *se = src + src_len - 1;

    if (!src || src_len < 4 || !dst) {
        return -1;
    }

    switch (*s++) {
        case 0xF0:// compressed
            while (s < se) {
                u_i = 0;
                type = *s++;

                while (u_i++ < 8 && s < se) {
                    if (type & 1) {// codeword
                        l = (s[0] & 0xF0) >> 4;
                        l = len_table[l];
                        l = de - d > l ? l : de - d;
                        bd = ((s[0] & 0x0F) << 8) | s[1];
                        s += 2;

                        if (bd) {
                            while (l--) {
                                *d = *(d - bd);
                                d++;
                            }
                        } else {
                            goto inflate_end;
                        }
                    } else {
                        *d++ = *s++;
                    }

                    type >>= 1;
                }
            }
            break;

        case 0x0F:// raw
            l = s[1] | (s[2] << 8);
            l = dst_len > l ? l : dst_len;
            s += 3;
            memcpy(d, s, l);
            d += l;
            s += l;
            break;

        default:
            return -1;
    };

inflate_end:
    if (sd) {
        *sd = s;
    }

    return d - dst;
}
