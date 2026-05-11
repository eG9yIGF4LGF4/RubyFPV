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

#include <pthread.h>
#include "packets_utils.h"
#include "../base/base.h"
#include "../base/config.h"
#include "../base/flags.h"
#include "../base/encr.h"
#include "../base/commands.h"
#include "../base/hw_procs.h"
#include "../base/tx_powers.h"
#include "../base/radio_utils.h"
#include "../common/radio_stats.h"
#include "../common/string_utils.h"
#include "ruby_rt_vehicle.h"
#include "shared_vars.h"
#include "timers.h"
#include "processor_tx_video.h"
#include "test_link_params.h"
#include "adaptive_video.h"
#include "negociate_radio.h"

#include "../radio/radiopackets2.h"
#include "../radio/radiolink.h"
#include "../radio/udplink.h"
#include "../radio/radio_tx.h"

u8 s_RadioRawPacket[MAX_PACKET_TOTAL_SIZE];

u32 s_StreamsTxPacketIndex[MAX_RADIO_STREAMS];

u32 s_uPacketsAdaptiveVideoBitrateBPS = 0;
int s_LastTxDataRatesVideo[MAX_RADIO_INTERFACES];
int s_LastTxDataRatesData[MAX_RADIO_INTERFACES];
int s_LastSetAtherosCardsDatarates[MAX_RADIO_INTERFACES];
int s_iLastRawTxPowerPerRadioInterface[MAX_RADIO_INTERFACES];

u32 s_VehicleLogSegmentIndex = 0;


typedef struct
{
   u32 uIndex; // monotonicaly increasing
   u32 uId;
   u32 uFlags1;
   u32 uFlags2;
   u32 uRepeatCount;
   u32 uStartTime;
} ALIGN_STRUCT_SPEC_INFO t_alarm_info;

#define MAX_ALARMS_QUEUE 20

t_alarm_info s_AlarmsQueue[MAX_ALARMS_QUEUE];
int s_AlarmsPendingInQueue = 0;
u32 s_uAlarmsIndex = 0;
u32 s_uTimeLastAlarmSentToRouter = 0;

void packet_utils_init()
{
   for( int i=0; i<MAX_RADIO_STREAMS; i++ )
      s_StreamsTxPacketIndex[i] = 0;
   for( int i=0; i<MAX_RADIO_INTERFACES; i++ )
   {
      s_LastTxDataRatesVideo[i] = 0;
      s_LastTxDataRatesData[i] = 0;
      s_LastSetAtherosCardsDatarates[i] = 5000;
      s_iLastRawTxPowerPerRadioInterface[i] = 0;
   }
}

void packet_utils_set_adaptive_video_bitrate(u32 uBitrate)
{
   s_uPacketsAdaptiveVideoBitrateBPS = uBitrate;
}

int get_last_tx_power_used_for_radiointerface(int iRadioInterface)
{
   if ( (iRadioInterface >= 0) && (iRadioInterface <= MAX_RADIO_INTERFACES) )
      return s_iLastRawTxPowerPerRadioInterface[iRadioInterface];
   return 0;
}

// Returns the actual datarate bps used last time for data or video
int get_last_tx_used_datarate_bps_video(int iInterface)
{
   return s_LastTxDataRatesVideo[iInterface];
}

int get_last_tx_used_datarate_bps_data(int iInterface)
{
   return s_LastTxDataRatesData[iInterface];
}

// Returns the actual datarate total mbps used last time for video

int get_last_tx_minimum_video_radio_datarate_bps()
{
   u32 nMinRate = MAX_U32;

   for( int i=0; i<g_pCurrentModel->radioInterfacesParams.interfaces_count; i++ )
   {
      if ( g_pCurrentModel->radioInterfacesParams.interface_capabilities_flags[i] & RADIO_HW_CAPABILITY_FLAG_USED_FOR_RELAY )
         continue;
      if ( !( g_pCurrentModel->radioInterfacesParams.interface_capabilities_flags[i] & RADIO_HW_CAPABILITY_FLAG_CAN_USE_FOR_VIDEO) )
         continue;
      if ( s_LastTxDataRatesVideo[i] == 0 )
         continue;
      radio_hw_info_t* pRadioInfo = hardware_get_radio_info(i);
      if ( (NULL == pRadioInfo) || (! pRadioInfo->isHighCapacityInterface) )
         continue;
        
      int iRadioLink = g_SM_RadioStats.radio_interfaces[i].assignedVehicleRadioLinkId;
      if ( iRadioLink >= 0 )
      if ( getRealDataRateFromRadioDataRate(s_LastTxDataRatesVideo[i], g_pCurrentModel->radioLinksParams.link_radio_flags[iRadioLink], 1) < nMinRate )
         nMinRate = getRealDataRateFromRadioDataRate(s_LastTxDataRatesVideo[i], g_pCurrentModel->radioLinksParams.link_radio_flags[iRadioLink], 1);
   }
   if ( nMinRate == MAX_U32 )
      return 0;
   return nMinRate;
}

static pthread_t s_pThreadSetTxPower;
static bool s_bThreadSetTxPowerRunning = false;
static int s_iThreadSetTxPowerInterfaceIndex = -1;
static int s_iThreadSetTxPowerRawValue = 0;

void* _thread_set_tx_power_async(void *argument)
{
   sched_yield();
   s_bThreadSetTxPowerRunning = true;
   log_line("[ThreadTxPower] Start: Set radio interface %d raw tx power to %d", s_iThreadSetTxPowerInterfaceIndex, s_iThreadSetTxPowerRawValue);
   radio_hw_info_t* pRadioHWInfo = hardware_get_radio_info(s_iThreadSetTxPowerInterfaceIndex);
   if ( ! pRadioHWInfo->isConfigurable )
      return NULL;
   if ( hardware_radio_driver_is_rtl8812au_card(pRadioHWInfo->iRadioDriver) )
      hardware_radio_set_txpower_raw_rtl8812au(s_iThreadSetTxPowerInterfaceIndex, s_iThreadSetTxPowerRawValue);
   if ( hardware_radio_driver_is_rtl8812eu_card(pRadioHWInfo->iRadioDriver) )
      hardware_radio_set_txpower_raw_rtl8812eu(s_iThreadSetTxPowerInterfaceIndex, s_iThreadSetTxPowerRawValue);
   if ( hardware_radio_driver_is_rtl8733bu_card(pRadioHWInfo->iRadioDriver) )
      hardware_radio_set_txpower_raw_rtl8733bu(s_iThreadSetTxPowerInterfaceIndex, s_iThreadSetTxPowerRawValue);
   if ( hardware_radio_driver_is_atheros_card(pRadioHWInfo->iRadioDriver) )
      hardware_radio_set_txpower_raw_atheros(s_iThreadSetTxPowerInterfaceIndex, s_iThreadSetTxPowerRawValue);

   log_line("[ThreadTxPower] End: Done setting radio interface %d raw tx power to %d", s_iThreadSetTxPowerInterfaceIndex, s_iThreadSetTxPowerRawValue);
   s_bThreadSetTxPowerRunning = false;
   return NULL;
}

void _compute_packet_tx_power_on_ieee(int iVehicleRadioLinkId, int iRadioInterfaceIndex, int iDataRateTx)
{
   bool bIsInTxPITMode = false;
   g_pCurrentModel->uModelRuntimeStatusFlags &= ~(MODEL_RUNTIME_STATUS_FLAG_IN_PIT_MODE | MODEL_RUNTIME_STATUS_FLAG_IN_PIT_MODE_TEMPERATURE);
   
   // Manual PIT mode?
   if ( g_pCurrentModel->radioInterfacesParams.uFlagsRadioInterfaces & RADIO_INTERFACES_FLAGS_PIT_MODE_ENABLE )
   if ( g_pCurrentModel->radioInterfacesParams.uFlagsRadioInterfaces & RADIO_INTERFACES_FLAGS_PIT_MODE_ENABLE_MANUAL )
   {
      g_pCurrentModel->uModelRuntimeStatusFlags |= MODEL_RUNTIME_STATUS_FLAG_IN_PIT_MODE;
      bIsInTxPITMode = true;
   }

   // Arm/disarm PIT mode?
   if ( g_pCurrentModel->radioInterfacesParams.uFlagsRadioInterfaces & RADIO_INTERFACES_FLAGS_PIT_MODE_ENABLE )
   if ( g_pCurrentModel->radioInterfacesParams.uFlagsRadioInterfaces & RADIO_INTERFACES_FLAGS_PIT_MODE_ENABLE_ARMDISARM )
   if ( ! g_bVehicleArmed )
   {
      g_pCurrentModel->uModelRuntimeStatusFlags |= MODEL_RUNTIME_STATUS_FLAG_IN_PIT_MODE;
      bIsInTxPITMode = true;
   }

   // Temperature PIT mode?
   if ( g_pCurrentModel->radioInterfacesParams.uFlagsRadioInterfaces & RADIO_INTERFACES_FLAGS_PIT_MODE_ENABLE )
   if ( g_pCurrentModel->radioInterfacesParams.uFlagsRadioInterfaces & RADIO_INTERFACES_FLAGS_PIT_MODE_ENABLE_TEMP )
   {
      static bool s_bTemperatureTriggeredCutoff = false;

      int iTempThresholdC = (g_pCurrentModel->hwCapabilities.uHWFlags & 0xFF00) >> 8;

      if ( ! s_bTemperatureTriggeredCutoff )
      if ( g_iVehicleSOCTemperatureC >= iTempThresholdC )
         s_bTemperatureTriggeredCutoff = true;

      if ( s_bTemperatureTriggeredCutoff )
      if ( g_iVehicleSOCTemperatureC < iTempThresholdC - 1 )
         s_bTemperatureTriggeredCutoff = false;

      if ( s_bTemperatureTriggeredCutoff )
      {
         g_pCurrentModel->uModelRuntimeStatusFlags |= MODEL_RUNTIME_STATUS_FLAG_IN_PIT_MODE_TEMPERATURE;
         bIsInTxPITMode = true;
      }
   }

   if ( (iRadioInterfaceIndex < 0) || (iRadioInterfaceIndex >= g_pCurrentModel->radioInterfacesParams.interfaces_count) )
      return;


   int iRadioInterfacelModel = g_pCurrentModel->radioInterfacesParams.interface_card_model[iRadioInterfaceIndex];
   if ( iRadioInterfacelModel < 0 )
      iRadioInterfacelModel = -iRadioInterfacelModel;
   //int iRadioInterfacePowerMaxRaw = tx_powers_get_max_usable_power_raw_for_card(g_pCurrentModel->hwCapabilities.uBoardType, iRadioInterfacelModel);
   //int iRadioInterfacePowerMaxMw = tx_powers_get_max_usable_power_mw_for_card(g_pCurrentModel->hwCapabilities.uBoardType, iRadioInterfacelModel);
   //int iRadioInterfaceTxPowerMw = tx_powers_convert_raw_to_mw(g_pCurrentModel->hwCapabilities.uBoardType, iRadioInterfacelModel, iRadioInterfaceRawTxPower);

   int iRadioInterfaceRawTxPowerToUse = g_pCurrentModel->radioInterfacesParams.interface_raw_power[iRadioInterfaceIndex];
   if ( bIsInTxPITMode )
      iRadioInterfaceRawTxPowerToUse = tx_powers_convert_mw_to_raw(g_pCurrentModel->hwCapabilities.uBoardType, iRadioInterfacelModel, 5);

   if ( negociate_radio_link_is_in_progress() )
   {
      int iTxTestPowerMw = negociate_radio_link_get_txpower_mw();
      if ( iTxTestPowerMw > 0 )
         iRadioInterfaceRawTxPowerToUse = tx_powers_convert_mw_to_raw(g_pCurrentModel->hwCapabilities.uBoardType, iRadioInterfacelModel, iTxTestPowerMw);
   }

   if ( iRadioInterfaceRawTxPowerToUse == s_iLastRawTxPowerPerRadioInterface[iRadioInterfaceIndex] )
      return;

   radio_hw_info_t* pRadioHWInfo = hardware_get_radio_info(iRadioInterfaceIndex);
   if ( ! pRadioHWInfo->isConfigurable )
      return;

   if ( s_bThreadSetTxPowerRunning )
      return;

   s_iLastRawTxPowerPerRadioInterface[iRadioInterfaceIndex] = iRadioInterfaceRawTxPowerToUse;


   s_bThreadSetTxPowerRunning = true;
   s_iThreadSetTxPowerInterfaceIndex = iRadioInterfaceIndex;
   s_iThreadSetTxPowerRawValue = iRadioInterfaceRawTxPowerToUse;

   pthread_attr_t attr;
   hw_init_worker_thread_attrs(&attr);
   if ( 0 != pthread_create(&s_pThreadSetTxPower, &attr, &_thread_set_tx_power_async, NULL) )
   {
      pthread_attr_destroy(&attr);
      s_bThreadSetTxPowerRunning = false;
      return;
   }
   pthread_attr_destroy(&attr);
}


int _compute_packet_downlink_datarate_radioflags_tx_power(u8* pPacketData, int iVehicleRadioLink, int iRadioInterfaceIndex)
{
   // Compute radio flags, datarate then tx power
   // If negociate radio is in progress, use those values

   radio_hw_info_t* pRadioHWInfo = hardware_get_radio_info(iRadioInterfaceIndex);
   if ( NULL == pRadioHWInfo )
      return DEFAULT_RADIO_DATARATE_LOWEST;
   if ( ! pRadioHWInfo->isConfigurable )
      return DEFAULT_RADIO_DATARATE_LOWEST;

   if ( (NULL == g_pCurrentModel) || (NULL == pPacketData) )
      return DEFAULT_RADIO_DATARATE_LOWEST;

   t_packet_header* pPH = (t_packet_header*)pPacketData;

   bool bUseLowest = false;
   if ( (pPH->packet_type == PACKET_TYPE_NEGOCIATE_RADIO_LINKS) ||
        (pPH->packet_type == PACKET_TYPE_RUBY_PAIRING_REQUEST) ||
        (pPH->packet_type == PACKET_TYPE_RUBY_PAIRING_CONFIRMATION) )
      bUseLowest = true;

   if ( test_link_is_in_progress() )
   if ( (pPH->packet_flags & PACKET_FLAGS_MASK_MODULE) != PACKET_COMPONENT_VIDEO )
      bUseLowest = true;

   bool bIsVideoPacket = false;
   bool bIsAudioPacket = false;
   if ( (pPH->packet_flags & PACKET_FLAGS_MASK_MODULE) == PACKET_COMPONENT_AUDIO )
      bIsAudioPacket = true;
   if ( ((pPH->stream_packet_idx) >> PACKET_FLAGS_MASK_SHIFT_STREAM_INDEX) >= STREAM_ID_VIDEO_1 )
      bIsVideoPacket = true;

   //--------------------------------------------
   // Radio flags - begin

   u32 uRadioFlags = g_pCurrentModel->radioLinksParams.link_radio_flags[iVehicleRadioLink];
   if ( test_link_is_in_progress() )
   {
      type_radio_links_parameters* pRP = test_link_get_temp_radio_params();
      uRadioFlags = pRP->link_radio_flags[iVehicleRadioLink];
   }
   if ( (! bUseLowest) && negociate_radio_link_is_in_progress() && bIsVideoPacket )
      uRadioFlags = negociate_radio_link_get_radio_flags();

   radio_set_frames_flags(uRadioFlags, g_TimeNow);
   
   // Radio flags - end


   //--------------------------------------------
   // Datarates - begin

   int iDataRateTx = g_pCurrentModel->radioLinksParams.downlink_datarate_data_bps[iVehicleRadioLink];
   if ( bIsVideoPacket || bIsAudioPacket )
      iDataRateTx = g_pCurrentModel->radioLinksParams.downlink_datarate_video_bps[iVehicleRadioLink];

   if ( test_link_is_in_progress() )
   {
      type_radio_links_parameters* pRP = test_link_get_temp_radio_params();
      iDataRateTx = pRP->downlink_datarate_data_bps[iVehicleRadioLink];
      if ( bIsVideoPacket || bIsAudioPacket )
         iDataRateTx = pRP->downlink_datarate_video_bps[iVehicleRadioLink];
   }

   if ( (! bUseLowest) && negociate_radio_link_is_in_progress() && bIsVideoPacket )
   {
      int iRate = negociate_radio_link_get_data_rate();
      if ( (iRate != 0) && (iRate != -100) )
         iDataRateTx = iRate;
   }

   if ( 0 == iDataRateTx )
   if ( (!bIsVideoPacket) && (!bIsAudioPacket) )
   {
      iDataRateTx = DEFAULT_RADIO_DATARATE_LOWEST;
      if ( uRadioFlags & RADIO_FLAGS_USE_MCS_DATARATES )
         iDataRateTx = -1;
   }
   if ( 0 == iDataRateTx )
   if ( bIsVideoPacket || bIsAudioPacket )
   {
      iDataRateTx = DEFAULT_RADIO_DATARATE_LOWEST;
      if ( uRadioFlags & RADIO_FLAGS_USE_MCS_DATARATES )
         iDataRateTx = -1;

      if ( 0 != g_pCurrentModel->radioLinksParams.downlink_datarate_video_bps[iVehicleRadioLink] )
         iDataRateTx = g_pCurrentModel->radioLinksParams.downlink_datarate_video_bps[iVehicleRadioLink];
      else
      {
         u32 uCurrentVideoBitrate = g_pCurrentModel->video_link_profiles[g_pCurrentModel->video_params.iCurrentVideoProfile].bitrate_fixed_bps;
         if ( 0 != s_uPacketsAdaptiveVideoBitrateBPS )
            uCurrentVideoBitrate = s_uPacketsAdaptiveVideoBitrateBPS;
         int iDataRateForVideo = g_pCurrentModel->getRadioDataRateForVideoBitrate(uCurrentVideoBitrate, iVehicleRadioLink);

         if ( 0 != adaptive_video_get_current_dr_boost() )
         {
            if ( iDataRateForVideo < 0 )
            {
               iDataRateForVideo -= adaptive_video_get_current_dr_boost();
               if ( iDataRateForVideo < -MAX_MCS_INDEX-1 )
                  iDataRateForVideo = -MAX_MCS_INDEX-1;
            }
            else
            {
               for( int i=0; i<getDataRatesCount(); i++ )
               {
                  if ( getDataRatesBPS()[i] == iDataRateForVideo )
                  {
                     int k = i + adaptive_video_get_current_dr_boost();
                     if ( k >= getDataRatesCount() )
                        k = getDataRatesCount() - 1;
                     iDataRateForVideo = getDataRatesBPS()[k];
                     break;
                  }
               }
            }
         }
         if ( (g_pCurrentModel->video_link_profiles[g_pCurrentModel->video_params.iCurrentVideoProfile].uProfileEncodingFlags) & VIDEO_PROFILE_ENCODING_FLAG_USE_MEDIUM_ADAPTIVE_VIDEO )
         {
            if ( (iDataRateForVideo < 0) && (iDataRateForVideo == -1) )
               iDataRateForVideo = -2;
            if ( (iDataRateForVideo > 0) && (iDataRateForVideo < getDataRatesBPS()[1]) )
               iDataRateForVideo = getDataRatesBPS()[1];
         }

         if ( iDataRateForVideo != 0 )
            iDataRateTx = iDataRateForVideo;
      }
   }
   if ( (-100 == iDataRateTx) || bUseLowest )
   {
      if ( g_pCurrentModel->radioLinksParams.link_radio_flags[iVehicleRadioLink] & RADIO_FLAGS_USE_MCS_DATARATES )
         iDataRateTx = -1;
      else
         iDataRateTx = DEFAULT_RADIO_DATARATE_LOWEST;
   }

   radio_set_out_datarate(iDataRateTx, pPH->packet_type, g_TimeNow);

   if ( (pRadioHWInfo->iRadioType == RADIO_TYPE_ATHEROS) ||
        (pRadioHWInfo->iRadioType == RADIO_TYPE_RALINK) )
   {
      if ( s_LastSetAtherosCardsDatarates[iRadioInterfaceIndex] != iDataRateTx )
      {
         s_LastSetAtherosCardsDatarates[iRadioInterfaceIndex] = iDataRateTx;
         update_atheros_card_datarate(g_pCurrentModel, iRadioInterfaceIndex, iDataRateTx, g_pProcessStats);
      }
      g_TimeNow = get_current_timestamp_ms();
   }

   if ( bIsVideoPacket || bIsAudioPacket )
      s_LastTxDataRatesVideo[iRadioInterfaceIndex] = iDataRateTx;
   else
      s_LastTxDataRatesData[iRadioInterfaceIndex] = iDataRateTx;
   
   // Datarates - end


   /*
   if ( (!bIsVideoPacket) && (!bIsAudioPacket) )
   {
   char szBuff[128];
   char szBuff2[128];
   str_get_radio_frame_flags_description(uRadioFlags, szBuff);
   str_getDataRateDescription(iDataRateTx, 0, szBuff2);
   log_line("DBG out rate: %s, out flags: %s", szBuff2, szBuff);
   }
   */

   //------------------------------------------
   // Tx power
   _compute_packet_tx_power_on_ieee(iVehicleRadioLink, iRadioInterfaceIndex, iDataRateTx);

   return iDataRateTx;

   // To fix may2025
   /*
   // Don't use lowest unless modulation scheme is not QAM (changes to QAM are slow)
   if ( (nRateTx >= -3) && (nRateTx <= 18000000) )
   if ( (pPH->packet_type == PACKET_TYPE_NEGOCIATE_RADIO_LINKS) ||
        (pPH->packet_type == PACKET_TYPE_COMMAND_RESPONSE) ||
        (pPH->packet_type == PACKET_TYPE_COMMAND) ||
        (pPH->packet_type == PACKET_TYPE_RUBY_PAIRING_REQUEST) ||
        (pPH->packet_type == PACKET_TYPE_RUBY_PAIRING_CONFIRMATION) )
   {
      // Use lowest datarate for these packets.
      if ( g_pCurrentModel->radioLinksParams.downlink_datarate_video_bps[iVehicleRadioLinkId] > 0 )
         nRateTx = DEFAULT_RADIO_DATARATE_LOWEST;
      else
         nRateTx = -1;
   }
   */
}


bool _send_packet_to_serial_radio_interface(int iLocalRadioLinkId, int iRadioInterfaceIndex, u8* pPacketData, int nPacketLength)
{
   if ( (NULL == pPacketData) || (nPacketLength <= 0) || (NULL == g_pCurrentModel) )
      return false;
   
   radio_hw_info_t* pRadioHWInfo = hardware_get_radio_info(iRadioInterfaceIndex);
   if ( NULL == pRadioHWInfo )
      return false;

   int iVehicleRadioLinkId = g_SM_RadioStats.radio_links[iLocalRadioLinkId].matchingVehicleRadioLinkId;
   if ( (iVehicleRadioLinkId < 0) || (iVehicleRadioLinkId >= g_pCurrentModel->radioLinksParams.links_count) )
      return false;
   
   // Do not send packet if the link is overloaded
   int iAirRate = g_pCurrentModel->radioLinksParams.downlink_datarate_data_bps[iVehicleRadioLinkId] /8;
   if ( hardware_radio_index_is_sik_radio(iRadioInterfaceIndex) )
      iAirRate = hardware_radio_sik_get_air_baudrate_in_bytes(iRadioInterfaceIndex);

   s_LastTxDataRatesData[iRadioInterfaceIndex] = iAirRate*8;
  
   t_packet_header* pPH = (t_packet_header*)pPacketData;
   if ( ! radio_can_send_packet_on_slow_link(iLocalRadioLinkId, pPH->packet_type, 0, g_TimeNow) )
      return false;

   if ( pPH->total_length > 200 )
      return false;

   if ( iAirRate > 0 )
   if ( g_SM_RadioStats.radio_interfaces[iRadioInterfaceIndex].txBytesPerSec >= (DEFAULT_RADIO_SERIAL_MAX_TX_LOAD * (u32)iAirRate) / 100 )
   {
      static u32 sl_uLastTimeInterfaceTxOverloaded = 0;
      if ( g_TimeNow > sl_uLastTimeInterfaceTxOverloaded + 20000 )
      {
         sl_uLastTimeInterfaceTxOverloaded = g_TimeNow;
         log_line("Radio interface %d is tx overloaded: sending %d bytes/sec and air data rate is %d bytes/sec", iRadioInterfaceIndex+1, (int)g_SM_RadioStats.radio_interfaces[iRadioInterfaceIndex].txBytesPerSec, iAirRate);
         send_alarm_to_controller(ALARM_ID_RADIO_LINK_DATA_OVERLOAD, (g_SM_RadioStats.radio_interfaces[iRadioInterfaceIndex].txBytesPerSec & 0xFFFFFF) | (((u32)iRadioInterfaceIndex)<<24),(u32)iAirRate,0);
      }
      return false;
   }

   if ( (iLocalRadioLinkId < 0) || (iLocalRadioLinkId >= MAX_RADIO_INTERFACES) )
      iLocalRadioLinkId = 0;
   u16 uRadioLinkPacketIndex = radio_get_next_radio_link_packet_index(iLocalRadioLinkId);
   pPH->radio_link_packet_index = uRadioLinkPacketIndex;

   if ( pPH->packet_flags & PACKET_FLAGS_BIT_HEADERS_ONLY_CRC )
      radio_packet_compute_crc((u8*)pPH, sizeof(t_packet_header));
   else
      radio_packet_compute_crc((u8*)pPH, pPH->total_length);

   if ( 1 != pRadioHWInfo->openedForWrite )
   {
      log_softerror_and_alarm("Radio serial interface %d is not opened for write. Can't send packet on it.", iRadioInterfaceIndex+1);
      return false;
   }
      
   u32 microT1 = get_current_timestamp_micros();

   int iWriteResult = radio_tx_send_serial_radio_packet(iRadioInterfaceIndex, (u8*)pPH, pPH->total_length);
   if ( iWriteResult > 0 )
   {
      u32 microT2 = get_current_timestamp_micros();
      if ( microT2 > microT1 )
         g_RadioTxTimers.aTmpInterfacesTxTotalTimeMicros[iRadioInterfaceIndex] += microT2 - microT1;
      
      int iTotalSent = pPH->total_length;
      if ( g_pCurrentModel->radioLinksParams.iSiKPacketSize > 0 )
         iTotalSent += sizeof(t_packet_header_short) * (int) (pPH->total_length / g_pCurrentModel->radioLinksParams.iSiKPacketSize);
      
      u32 uStreamId = (pPH->stream_packet_idx) >> PACKET_FLAGS_MASK_SHIFT_STREAM_INDEX;
      radio_stats_update_on_packet_sent_on_radio_interface(&g_SM_RadioStats, g_TimeNow, iRadioInterfaceIndex, iTotalSent);
      radio_stats_update_on_packet_sent_on_radio_link(&g_SM_RadioStats, g_TimeNow, iLocalRadioLinkId, (int)uStreamId, pPH->total_length);
      return true;
   }
   log_softerror_and_alarm("Failed to write to serial radio interface %d.", iRadioInterfaceIndex+1);
   if ( iWriteResult == -2 )
   if ( hardware_radio_index_is_sik_radio(iRadioInterfaceIndex) )
      flag_reinit_sik_interface(iRadioInterfaceIndex);
   return false;
}

bool _send_packet_to_wifi_radio_interface(int iLocalRadioLinkId, int iRadioInterfaceIndex, u8* pPacketData, int nPacketLength)
{
   if ( (NULL == pPacketData) || (nPacketLength <= 0) || (NULL == g_pCurrentModel) )
      return false;

   radio_hw_info_t* pRadioHWInfo = hardware_get_radio_info(iRadioInterfaceIndex);
   if ( NULL == pRadioHWInfo )
      return false;

   int iVehicleRadioLinkId = g_SM_RadioStats.radio_links[iLocalRadioLinkId].matchingVehicleRadioLinkId;
   if ( (iVehicleRadioLinkId < 0) || (iVehicleRadioLinkId >= g_pCurrentModel->radioLinksParams.links_count) )
      return false;
   
   t_packet_header* pPH = (t_packet_header*)pPacketData;
   u32 uStreamId = (pPH->stream_packet_idx) >> PACKET_FLAGS_MASK_SHIFT_STREAM_INDEX;

   bool bIsVideoPacket = false;
   bool bIsAudioPacket = false;
   if ( (pPH->packet_flags & PACKET_FLAGS_MASK_MODULE) == PACKET_COMPONENT_AUDIO )
      bIsAudioPacket = true;
   if ( uStreamId >= STREAM_ID_VIDEO_1 )
      bIsVideoPacket = true;

   int be = 0;
   if ( g_pCurrentModel->enc_flags != MODEL_ENC_FLAGS_NONE )
   if ( hpp() )
   {
      if ( bIsVideoPacket || bIsAudioPacket)
      if ( (g_pCurrentModel->enc_flags & MODEL_ENC_FLAG_ENC_VIDEO) || (g_pCurrentModel->enc_flags & MODEL_ENC_FLAG_ENC_ALL) )
         be = 1;
      if ( (! bIsVideoPacket) && ( ! bIsAudioPacket) )
      {
         if ( (g_pCurrentModel->enc_flags & MODEL_ENC_FLAG_ENC_BEACON) || (g_pCurrentModel->enc_flags & MODEL_ENC_FLAG_ENC_ALL) )
            be = 1;
         if ( (g_pCurrentModel->enc_flags & MODEL_ENC_FLAG_ENC_DATA) || (g_pCurrentModel->enc_flags & MODEL_ENC_FLAG_ENC_ALL) )
            be = 1;
      }
   }

   // To fix
   /*
   t_packet_header* pPH = (t_packet_header*)pPacketData;
   if ( pPH->packet_type == PACKET_TYPE_VIDEO_DATA_FULL )
   {
      t_packet_header_video_full_77* pPHVF = (t_packet_header_video_full_77*) (pPacketData+sizeof(t_packet_header));
      if ( pPHVF->uVideoStatusFlags2 & VIDEO_STATUS_FLAGS2_HAS_DEBUG_TIMESTAMPS )
      {
         u8* pExtraData = pPacketData + sizeof(t_packet_header) + sizeof(t_packet_header_video_full_77) + pPHVF->video_data_length;
         u32* pExtraDataU32 = (u32*)pExtraData;
         pExtraDataU32[3] = get_current_timestamp_ms();
      }
   }
   */ 

   int iDataRateTx = _compute_packet_downlink_datarate_radioflags_tx_power(pPacketData, iVehicleRadioLinkId, iRadioInterfaceIndex);
   int totalLength = radio_build_new_raw_ieee_packet(iLocalRadioLinkId, s_RadioRawPacket, pPacketData, nPacketLength, RADIO_PORT_ROUTER_DOWNLINK, be);
   u32 microT1 = get_current_timestamp_micros();

   int iRepeatCount = 0;
   if ( pPH->packet_type == PACKET_TYPE_VIDEO_ADAPTIVE_VIDEO_PARAMS_ACK )
      iRepeatCount++;

   /*
   t_packet_header* pPHTmp = (t_packet_header*)(((u8*)&s_RadioRawPacket[0]) + totalLength - nPacketLength);
   if ( pPHTmp->packet_type == PACKET_TYPE_VIDEO_DATA )
   {
      t_packet_header_video_segment* pPHVTmp = (t_packet_header_video_segment*)(((u8*)&s_RadioRawPacket[0]) + totalLength - nPacketLength + sizeof(t_packet_header));
      log_line("DBG r-out video: int: %d, packet: %u, stream: %u, (%u/%u, ec: %d/%d), EOF: %d, delta: %d, has data after: %d", iRadioInterfaceIndex, pPHTmp->radio_link_packet_index, pPHTmp->stream_packet_idx & PACKET_FLAGS_MASK_STREAM_PACKET_IDX,
         pPHVTmp->uCurrentBlockIndex, pPHVTmp->uCurrentBlockPacketIndex,
         pPHVTmp->uCurrentBlockDataPackets, pPHVTmp->uCurrentBlockECPackets,
         (pPHTmp->packet_flags_extended & PACKET_FLAGS_EXTENDED_BIT_END_OF_TRANSMISSION_FRAME)?1:0, pPHTmp->packet_flags_extended & PACKET_FLAGS_EXTENDED_BIT_EOTF_MASK,
         (pPHTmp->packet_flags_extended & PACKET_FLAGS_EXTENDED_BIT_HAS_DATA_AFTER_VIDEO)?1:0);
   }
   else
      log_line("DBG r-out data: int: %d, packet: %u, stream: %u, EOF: %d, delta: %d, has data after: %d, (%s)", iRadioInterfaceIndex, pPHTmp->radio_link_packet_index, pPHTmp->stream_packet_idx & PACKET_FLAGS_MASK_STREAM_PACKET_IDX,
        (pPHTmp->packet_flags_extended & PACKET_FLAGS_EXTENDED_BIT_END_OF_TRANSMISSION_FRAME)?1:0, pPHTmp->packet_flags_extended & PACKET_FLAGS_EXTENDED_BIT_EOTF_MASK, 
        (pPHTmp->packet_flags_extended & PACKET_FLAGS_EXTENDED_BIT_HAS_DATA_AFTER_VIDEO)?1:0,
        str_get_packet_type(pPHTmp->packet_type));
   */

   if ( radio_write_raw_ieee_packet(iRadioInterfaceIndex, s_RadioRawPacket, totalLength, iRepeatCount) )
   {       
      u32 microT2 = get_current_timestamp_micros();
      if ( microT2 > microT1 )
      {
         g_RadioTxTimers.aTmpInterfacesTxTotalTimeMicros[iRadioInterfaceIndex] += microT2 - microT1;
         if ( bIsVideoPacket )
            g_RadioTxTimers.aTmpInterfacesTxVideoTimeMicros[iRadioInterfaceIndex] += microT2 - microT1;
      }
      radio_stats_update_on_packet_sent_on_radio_interface(&g_SM_RadioStats, g_TimeNow, iRadioInterfaceIndex, nPacketLength);
      radio_stats_set_tx_radio_datarate_for_packet(&g_SM_RadioStats, iRadioInterfaceIndex, iLocalRadioLinkId, iDataRateTx, (bIsVideoPacket || bIsAudioPacket)?1:0);

      radio_stats_update_on_packet_sent_on_radio_link(&g_SM_RadioStats, g_TimeNow, iLocalRadioLinkId, (int)uStreamId, nPacketLength);
      return true;
   }

   log_softerror_and_alarm("Failed to write to radio interface %d (type %s, size: %d bytes, raw: %d bytes)",
      iRadioInterfaceIndex+1,
      str_get_packet_type(pPH->packet_type), pPH->total_length, nPacketLength);
   return false;
}

// Sends a radio packet to all posible radio interfaces or just to a single radio link

int send_packet_to_radio_interfaces(u8* pPacketData, int nPacketLength, int iSendToSingleRadioLink)
{
   if ( nPacketLength <= 0 )
      return -1;

   // Set packets indexes and tx times if multiple packets are found in the input buffer

   bool bIsVideoPacket = false;
   bool bIsAudioPacket = false;
   bool bHasCommandParamsZipResponse = false;
   bool bHasZipParamsPacket = false;

   
   int iZipParamsPacketSize = 0;
   u32 uZipParamsUniqueIndex = 0;
   u8  uZipParamsFlags = 0;
   u8  uZipParamsSegmentIndex = 0;
   u8  uZipParamsTotalSegments = 0;
   u8  uPingReplySendOnLocalRadioLinkId = 0xFF;

   bool bIsRetransmited = false;
   bool bIsPingReplyPacket = false;
   bool bIsLowCapacityLinkOnlyPacket = false;
   bool bIsPingPacket = false;
   int iPingOnLocalRadioLinkId = -1;
   
   t_packet_header* pPHZipParams = NULL;
   t_packet_header_command_response* pPHCRZipParams = NULL;
   t_packet_header* pPH = (t_packet_header*)pPacketData;
   
   if ( pPH->packet_flags_extended & PACKET_FLAGS_EXTENDED_BIT_SEND_ON_LOW_CAPACITY_LINK_ONLY )
      bIsLowCapacityLinkOnlyPacket = true;
   if ( pPH->packet_flags & PACKET_FLAGS_BIT_RETRANSMITED )
      bIsRetransmited = true;


   if ( pPH->packet_type == PACKET_TYPE_TEST_RADIO_LINK )
   {
      int iModelRadioLinkId = pPacketData[sizeof(t_packet_header)+2];
      int iCmdType = pPacketData[sizeof(t_packet_header)+4];

      for( int iRadioLinkId=0; iRadioLinkId<g_pCurrentModel->radioLinksParams.links_count; iRadioLinkId++ )
      {
         int iVehicleRadioLinkId = g_SM_RadioStats.radio_links[iRadioLinkId].matchingVehicleRadioLinkId;
         if ( iModelRadioLinkId == iVehicleRadioLinkId )
         if ( iCmdType != PACKET_TYPE_TEST_RADIO_LINK_COMMAND_START )
         if ( iCmdType != PACKET_TYPE_TEST_RADIO_LINK_COMMAND_END )
         {
            iSendToSingleRadioLink = iRadioLinkId;
         }
      }
   }

   if ( (pPH->packet_flags & PACKET_FLAGS_MASK_MODULE) == PACKET_COMPONENT_COMMANDS )
   {
      t_packet_header_command_response* pPHCR = (t_packet_header_command_response*)(pPacketData + sizeof(t_packet_header));
      if ( pPHCR->origin_command_type == COMMAND_ID_GET_ALL_PARAMS_ZIP )
      {
         bHasCommandParamsZipResponse = true;
         pPHZipParams = pPH;
         pPHCRZipParams = pPHCR;
      }
   }

   if ( pPH->packet_type == PACKET_TYPE_RUBY_PING_CLOCK )
   {
      u8 uLocalRadioLinkId = 0;
      memcpy( &uLocalRadioLinkId, pPacketData + sizeof(t_packet_header)+sizeof(u8), sizeof(u8));
      iPingOnLocalRadioLinkId = (int)uLocalRadioLinkId;
      bIsPingPacket = true;
   }

   if ( pPH->packet_type == PACKET_TYPE_RUBY_PING_CLOCK_REPLY )
   {
      bIsPingReplyPacket = true;
      memcpy((u8*)&uPingReplySendOnLocalRadioLinkId, pPacketData + sizeof(t_packet_header) + 2*sizeof(u8) + sizeof(u32), sizeof(u8));
   }

   if ( pPH->packet_type == PACKET_TYPE_RUBY_MODEL_SETTINGS )
   {
      bHasZipParamsPacket = true;

      memcpy((u8*)&uZipParamsUniqueIndex, pPacketData + sizeof(t_packet_header) + sizeof(u32), sizeof(u32));
      memcpy(&uZipParamsFlags, pPacketData + sizeof(t_packet_header) + 2*sizeof(u32), sizeof(u8));

      if ( 0 == uZipParamsFlags )
         iZipParamsPacketSize = pPH->total_length - sizeof(t_packet_header) - 2 * sizeof(u32) - sizeof(u8);
      else
      {
         uZipParamsSegmentIndex = *(pPacketData + sizeof(t_packet_header)+2*sizeof(u32) + sizeof(u8));
         uZipParamsTotalSegments = *(pPacketData + sizeof(t_packet_header)+2*sizeof(u32) + 2*sizeof(u8));
         iZipParamsPacketSize = (int)(*(pPacketData + sizeof(t_packet_header)+2*sizeof(u32) + 3*sizeof(u8)));
      }
   }

   u32 uDestVehicleId = pPH->vehicle_id_dest;      
   u32 uStreamId = (pPH->stream_packet_idx) >> PACKET_FLAGS_MASK_SHIFT_STREAM_INDEX;

   if ( pPH->packet_type != PACKET_TYPE_RUBY_PING_CLOCK )
   if ( pPH->packet_type != PACKET_TYPE_RUBY_PING_CLOCK_REPLY )
      s_StreamsTxPacketIndex[uStreamId]++;
   pPH->stream_packet_idx = (((u32)uStreamId)<<PACKET_FLAGS_MASK_SHIFT_STREAM_INDEX) | (s_StreamsTxPacketIndex[uStreamId] & PACKET_FLAGS_MASK_STREAM_PACKET_IDX);

   if ( (pPH->packet_flags & PACKET_FLAGS_MASK_MODULE) == PACKET_COMPONENT_AUDIO )
      bIsAudioPacket = true;
   if ( uStreamId >= STREAM_ID_VIDEO_1 )
      bIsVideoPacket = true;


   // If Use IP Tunnel enabled, then send the packet to IP Tunnel endpoint 
   if(g_pCurrentModel->enableIPTunnel) {
      radio_process_udp_data_out(pPacketData, nPacketLength);
   }

   // Send packet on all radio links that can send this packet or just to the single radio interface that user wants
   // Exception: Ping reply packet is sent only on the associated radio link for this ping

   bool bPacketSent = false;

   for( int iRadioLinkId=0; iRadioLinkId<g_pCurrentModel->radioLinksParams.links_count; iRadioLinkId++ )
   {
      int iVehicleRadioLinkId = g_SM_RadioStats.radio_links[iRadioLinkId].matchingVehicleRadioLinkId;
      int iRadioInterfaceIndex = -1;
      for( int k=0; k<g_pCurrentModel->radioInterfacesParams.interfaces_count; k++ )
      {
         if ( g_pCurrentModel->radioInterfacesParams.interface_link_id[k] == iVehicleRadioLinkId )
         {
            iRadioInterfaceIndex = k;
            break;
         }
      }
      if ( iRadioInterfaceIndex < 0 )
         continue;

      if ( (-1 != iSendToSingleRadioLink) && (iRadioLinkId != iSendToSingleRadioLink) )
         continue;

      if ( g_pCurrentModel->radioLinksParams.link_capabilities_flags[iVehicleRadioLinkId] & RADIO_HW_CAPABILITY_FLAG_DISABLED )
         continue;

      if ( bIsPingReplyPacket && (uPingReplySendOnLocalRadioLinkId != 0xFF) )
      if ( iRadioLinkId != (int) uPingReplySendOnLocalRadioLinkId )
         continue;

      // Do not send regular packets to controller using relay links
      if ( (g_pCurrentModel->radioLinksParams.link_capabilities_flags[iVehicleRadioLinkId] & RADIO_HW_CAPABILITY_FLAG_USED_FOR_RELAY) ||
           (g_pCurrentModel->relay_params.isRelayEnabledOnRadioLinkId == iVehicleRadioLinkId) )
         continue;

      // Send Ping reply packets only to the assigned radio link
      if ( bIsPingPacket )
      if ( iRadioLinkId != iPingOnLocalRadioLinkId )
         continue;

      if ( !(g_pCurrentModel->radioLinksParams.link_capabilities_flags[iVehicleRadioLinkId] & RADIO_HW_CAPABILITY_FLAG_CAN_TX) )
         continue;

      if ( bIsVideoPacket || bIsAudioPacket )
      if ( ! (g_pCurrentModel->radioLinksParams.link_capabilities_flags[iVehicleRadioLinkId] & RADIO_HW_CAPABILITY_FLAG_CAN_USE_FOR_VIDEO) )
         continue;

      if ( (! bIsVideoPacket) && (! bIsAudioPacket) )
      if ( ! (g_pCurrentModel->radioLinksParams.link_capabilities_flags[iVehicleRadioLinkId] & RADIO_HW_CAPABILITY_FLAG_CAN_USE_FOR_DATA) )
         continue;

      radio_hw_info_t* pRadioHWInfo = hardware_get_radio_info(iRadioInterfaceIndex);
      if ( ! pRadioHWInfo->openedForWrite )
         continue;
      if ( g_pCurrentModel->radioInterfacesParams.interface_capabilities_flags[iRadioInterfaceIndex] & RADIO_HW_CAPABILITY_FLAG_DISABLED )
         continue;
      if ( !(g_pCurrentModel->radioInterfacesParams.interface_capabilities_flags[iRadioInterfaceIndex] & RADIO_HW_CAPABILITY_FLAG_CAN_TX) )
         continue;
      if ( (bIsVideoPacket || bIsAudioPacket) && (!(g_pCurrentModel->radioInterfacesParams.interface_capabilities_flags[iRadioInterfaceIndex] & RADIO_HW_CAPABILITY_FLAG_CAN_USE_FOR_VIDEO)) )
         continue;
      if ( (!bIsVideoPacket) && (!bIsAudioPacket) && (!(g_pCurrentModel->radioInterfacesParams.interface_capabilities_flags[iRadioInterfaceIndex] & RADIO_HW_CAPABILITY_FLAG_CAN_USE_FOR_DATA)) )
         continue;
      
      if ( hardware_radio_index_is_serial_radio(iRadioInterfaceIndex) )
      {
         if ( (! bIsVideoPacket) && (!bIsRetransmited) && (!bIsAudioPacket) )
         if ( _send_packet_to_serial_radio_interface(iRadioLinkId, iRadioInterfaceIndex, pPacketData, nPacketLength) )
         {
            bPacketSent = true;
            if ( bHasZipParamsPacket )
            {
               if ( 0 == uZipParamsFlags )
                  log_line("Sent radio packet: zip params packet (%d bytes) (transfer id %u) as single packet to SiK radio interface %d", iZipParamsPacketSize, uZipParamsUniqueIndex, iRadioInterfaceIndex+1 );  
               else
                  log_line("Sent radio packet: zip params packet (%d bytes) (transfer id %u) as small packets (%u of %u) to SiK radio interface %d", iZipParamsPacketSize, uZipParamsUniqueIndex, uZipParamsSegmentIndex, uZipParamsTotalSegments, iRadioInterfaceIndex+1 );  
            }
         }
      }
      else
      {
         if ( bIsLowCapacityLinkOnlyPacket )
            continue;
         if ( _send_packet_to_wifi_radio_interface(iRadioLinkId, iRadioInterfaceIndex, pPacketData, nPacketLength) )
         {
            bPacketSent = true;
            if ( bHasCommandParamsZipResponse )
            {
               if ( pPHZipParams->total_length > sizeof(t_packet_header) + 200 )
                  log_line("Sent radio packet: model zip params command response (%d bytes) as single zip file to radio interface %d, command index: %d, retry: %d",
                     (int)(pPHZipParams->total_length - sizeof(t_packet_header) - sizeof(t_packet_header_command_response)), 
                     iRadioInterfaceIndex+1,
                     (int)pPHCRZipParams->origin_command_counter, (int)pPHCRZipParams->origin_command_resend_counter );
               else
                  log_line("Sent radio packet: model zip params command response (%d bytes) as small segment (%d of %d, unique id %d) zip file to radio interface %d, command index: %d, retry: %d",
                     (int)(pPHZipParams->total_length - sizeof(t_packet_header) - sizeof(t_packet_header_command_response) - 4*sizeof(u8)),
                     1+(int)(*(((u8*)pPHZipParams) + sizeof(t_packet_header) + sizeof(t_packet_header_command_response) + sizeof(u8))),
                     (int)(*(((u8*)pPHZipParams) + sizeof(t_packet_header) + sizeof(t_packet_header_command_response) + 2*sizeof(u8))),
                     (int)(*(((u8*)pPHZipParams) + sizeof(t_packet_header) + sizeof(t_packet_header_command_response))),
                     iRadioInterfaceIndex+1,
                     (int)pPHCRZipParams->origin_command_counter, (int)pPHCRZipParams->origin_command_resend_counter );
            }
            if ( bHasZipParamsPacket )
            {
               if ( 0 == uZipParamsFlags )
                  log_line("Sent radio packet: zip params packet (%d bytes) (transfer id %u) as single packet to radio interface %d", iZipParamsPacketSize, uZipParamsUniqueIndex, iRadioInterfaceIndex+1 );
               else
                  log_line("Sent radio packet: zip params packet (%d bytes) (transfer id %u) as small packets (%u of %u) to radio interface %d", iZipParamsPacketSize, uZipParamsUniqueIndex, uZipParamsSegmentIndex, uZipParamsTotalSegments, iRadioInterfaceIndex+1 );
            }
         }
      }
   }

   if ( ! bPacketSent )
   {
      if ( bIsLowCapacityLinkOnlyPacket )
         return 0;

      if ( test_link_is_in_progress() )
      if ( 1 == g_pCurrentModel->radioLinksParams.links_count )
      {
         static u32 s_uDebugTimeLastErrorPacketNotSent = 0;
         if ( g_TimeNow > s_uDebugTimeLastErrorPacketNotSent + 100 )
         {
            s_uDebugTimeLastErrorPacketNotSent = g_TimeNow;
            log_line("Packet not sent on tx interface. Test link params is in progress.");
         }
         return 0;
      }
      log_softerror_and_alarm("Packet not sent! No radio interface could send it (%s, type: %d, %s, %d bytes). %d radio links. %s",
         bIsVideoPacket?"video packet":"data packet",
         pPH->packet_type, str_get_packet_type(pPH->packet_type), nPacketLength,
         g_pCurrentModel->radioLinksParams.links_count,
         bIsLowCapacityLinkOnlyPacket?"Low capacity link packet":"No link specific flags (low/high capacity)");
      return 0;
   }

   // Packet sent. Update stats and info

   radio_stats_update_on_packet_sent_for_radio_stream(&g_SM_RadioStats, g_TimeNow, uDestVehicleId, uStreamId, pPH->packet_type, pPH->total_length);

   g_PHVehicleTxStats.tmp_uAverageTxCount++;

   if ( g_PHVehicleTxStats.historyTxPackets[0] < 255 )
      g_PHVehicleTxStats.historyTxPackets[0]++;

   u32 uTxGap = g_TimeNow - g_TimeLastTxPacket;
   g_TimeLastTxPacket = g_TimeNow;

   if ( uTxGap > 254 )
      uTxGap = 254;

   g_PHVehicleTxStats.tmp_uAverageTxSum += uTxGap;

   if ( 0xFF == g_PHVehicleTxStats.historyTxGapMaxMiliseconds[0] )
      g_PHVehicleTxStats.historyTxGapMaxMiliseconds[0] = uTxGap;
   if ( 0xFF == g_PHVehicleTxStats.historyTxGapMinMiliseconds[0] )
      g_PHVehicleTxStats.historyTxGapMinMiliseconds[0] = uTxGap;


   if ( uTxGap > g_PHVehicleTxStats.historyTxGapMaxMiliseconds[0] )
      g_PHVehicleTxStats.historyTxGapMaxMiliseconds[0] = uTxGap;
   if ( uTxGap < g_PHVehicleTxStats.historyTxGapMinMiliseconds[0] )
      g_PHVehicleTxStats.historyTxGapMinMiliseconds[0] = uTxGap;


   // To do / fix: compute now and averages per radio link, not total
     
   if ( g_TimeNow >= g_RadioTxTimers.uTimeLastUpdated + g_RadioTxTimers.uUpdateIntervalMs )
   {
      u32 uDeltaTime = g_TimeNow - g_RadioTxTimers.uTimeLastUpdated;
      g_RadioTxTimers.uTimeLastUpdated = g_TimeNow;
      
      g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondNow = 0;
      g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondNow = 0;
      
      for( int i=0; i<g_pCurrentModel->radioInterfacesParams.interfaces_count; i++ )
      {
         g_RadioTxTimers.aInterfacesTxTotalTimeMilisecPerSecond[i] = g_RadioTxTimers.aTmpInterfacesTxTotalTimeMicros[i] / uDeltaTime;
         g_RadioTxTimers.aTmpInterfacesTxTotalTimeMicros[i] = 0;

         g_RadioTxTimers.aInterfacesTxVideoTimeMilisecPerSecond[i] = g_RadioTxTimers.aTmpInterfacesTxVideoTimeMicros[i] / uDeltaTime;
         g_RadioTxTimers.aTmpInterfacesTxVideoTimeMicros[i] = 0;

         g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondNow += g_RadioTxTimers.aInterfacesTxTotalTimeMilisecPerSecond[i];
         g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondNow += g_RadioTxTimers.aInterfacesTxVideoTimeMilisecPerSecond[i];
      }

      if ( 0 == g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondAverage )
         g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondAverage = g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondNow;
      else if ( g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondNow > g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondAverage )
         g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondAverage = (g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondNow*2)/3 + g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondAverage/3;
      else
         g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondAverage = (g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondNow)/4 + (g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondAverage*3)/4;

      if ( 0 == g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondAverage )
         g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondAverage = g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondNow;
      else if ( g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondNow > g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondAverage )
         g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondAverage = (g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondNow*2)/3 + g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondAverage/3;
      else
         g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondAverage = (g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondNow)/4 + (g_RadioTxTimers.uComputedVideoTxTimeMilisecPerSecondAverage*3)/4;
   
      g_RadioTxTimers.aHistoryTotalRadioTxTimes[g_RadioTxTimers.iCurrentIndexHistoryTotalRadioTxTimes] = g_RadioTxTimers.uComputedTotalTxTimeMilisecPerSecondNow;
      g_RadioTxTimers.iCurrentIndexHistoryTotalRadioTxTimes++;
      if ( g_RadioTxTimers.iCurrentIndexHistoryTotalRadioTxTimes >= MAX_RADIO_TX_TIMES_HISTORY_INTERVALS )
         g_RadioTxTimers.iCurrentIndexHistoryTotalRadioTxTimes = 0;
   }

   if ( bIsVideoPacket )
   {
      s_countTXVideoPacketsOutTemp++;
   }
   else
   {
      s_countTXDataPacketsOutTemp++;
      s_countTXCompactedPacketsOutTemp++;
   }

   if ( NULL != g_pProcessStats )
      g_pProcessStats->lastRadioTxTime = g_TimeNow;
   

   #ifdef LOG_RAW_TELEMETRY
   t_packet_header* pPH = (t_packet_header*) pPacketData;
   if ( pPH->packet_type == PACKET_TYPE_TELEMETRY_RAW_DOWNLOAD )
   {
      t_packet_header_telemetry_raw* pPHTR = (t_packet_header_telemetry_raw*)(pPacketData + sizeof(t_packet_header));
      log_line("[Raw_Telem] Send raw telemetry packet to radio interfaces, index %u, %d bytes", pPHTR->telem_segment_index, pPH->total_length);
   }
   #endif

   return 0;
}

void send_packet_vehicle_log(u8* pBuffer, int length)
{
   t_packet_header PH;
   radio_packet_init(&PH, PACKET_COMPONENT_RUBY, PACKET_TYPE_RUBY_LOG_FILE_SEGMENT, STREAM_ID_DATA);
   PH.vehicle_id_src = g_pCurrentModel->uVehicleId;
   PH.vehicle_id_dest = PH.vehicle_id_src;
   PH.total_length = sizeof(t_packet_header) + sizeof(t_packet_header_file_segment) + (u16)length;

   t_packet_header_file_segment PHFS;
   PHFS.file_id = FILE_ID_VEHICLE_LOG;
   PHFS.segment_index = s_VehicleLogSegmentIndex++;
   PHFS.segment_size = (u32) length;
   PHFS.total_segments = MAX_U32;

   u8 packet[MAX_PACKET_TOTAL_SIZE];
   memcpy(packet, (u8*)&PH, sizeof(t_packet_header));
   memcpy(packet + sizeof(t_packet_header), (u8*)&PHFS, sizeof(t_packet_header_file_segment));
   memcpy(packet + sizeof(t_packet_header) + sizeof(t_packet_header_file_segment), pBuffer, length);

   packets_queue_add_packet(&g_QueueRadioPacketsOut, packet);
}

void _send_alarm_packet_to_radio_queue(u32 uAlarmIndex, u32 uAlarm, u32 uFlags1, u32 uFlags2, u32 uRepeatCount)
{
   t_packet_header PH;
   radio_packet_init(&PH, PACKET_COMPONENT_RUBY, PACKET_TYPE_RUBY_ALARM, STREAM_ID_DATA);
   PH.vehicle_id_src = g_pCurrentModel->uVehicleId;
   PH.vehicle_id_dest = 0;
   PH.total_length = sizeof(t_packet_header) + 4*sizeof(u32);

   u8 packet[MAX_PACKET_TOTAL_SIZE];
   memcpy(packet, (u8*)&PH, sizeof(t_packet_header));
   memcpy(packet+sizeof(t_packet_header), &uAlarmIndex, sizeof(u32));
   memcpy(packet+sizeof(t_packet_header)+sizeof(u32), &uAlarm, sizeof(u32));
   memcpy(packet+sizeof(t_packet_header)+2*sizeof(u32), &uFlags1, sizeof(u32));
   memcpy(packet+sizeof(t_packet_header)+3*sizeof(u32), &uFlags2, sizeof(u32));
   packets_queue_add_packet(&g_QueueRadioPacketsOut, packet);

   char szBuff[128];
   alarms_to_string(uAlarm, uFlags1, uFlags2, szBuff);
   log_line("Sent alarm packet to router: %s, alarm index: %u, repeat count: %u", szBuff, uAlarmIndex, uRepeatCount);

   s_uTimeLastAlarmSentToRouter = g_TimeNow;
}

void send_alarm_to_controller(u32 uAlarm, u32 uFlags1, u32 uFlags2, u32 uRepeatCount)
{
   char szBuff[128];
   alarms_to_string(uAlarm, uFlags1, uFlags2, szBuff);

   s_uAlarmsIndex++;

   if ( test_link_is_in_progress() )
   {
      log_line("Skip sending alarm to controller (test link in progress): %s, alarm index: %u, repeat count: %u", szBuff, s_uAlarmsIndex, uRepeatCount);
      return;
   }
   if ( negociate_radio_link_is_in_progress() )
   {
      log_line("Skip sending alarm to controller (negociate radio in progress): %s, alarm index: %u, repeat count: %u", szBuff, s_uAlarmsIndex, uRepeatCount);
      return;
   }
   log_line("Sending alarm to controller: %s, alarm index: %u, repeat count: %u", szBuff, s_uAlarmsIndex, uRepeatCount);

   _send_alarm_packet_to_radio_queue(s_uAlarmsIndex, uAlarm, uFlags1, uFlags2, uRepeatCount);

   if ( uRepeatCount <= 1 )
      return;

   if ( s_AlarmsPendingInQueue >= MAX_ALARMS_QUEUE )
   {
      log_softerror_and_alarm("Too many alarms in the queue (%d alarms). Can't queue any more alarms.", s_AlarmsPendingInQueue);
      return;
   }
   uRepeatCount--;
   log_line("Queued alarm %s to queue position %d to be sent %d more times", szBuff, s_AlarmsPendingInQueue, uRepeatCount );

   s_AlarmsQueue[s_AlarmsPendingInQueue].uIndex = s_uAlarmsIndex;
   s_AlarmsQueue[s_AlarmsPendingInQueue].uId = uAlarm;
   s_AlarmsQueue[s_AlarmsPendingInQueue].uFlags1 = uFlags1;
   s_AlarmsQueue[s_AlarmsPendingInQueue].uFlags2 = uFlags2;
   s_AlarmsQueue[s_AlarmsPendingInQueue].uRepeatCount = uRepeatCount;
   s_AlarmsQueue[s_AlarmsPendingInQueue].uStartTime = g_TimeNow;
   s_AlarmsPendingInQueue++;
}


void send_alarm_to_controller_now(u32 uAlarm, u32 uFlags1, u32 uFlags2, u32 uRepeatCount)
{
   char szBuff[128];
   alarms_to_string(uAlarm, uFlags1, uFlags2, szBuff);

   s_uAlarmsIndex++;

   if ( test_link_is_in_progress() )
   {
      log_line("Skip sending alarm to controller (test link in progress): %s, alarm index: %u, repeat count: %u", szBuff, s_uAlarmsIndex, uRepeatCount);
      return;
   }
   if ( negociate_radio_link_is_in_progress() )
   {
      log_line("Skip sending alarm to controller (negociate radio in progress): %s, alarm index: %u, repeat count: %u", szBuff, s_uAlarmsIndex, uRepeatCount);
      return;
   }
   log_line("Sending alarm to controller: %s, alarm index: %u, repeat count: %u", szBuff, s_uAlarmsIndex, uRepeatCount);

   for( u32 u=0; u<uRepeatCount; u++ )
   {
      t_packet_header PH;
      radio_packet_init(&PH, PACKET_COMPONENT_RUBY, PACKET_TYPE_RUBY_ALARM, STREAM_ID_DATA);
      PH.vehicle_id_src = g_pCurrentModel->uVehicleId;
      PH.vehicle_id_dest = 0;
      PH.total_length = sizeof(t_packet_header) + 4*sizeof(u32);

      u8 packet[MAX_PACKET_TOTAL_SIZE];
      memcpy(packet, (u8*)&PH, sizeof(t_packet_header));
      memcpy(packet+sizeof(t_packet_header), &s_uAlarmsIndex, sizeof(u32));
      memcpy(packet+sizeof(t_packet_header)+sizeof(u32), &uAlarm, sizeof(u32));
      memcpy(packet+sizeof(t_packet_header)+2*sizeof(u32), &uFlags1, sizeof(u32));
      memcpy(packet+sizeof(t_packet_header)+3*sizeof(u32), &uFlags2, sizeof(u32));
      send_packet_to_radio_interfaces(packet, PH.total_length, -1);

      alarms_to_string(uAlarm, uFlags1, uFlags2, szBuff);
      log_line("Sent alarm packet to radio: %s, alarm index: %u, repeat count: %u", szBuff, s_uAlarmsIndex, uRepeatCount);

      s_uTimeLastAlarmSentToRouter = g_TimeNow;
      hardware_sleep_ms(50);
   }
}

void send_pending_alarms_to_controller()
{
   if ( s_AlarmsPendingInQueue == 0 )
      return;

   if ( g_TimeNow < s_uTimeLastAlarmSentToRouter + 150 )
      return;

   if ( s_AlarmsQueue[0].uRepeatCount == 0 )
   {
      for( int i=0; i<s_AlarmsPendingInQueue-1; i++ )
         memcpy(&(s_AlarmsQueue[i]), &(s_AlarmsQueue[i+1]), sizeof(t_alarm_info));
      s_AlarmsPendingInQueue--;
      if ( s_AlarmsPendingInQueue == 0 )
         return;
   }

   if ( (! test_link_is_in_progress()) && (! negociate_radio_link_is_in_progress()) )
      _send_alarm_packet_to_radio_queue(s_AlarmsQueue[0].uIndex, s_AlarmsQueue[0].uId, s_AlarmsQueue[0].uFlags1, s_AlarmsQueue[0].uFlags2, s_AlarmsQueue[0].uRepeatCount );
   s_AlarmsQueue[0].uRepeatCount--;
}