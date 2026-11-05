/* Goxel 3D voxels editor
 *
 * copyright (c) 2019 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef IMAGE_H
#define IMAGE_H

#include "camera.h"
#include "layer.h"
#include "material.h"

#include <stdint.h>
#include <stdbool.h>

typedef struct history history_t;

typedef struct image image_t;
struct image {
    int     ref;
    layer_t *layers;
    layer_t *active_layer;

    camera_t *cameras;
    camera_t *active_camera;

    material_t *materials;
    material_t *active_material;

    float    box[4][4];

    float       selection_box[4][4];
    volume_t    *selection_mask;

    // For saving.
    // XXX: I think those should be persistend data of export code instead.
    char     *path;         // Path to save the gox file.
    char     *export_path;  // Path to export.
    const char *export_fmt; // Format of the export_path.
    bool     export_custom_size;
    int      export_width;
    int      export_height;
    bool     export_transparent_background;
    uint32_t saved_key;     // image_get_key() value of saved file.

    // Undo history.
    image_t *history;
    image_t *history_next, *history_prev;
    image_t *history_pos; // Point to the current position in the history.
};

image_t *image_new(void);
void image_delete(image_t *img);
layer_t *image_add_layer(image_t *img, layer_t *layer);
void image_delete_layer(image_t *img, layer_t *layer);
layer_t *image_duplicate_layer(image_t *img, layer_t *layer);
void image_merge_visible_layers(image_t *img);
void image_merge_layer_down(image_t *img, layer_t *layer);

void image_history_push(image_t *img);
void image_undo(image_t *img);
void image_redo(image_t *img);
void image_history_resize(image_t *img, int size);

bool image_layer_can_edit(const image_t *img, const layer_t *layer);

/*
 * AMFM #534 P0: a selection-driven move that treats everything inside the
 * selection as one authoring object.
 *
 * Goxel's Move tool used to translate the active layer only, so an assembly
 * that spans several layers — a PD turret whose body is on the ship layer and
 * whose smooth barrels are their own generated-surface layers — came apart: the
 * body moved and the generated surfaces stayed behind.  These calls snapshot
 * every layer holding cells in the mask, and each apply translates all of them
 * by the same transform, including the region a generated surface is built in.
 *
 * begin/apply/end mirrors how the tool drags: snapshot once at BEGIN, re-apply
 * from the snapshot on every frame, release at END (one history entry).
 */
typedef struct image_move image_move_t;
image_move_t *image_move_begin(image_t *img, const volume_t *mask);
void          image_move_apply(image_move_t *mv, const float mat[4][4]);
void          image_move_end(image_move_t *mv);
// True if a generated surface was only partly inside the selection.
bool          image_move_partial(const image_move_t *mv);
// How many layers the grab took hold of.
int           image_move_layer_count(const image_move_t *mv);

material_t *image_add_material(image_t *img, material_t *mat);
void image_delete_material(image_t *img, material_t *mat);

camera_t *image_add_camera(image_t *img, camera_t *cam);
void image_delete_camera(image_t *img, camera_t *cam);

void image_auto_resize(image_t *img);

/*
 * Function: image_get_key
 * Return a value that is guarantied to change when the image change.
 */
uint32_t image_get_key(const image_t *img);

/*
 * Function: image_is_empty
 * Return true if the image only contains empty layers.
 */
bool image_is_empty(const image_t *img);

#endif // IMAGE_H
