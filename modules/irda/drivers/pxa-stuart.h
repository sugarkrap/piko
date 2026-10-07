/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PXA_STUART_H
#define PXA_STUART_H

#define STRBR		0x00
#define STTHR		0x00
#define STDLL		0x00
#define STIER		0x04
#define STDLH		0x04
#define STIIR		0x08
#define STFCR		0x08
#define STLCR		0x0c
#define STMCR		0x10
#define STLSR		0x14
#define STISR		0x20

#define IER_RAVIE	(1 << 0)
#define IER_TIE		(1 << 1)
#define IER_RLSE	(1 << 2)
#define IER_RTIOE	(1 << 4)
#define IER_UUE		(1 << 6)

#define FCR_TRFIFOE	(1 << 0)
#define FCR_ITL1	(1 << 6)
#define FCR_ITL2	(1 << 7)
#define FCR_ITL_32	(FCR_ITL2 | FCR_ITL1)

#define LCR_WLS0	(1 << 0)
#define LCR_WLS1	(1 << 1)
#define LCR_DLAB	(1 << 7)

#define MCR_OUT2	(1 << 3)

#define LSR_DR		(1 << 0)
#define LSR_OE		(1 << 1)
#define LSR_PE		(1 << 2)
#define LSR_FE		(1 << 3)
#define LSR_BI		(1 << 4)
#define LSR_TDRQ	(1 << 5)
#define LSR_TEMT	(1 << 6)
#define LSR_FIFOE	(1 << 7)

#endif
