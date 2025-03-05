#include "core/stdafx.h"

#include "IlluminationGroupCollection.h"
#include "../controls/B2SPictureBox.h"

namespace B2S
{

void IlluminationGroupCollection::Add(B2SPictureBox* pPicbox)
{
   if (!pPicbox->GetGroupName().empty())
      (*this)[pPicbox->GetGroupName()].push_back(pPicbox);
}

}