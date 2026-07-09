#ifndef GTEINVERSE_H
#define GTEINVERSE_H

#include "CaptureTypes.h"

/** Approximate inverse of PS1 GTE screen projection (#422). */
namespace GteInverse {

bool screenToModel(const MatrixRecord &matrix, int sx, int sy, int sz, float &mx, float &my, float &mz);

/** Float variant for PGXP subpixel screen coords + view-space depth (#816).
 *  The int overload delegates here so both share one implementation. */
bool screenToModel(const MatrixRecord &matrix, float sx, float sy, float sz, float &mx, float &my,
                   float &mz);

/** Forward projection paired with screenToModel (fixed-point model units, PS1 screen coords). */
bool modelToScreen(const MatrixRecord &matrix, int mx, int my, int mz, int &sx, int &sy, int &sz);

/** PS1 screen (Y-down) → editor world (Y-up, right-handed). */
void psxScreenToWorld(float sx, float sy, float sz, float &wx, float &wy, float &wz);

/** GTE model space → editor world (handedness flip only, no screen re-centering). */
void modelToEditor(float mx, float my, float mz, float &wx, float &wy, float &wz);

} // namespace GteInverse

#endif // GTEINVERSE_H
