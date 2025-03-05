#include "core/stdafx.h"

#include "B2SPlayer.h"
#include "../collections/ControlCollection.h"

namespace B2S
{

void B2SPlayer::Add(int playerno) { (*this)[playerno] = new ControlCollection(); }

}