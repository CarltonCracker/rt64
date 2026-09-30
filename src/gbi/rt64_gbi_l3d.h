//
// RT64
//

#pragma once

#include "rt64_gbi.h"

#define L3D_G_LINE3D (F3D_G_TRI1 - 10)

namespace RT64 {
    namespace GBI_L3D {
        void line3D(State *state, DisplayList **dl);

        void setup(GBI *gbi);
    }
}