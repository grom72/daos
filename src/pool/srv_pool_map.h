/**
 * (C) Copyright 2021 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 */

#ifndef DAOS_SRV_POOL_MAP_H
#define DAOS_SRV_POOL_MAP_H
int
ds_pool_map_tgts_update(uuid_t pool_uuid, struct pool_map *map, struct pool_target_id_list *tgts,
			int opc, bool evict_rank, uint32_t *tgt_map_ver,
			bool print_changes);
#endif /* DAOS_SRV_POOL_MAP_H */
