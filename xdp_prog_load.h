/*
 * Copyright (C) 2019
 * Authors:	Ivan Khoronzhuk <ivan.khoronzhuk@linaro.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef PLGET_XDP_PROG_LOAD_H
#define PLGET_XDP_PROG_LOAD_H

#include "plget.h"

#ifdef CONF_AFXDP

int xdp_load_prog(struct plgett *plget);
void xdp_unload_prog(void);

#else

inline static int xdp_load_prog(struct plgett *plget)
{
	return 0;
}

inline static void xdp_unload_prog(void)
{
}

#endif

#endif
