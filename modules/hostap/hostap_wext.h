#ifndef HOSTAP_WEXT_H
#define HOSTAP_WEXT_H

#include <linux/wireless.h>
#include <net/iw_handler.h>

#ifndef IW_MODE_ADHOC
#define IW_MODE_ADHOC 1
#endif
#ifndef IW_MODE_INFRA
#define IW_MODE_INFRA 2
#endif
#ifndef IW_MODE_MASTER
#define IW_MODE_MASTER 3
#endif
#ifndef IW_MODE_REPEAT
#define IW_MODE_REPEAT 4
#endif
#ifndef IW_MODE_SECOND
#define IW_MODE_SECOND 5
#endif
#ifndef IW_MODE_MONITOR
#define IW_MODE_MONITOR 6
#endif

#if WIRELESS_EXT >= 15

#ifndef PRISM2_USE_WE_SUB_IOCTLS
#define PRISM2_USE_WE_SUB_IOCTLS
#endif 

#ifndef PRISM2_USE_WE_TYPE_ADDR
#define PRISM2_USE_WE_TYPE_ADDR
#endif 
#endif 

#ifdef PRISM2_USE_WE_TYPE_ADDR

#ifndef IW_PRIV_TYPE_ADDR
#define IW_PRIV_TYPE_ADDR 0x6000
#endif 
#endif 

#ifndef IW_QUAL_LEVEL_UPDATED
#define IW_QUAL_LEVEL_UPDATED	0x02	
#define IW_QUAL_NOISE_UPDATED	0x04
#define IW_QUAL_QUAL_INVALID	0x10	
#endif
#ifndef IW_QUAL_ALL_UPDATED
#define IW_QUAL_ALL_UPDATED	0x07	
#define IW_QUAL_DBM		0x08	
#define IW_QUAL_ALL_INVALID	0x70	
#endif

#endif 
