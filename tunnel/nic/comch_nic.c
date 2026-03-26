/*
 * comch_nic.c — Version dispatcher for NIC-side DOCA Comch.
 *
 * Selects the concrete implementation at compile time via COMCH_NIC_DOCA_VER:
 *
 *   COMCH_NIC_DOCA_VER == 15   →  comch_nic_doca15.c   (DOCA 1.5, BF2, current)
 *   COMCH_NIC_DOCA_VER >= 30   →  comch_nic_doca31.c   (DOCA 3.x, BF3, future)
 *
 * Usage: callers include comch_api.h and link against this object.
 *
 * To migrate to BF3: change COMCH_NIC_DOCA_VER=31 in the Makefile and
 * fill in comch_nic_doca31.c — no other files need to change.
 */

#include "../comch_api.h"

#if COMCH_NIC_DOCA_VER >= 30
#  include "comch_nic_doca31.c"
#else
#  include "comch_nic_doca15.c"   /* Default: DOCA 1.5 for BF2 */
#endif
