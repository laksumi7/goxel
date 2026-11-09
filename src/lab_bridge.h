/* Goxel 3D voxels editor
 *
 * AMFM #532 A3-P0 / A3-P2 (under #534): a narrow, session-safe local control
 * channel so an external tool can read the live document and make one bounded
 * edit that goes through the normal undo history.
 *
 * Upstream Goxel has no live bridge at all (there is no socket or rpc code in
 * the tree), so this is the LAB-local adapter, not an enablement of existing
 * code.  It is deliberately not a remote-control server:
 *   - off unless GOXEL_LAB_PORT is set,
 *   - binds 127.0.0.1 only,
 *   - one client at a time, non-blocking, a little work per frame,
 *   - exposes only read / one-voxel edit / undo / redo,
 *   - every edit goes through image_history_push(), so Ctrl+Z reverts it,
 *   - it refuses to edit while a shape session is open or while the active
 *     layer is a generated-shape layer, so it cannot fight the manual flow.
 */

#ifndef LAB_BRIDGE_H
#define LAB_BRIDGE_H

#include <stddef.h>

// Start listening on 127.0.0.1:<port>.  A non-positive port disables it.
void lab_bridge_init(int port, const char *doc_path);

// Called once per frame.  Never blocks.
void lab_bridge_iter(void);

// AMFM #541 test seam: run one bridge line in-process and copy the reply.
// Used only by the headless navigator self-test.
void lab_bridge_test_call(const char *line, char *out, size_t out_size);

#endif // LAB_BRIDGE_H
