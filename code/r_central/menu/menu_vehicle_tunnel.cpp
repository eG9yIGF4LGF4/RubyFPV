/*
    Ruby Licence
    Copyright (c) 2020-2025 Petru Soroaga petrusoroaga@yahoo.com
    All rights reserved.

    Redistribution and/or use in source and/or binary forms, with or without
    modification, are permitted provided that the following conditions are met:
        * Redistributions and/or use of the source code (partially or complete) must retain
        the above copyright notice, this list of conditions and the following disclaimer
        in the documentation and/or other materials provided with the distribution.
        * Redistributions in binary form (partially or complete) must reproduce
        the above copyright notice, this list of conditions and the following disclaimer
        in the documentation and/or other materials provided with the distribution.
        * Copyright info and developer info must be preserved as is in the user
        interface, additions could be made to that info.
        * Neither the name of the organization nor the
        names of its contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.
        * Military use is not permitted.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE AUTHOR (PETRU SOROAGA) BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "menu.h"
#include "menu_objects.h"
#include "menu_vehicle_tunnel.h"
#include "menu_text.h"
#include "menu_item_section.h"
#include "menu_item_legend.h"
#include "menu_item_text.h"
#include "menu_confirmation.h"
#include "menu_radio_config.h"
#include "menu_tx_raw_power.h"
#include "../../base/ctrl_settings.h"
#include "../../base/tx_powers.h"
#include "../../utils/utils_controller.h"

#include <time.h>
#include <sys/resource.h>

MenuVehicleTunnel::MenuVehicleTunnel(void)
:Menu(MENU_ID_CONTROLLER_RADIO, L("IP Tunnel Settings"), NULL)
{
   m_Width = 0.38;
   m_xPos = menu_get_XStartPos(m_Width); m_yPos = 0.18;
}

void MenuVehicleTunnel::onShow()
{
    int iTmp = getSelectedMenuItemIndex();
    Menu::onShow();
    addItems();
    invalidate();

    m_SelectedIndex = iTmp;
    if ( m_SelectedIndex < 0 )
      m_SelectedIndex = 0;
    if ( m_SelectedIndex >= m_ItemsCount )
      m_SelectedIndex = m_ItemsCount-1;
}


void MenuVehicleTunnel::addItems()
{
    int iTmp = getSelectedMenuItemIndex();
    removeAllItems();

    ControllerSettings* pCS = get_ControllerSettings();

    m_pItemsSelect[0] = new MenuItemSelect(L("IP Tunnel Mode"), L("Sets the TCP/IP based communication between vehicle and controller on the controller side."));
    m_pItemsSelect[0]->addSelection(L("Disabled"));
    m_pItemsSelect[0]->addSelection(L("Enabled"));
    m_pItemsSelect[0]->setIsEditable();
    m_pItemsSelect[0]->setSelectedIndex(pCS->iTunnel);
    m_pItemsSelect[0]->setExtraHeight(0.2*g_pRenderEngine->textHeight(g_idFontMenu));
    m_IndexTxPowerMode = addMenuItem(m_pItemsSelect[0]);
   
    m_pMenuItems[m_ItemsCount-1]->setExtraHeight(0.4*g_pRenderEngine->textHeight(g_idFontMenu));
}

void MenuVehicleTunnel::valuesToUI()
{
   addItems();
}

void MenuVehicleTunnel::Render()
{
   RenderPrepare();
   float yTop = RenderFrameAndTitle();
   float y = yTop;

   for( int i=0; i<m_ItemsCount; i++ )
      y += RenderItem(i,y);
   RenderEnd(yTop);
}

bool MenuVehicleTunnel::periodicLoop()
{
   return false;
}

void MenuVehicleTunnel::onReturnFromChild(int iChildMenuId, int returnValue)
{
   Menu::onReturnFromChild(iChildMenuId, returnValue);
   valuesToUI();
}

void MenuVehicleTunnel::onSelectItem()
{
//    Menu::onSelectItem();
//    if ( (-1 == m_SelectedIndex) || (m_pMenuItems[m_SelectedIndex]->isEditing()) )
//       return;


//    ControllerSettings* pCS = get_ControllerSettings();
//    if ( NULL == pCS )
//    {
//       log_softerror_and_alarm("Failed to get pointer to controller settings structure");
//       return;
//    }

//    if ( m_IndexRadioConfig == m_SelectedIndex )
//    {
//       MenuRadioConfig* pM = new MenuRadioConfig();
//       add_menu_to_stack(pM);
//       return;
//    }

//    if ( m_IndexTxPowerMode == m_SelectedIndex )
//    {
//       ControllerSettings* pCS = get_ControllerSettings();
//       pCS->iFixedTxPower = 1 - m_pItemsSelect[0]->getSelectedIndex();
//       save_ControllerSettings();
//       send_model_changed_message_to_router(MODEL_CHANGED_RADIO_POWERS, 0);
//       valuesToUI();
//       return;
//    }

  
//    if ( (-1 != m_IndexTxPowerSingle) && (m_IndexTxPowerSingle == m_SelectedIndex) )
//    {
//       ControllerSettings* pCS = get_ControllerSettings();
//       int iIndex = m_pItemsSelect[1]->getSelectedIndex();
//       if ( iIndex == m_pItemsSelect[1]->getSelectionsCount() -1 )
//       {
//          MenuTXRawPower* pMenu = new MenuTXRawPower();
//          pMenu->m_bShowVehicleSide = false;
//          add_menu_to_stack(pMenu);
//          return;
//       }
      
//       pCS->iFixedTxPower = 1;

//       int iPowerLevelsCount = 0;
//       const int* piPowerLevelsMw = tx_powers_get_ui_levels_mw(&iPowerLevelsCount);
//       int iPowerMwToSet = piPowerLevelsMw[iIndex];
//       log_line("MenuControllerTunnel: Setting all cards mw tx power to: %d mw", iPowerMwToSet);
//       for( int i=0; i<hardware_get_radio_interfaces_count(); i++ )
//       {
//          if ( ! hardware_radio_index_is_wifi_radio(i) )
//             continue;
//          if ( hardware_radio_index_is_sik_radio(i) )
//             continue;
//          radio_hw_info_t* pRadioHWInfo = hardware_get_radio_info(i);
//          if ( (! pRadioHWInfo->isConfigurable) || (! pRadioHWInfo->isSupported) )
//             continue;

//          t_ControllerRadioInterfaceInfo* pCRII = controllerGetRadioCardInfo(pRadioHWInfo->szMAC);
//          if ( NULL == pCRII )
//             continue;

//          int iCardModel = pCRII->cardModel;

//          int iCardMaxPowerMw = tx_powers_get_max_usable_power_mw_for_card(hardware_getBoardType(), iCardModel);
//          int iCardNewPowerMw = iPowerMwToSet;
//          if ( iCardNewPowerMw > iCardMaxPowerMw )
//             iCardNewPowerMw = iCardMaxPowerMw;
//          int iTxPowerRawToSet = tx_powers_convert_mw_to_raw(hardware_getBoardType(), iCardModel, iCardNewPowerMw);
//          log_line("MenuControllerTunnel: Setting tx raw power for card %d from %d to: %d (%d mw out of max %d mw for this card)",
//             i+1, pCRII->iRawPowerLevel, iTxPowerRawToSet, iCardNewPowerMw, iCardMaxPowerMw);
//          pCRII->iRawPowerLevel = iTxPowerRawToSet;
//       }
//       save_ControllerSettings();
//       save_ControllerInterfacesSettings();
//       send_model_changed_message_to_router(MODEL_CHANGED_RADIO_POWERS, 0);
//       valuesToUI();
//       return;
//    }
}

