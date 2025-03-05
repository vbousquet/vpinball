#pragma once

#include <map>

namespace B2S
{

class ControlCollection;

class B2SPlayer : public std::map<int, ControlCollection*>
{
public:
   void Add(int playerno);
};

}