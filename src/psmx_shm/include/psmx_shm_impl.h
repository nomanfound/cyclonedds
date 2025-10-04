// Copyright(c) 2025 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

#ifndef PSMX_SHM_IMPL_H
#define PSMX_SHM_IMPL_H

#include "dds/ddsc/dds_psmx.h"
#include "dds/ddsrt/retcode.h"

#if defined (__cplusplus)
extern "C" {
#endif

/**
 * @brief Create a PSMX instance for shared memory transport
 *
 * @param[out] psmx_out     Pointer to store the created PSMX instance
 * @param[in]  instance_id  Unique identifier for this PSMX instance
 * @param[in]  config       Configuration string (optional)
 * @return DDS_RETCODE_OK on success, error code otherwise
 */
dds_return_t shm_create_psmx(
  dds_psmx_t **psmx_out,
  dds_psmx_instance_id_t instance_id,
  const char *config
);

#if defined (__cplusplus)
}
#endif

#endif /* PSMX_SHM_IMPL_H */
