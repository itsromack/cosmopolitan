/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2020 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ This program is free software; you can redistribute it and/or modify         │
│ it under the terms of the GNU General Public License as published by         │
│ the Free Software Foundation; version 2 of the License.                      │
│                                                                              │
│ This program is distributed in the hope that it will be useful, but          │
│ WITHOUT ANY WARRANTY; without even the implied warranty of                   │
│ MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU             │
│ General Public License for more details.                                     │
│                                                                              │
│ You should have received a copy of the GNU General Public License            │
│ along with this program; if not, write to the Free Software                  │
│ Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA                │
│ 02110-1301 USA                                                               │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "libc/assert.h"
#include "libc/log/log.h"
#include "libc/macros.h"
#include "libc/runtime/runtime.h"
#include "tool/build/lib/alu.h"
#include "tool/build/lib/endian.h"
#include "tool/build/lib/flags.h"
#include "tool/build/lib/ioports.h"
#include "tool/build/lib/machine.h"
#include "tool/build/lib/memory.h"
#include "tool/build/lib/modrm.h"
#include "tool/build/lib/string.h"
#include "tool/build/lib/throw.h"

static uint64_t ReadInt(uint8_t p[8], unsigned long w) {
  switch (w) {
    case 0:
      return Read8(p);
    case 1:
      return Read16(p);
    case 2:
      return Read32(p);
    case 3:
      return Read64(p);
    default:
      unreachable;
  }
}

static void WriteInt(uint8_t p[8], uint64_t x, unsigned long w) {
  switch (w) {
    case 0:
      Write8(p, x);
      break;
    case 1:
      Write16(p, x);
      break;
    case 2:
      Write64(p, x);
      break;
    case 3:
      Write64(p, x);
      break;
    default:
      unreachable;
  }
}

void OpString(struct Machine *m, uint32_t rde, int op) {
  void *p[2];
  unsigned n;
  uint64_t asz;
  bool compare;
  int64_t sgn, v;
  uint8_t s[3][8];
  sgn = GetFlag(m->flags, FLAGS_DF) ? -1 : 1;
  asz = Asz(rde) ? 0xffffffff : 0xffffffffffffffff;
  n = 1 << RegLog2(rde);
  for (;;) {
    if (Rep(rde) && !Read64(m->cx)) break;
    v = 0;
    *p = NULL;
    compare = false;
    switch (op) {
      case STRING_CMPS:
        Alu(RegLog2(rde), ALU_SUB,
            ReadInt(Load(m, (Read64(m->si) + GetSegment(m)) & asz, n, s[2]),
                    RegLog2(rde)),
            ReadInt(Load(m, Read64(m->di) & asz, n, s[1]), RegLog2(rde)),
            &m->flags);
        Write64(m->di, (Read64(m->di) + sgn * n) & asz);
        Write64(m->si, (Read64(m->si) + sgn * n) & asz);
        compare = true;
        break;
      case STRING_MOVS:
        memcpy(BeginStore(m, (v = Read64(m->di) & asz), n, p, s[0]),
               Load(m, (Read64(m->si) + GetSegment(m)) & asz, n, s[1]), n);
        Write64(m->di, (Read64(m->di) + sgn * n) & asz);
        Write64(m->si, (Read64(m->si) + sgn * n) & asz);
        break;
      case STRING_STOS:
        memcpy(BeginStore(m, (v = Read64(m->di) & asz), n, p, s[0]), m->ax, n);
        Write64(m->di, (Read64(m->di) + sgn * n) & asz);
        break;
      case STRING_LODS:
        memcpy(m->ax, Load(m, (Read64(m->si) + GetSegment(m)) & asz, n, s[1]),
               n);
        Write64(m->si, (Read64(m->si) + sgn * n) & asz);
        break;
      case STRING_SCAS:
        Alu(RegLog2(rde), ALU_SUB,
            ReadInt(Load(m, Read64(m->di) & asz, n, s[1]), RegLog2(rde)),
            ReadInt(m->ax, RegLog2(rde)), &m->flags);
        Write64(m->di, (Read64(m->di) + sgn * n) & asz);
        compare = true;
        break;
      case STRING_OUTS:
        OpOut(m, Read16(m->dx),
              ReadInt(Load(m, (Read64(m->si) + GetSegment(m)) & asz, n, s[1]),
                      RegLog2(rde)));
        Write64(m->si, (Read64(m->si) + sgn * n) & asz);
        break;
      case STRING_INS:
        WriteInt(BeginStore(m, (v = Read64(m->di) & asz), n, p, s[0]),
                 OpIn(m, Read16(m->dx)), RegLog2(rde));
        Write64(m->di, (Read64(m->di) + sgn * n) & asz);
        break;
      default:
        abort();
    }
    EndStore(m, v, n, p, s[0]);
    if (!Rep(rde)) break;
    Write64(m->cx, Read64(m->cx) - 1);
    if (compare) {
      if (Rep(rde) == 2 && GetFlag(m->flags, FLAGS_ZF)) break;
      if (Rep(rde) == 3 && !GetFlag(m->flags, FLAGS_ZF)) break;
    }
  }
}

void OpRepMovsbEnhanced(struct Machine *m, uint32_t rde) {
  bool failed;
  uint8_t *direal, *sireal;
  unsigned diremain, siremain, i, n;
  uint64_t divirtual, sivirtual, diactual, siactual, failaddr, asz, cx;
  if (!(cx = Read64(m->cx))) return;
  failed = false;
  failaddr = 0;
  asz = Asz(rde) ? 0xffffffff : 0xffffffffffffffff;
  divirtual = Read64(m->di) & asz;
  sivirtual = Read64(m->si) & asz;
  SetWriteAddr(m, (GetSegment(m) + divirtual) & asz, cx);
  SetReadAddr(m, (GetSegment(m) + sivirtual) & asz, cx);
  do {
    diactual = (GetSegment(m) + divirtual) & asz;
    siactual = (GetSegment(m) + sivirtual) & asz;
    if (!(direal = FindReal(m, diactual))) {
      failaddr = diactual;
      failed = true;
      break;
    }
    if (!(sireal = FindReal(m, siactual))) {
      failaddr = siactual;
      failed = true;
      break;
    }
    diremain = 0x1000 - (divirtual & 0xfff);
    siremain = 0x1000 - (sivirtual & 0xfff);
    n = MIN(cx, MIN(diremain, siremain));
    for (i = 0; i < n; ++i) {
      direal[i] = sireal[i];
    }
    cx -= n;
    divirtual = (divirtual + n) & asz;
    sivirtual = (sivirtual + n) & asz;
  } while (cx);
  Write64(m->cx, cx);
  Write64(m->di, divirtual);
  Write64(m->si, sivirtual);
  if (failed) ThrowSegmentationFault(m, failaddr);
}

void OpRepStosbEnhanced(struct Machine *m, uint32_t rde) {
  bool failed;
  uint8_t *direal, al;
  unsigned diremain, i, n;
  uint64_t divirtual, diactual, failaddr, asz, cx;
  if (!(cx = Read64(m->cx))) return;
  failaddr = 0;
  failed = false;
  al = Read8(m->ax);
  asz = Asz(rde) ? 0xffffffff : 0xffffffffffffffff;
  divirtual = Read64(m->di) & asz;
  SetWriteAddr(m, (GetSegment(m) + divirtual) & asz, cx);
  do {
    diactual = (GetSegment(m) + divirtual) & asz;
    if (!(direal = FindReal(m, diactual))) {
      failaddr = diactual;
      failed = true;
      break;
    }
    diremain = 0x1000 - (divirtual & 0xfff);
    n = MIN(cx, diremain);
    for (i = 0; i < n; ++i) {
      direal[i] = al;
    }
    cx -= n;
    divirtual = (divirtual + n) & asz;
  } while (cx);
  Write64(m->cx, cx);
  Write64(m->di, divirtual);
  if (failed) ThrowSegmentationFault(m, failaddr);
}

void OpMovsb(struct Machine *m, uint32_t rde) {
  if (Rep(rde) && !GetFlag(m->flags, FLAGS_DF)) {
    OpRepMovsbEnhanced(m, rde);
  } else {
    OpString(m, rde, STRING_MOVS);
  }
}

void OpStosb(struct Machine *m, uint32_t rde) {
  if (Rep(rde) && !GetFlag(m->flags, FLAGS_DF)) {
    OpRepStosbEnhanced(m, rde);
  } else {
    OpString(m, rde, STRING_STOS);
  }
}
