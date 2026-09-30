//
// RT64
//

#include "rt64_gbi_l3d.h"
#include "rt64_gbi_f3d.h"

namespace RT64 {
    namespace GBI_L3D {
        void line3D(State *state, DisplayList **dl) {
            state->rsp->drawLine3D((*dl)->p1(16, 8) / 10, (*dl)->p1(8, 8) / 10, (*dl)->p1(0, 8));
        }

        void setup(GBI *gbi) {
            GBI_F3D::setup(gbi);

            gbi->map[L3D_G_LINE3D] = &line3D;
        }
    }
}
