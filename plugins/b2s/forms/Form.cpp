#include "core/stdafx.h"

#include "Form.h"
#include "FormWindow.h"

namespace B2S
{

Form::Form()
{
   m_topMost = false;
   m_pWindow = nullptr;
   //m_pGraphics = nullptr;
}

Form::~Form() { delete m_pWindow; }

void Form::Show()
{
   //if (m_pWindow)
   //   m_pWindow->Show();
}

void Form::Hide()
{
   /* if (m_pWindow)
      m_pWindow->Hide();*/
}

void Form::Render()
{
   /* if (!m_pGraphics || !IsInvalidated())
      return false;

   m_pGraphics->SetColor(RGB(0, 0, 0));
   m_pGraphics->Clear();
   m_pGraphics->SetBlendMode(SDL_BLENDMODE_BLEND);
   OnPaint(m_pGraphics);

   return true; */
}

}