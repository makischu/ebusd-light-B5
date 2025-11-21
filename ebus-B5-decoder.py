#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#Copyright (C) 2025 makischu

#ebus-5B-decoder: translates vaillant-specific ebus-telegrams to readable json.
# see README for details.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.



import logging
import time
import paho.mqtt.client as mqtt
import json
import signal 
import queue
import pandas as pd
import matplotlib.pyplot as plt
from datetime import datetime


mqttIP = "192.168.2.43"
clientStrom = mqtt.Client()     
topicin   = "ebus/ll/rx"
topicout  = "ebus/ll/rxd" 
topictx   = "ebus/ll/tx" 
q = queue.Queue() #for processing messages in main loop, not callback


run = True

def handler_stop_signals(signum, frame):
    global run
    run = False

signal.signal(signal.SIGINT, handler_stop_signals)
signal.signal(signal.SIGTERM, handler_stop_signals)


#CRC8 lookup table for the polynom 0x9b = x^8 + x^7 + x^4 + x^3 + x^1 + 1.
CRC_LOOKUP_TABLE = [
  0x00, 0x9b, 0xad, 0x36, 0xc1, 0x5a, 0x6c, 0xf7, 0x19, 0x82, 0xb4, 0x2f, 0xd8, 0x43, 0x75, 0xee,
  0x32, 0xa9, 0x9f, 0x04, 0xf3, 0x68, 0x5e, 0xc5, 0x2b, 0xb0, 0x86, 0x1d, 0xea, 0x71, 0x47, 0xdc,
  0x64, 0xff, 0xc9, 0x52, 0xa5, 0x3e, 0x08, 0x93, 0x7d, 0xe6, 0xd0, 0x4b, 0xbc, 0x27, 0x11, 0x8a,
  0x56, 0xcd, 0xfb, 0x60, 0x97, 0x0c, 0x3a, 0xa1, 0x4f, 0xd4, 0xe2, 0x79, 0x8e, 0x15, 0x23, 0xb8,
  0xc8, 0x53, 0x65, 0xfe, 0x09, 0x92, 0xa4, 0x3f, 0xd1, 0x4a, 0x7c, 0xe7, 0x10, 0x8b, 0xbd, 0x26,
  0xfa, 0x61, 0x57, 0xcc, 0x3b, 0xa0, 0x96, 0x0d, 0xe3, 0x78, 0x4e, 0xd5, 0x22, 0xb9, 0x8f, 0x14,
  0xac, 0x37, 0x01, 0x9a, 0x6d, 0xf6, 0xc0, 0x5b, 0xb5, 0x2e, 0x18, 0x83, 0x74, 0xef, 0xd9, 0x42,
  0x9e, 0x05, 0x33, 0xa8, 0x5f, 0xc4, 0xf2, 0x69, 0x87, 0x1c, 0x2a, 0xb1, 0x46, 0xdd, 0xeb, 0x70,
  0x0b, 0x90, 0xa6, 0x3d, 0xca, 0x51, 0x67, 0xfc, 0x12, 0x89, 0xbf, 0x24, 0xd3, 0x48, 0x7e, 0xe5,
  0x39, 0xa2, 0x94, 0x0f, 0xf8, 0x63, 0x55, 0xce, 0x20, 0xbb, 0x8d, 0x16, 0xe1, 0x7a, 0x4c, 0xd7,
  0x6f, 0xf4, 0xc2, 0x59, 0xae, 0x35, 0x03, 0x98, 0x76, 0xed, 0xdb, 0x40, 0xb7, 0x2c, 0x1a, 0x81,
  0x5d, 0xc6, 0xf0, 0x6b, 0x9c, 0x07, 0x31, 0xaa, 0x44, 0xdf, 0xe9, 0x72, 0x85, 0x1e, 0x28, 0xb3,
  0xc3, 0x58, 0x6e, 0xf5, 0x02, 0x99, 0xaf, 0x34, 0xda, 0x41, 0x77, 0xec, 0x1b, 0x80, 0xb6, 0x2d,
  0xf1, 0x6a, 0x5c, 0xc7, 0x30, 0xab, 0x9d, 0x06, 0xe8, 0x73, 0x45, 0xde, 0x29, 0xb2, 0x84, 0x1f,
  0xa7, 0x3c, 0x0a, 0x91, 0x66, 0xfd, 0xcb, 0x50, 0xbe, 0x25, 0x13, 0x88, 0x7f, 0xe4, 0xd2, 0x49,
  0x95, 0x0e, 0x38, 0xa3, 0x54, 0xcf, 0xf9, 0x62, 0x8c, 0x17, 0x21, 0xba, 0x4d, 0xd6, 0xe0, 0x7b,
]
    
def ebusCRC(arraydata):
    data = bytes(arraydata)
    crc  = b'\x00';
    byte = b'\x00';
    inject=b'\xFF'
    i=0
    while i<len(data):
        byte = data[i:i+1];
        # "the CRC is calculated over the EXPANDED byte transmission sequence"
        if inject!=b'\xFF':
            byte   = inject
            inject = b'\xFF'
            i-=1
        if byte == b'\xAA':
            byte   = b'\xA9'
            inject = b'\x01'
        if byte == b'\xA9':
            byte   = b'\xA9'
            inject = b'\x00'
        crc = bytes([ CRC_LOOKUP_TABLE[crc[0]]  ^  byte[0] ]);
        i+=1
    return bytearray([ crc[0] ])

def isMaster(ZZ):
    ZZi = ZZ[0]
    master_addresses = [
        0x00, 0x10, 0x30, 0x70, 0xF0,
        0x01, 0x11, 0x31, 0x71, 0xF1,
        0x03, 0x13, 0x33, 0x73, 0xF3,
        0x07, 0x17, 0x37, 0x77, 0xF7,
        0x0F, 0x1F, 0x3F, 0x7F, 0xFF
    ]
    return ZZi in master_addresses;

def isBroadcast(ZZ):
    return ZZ[0] == 0xFE

# def isACKorNAK(ACK):
#     return ACK[0] == 0x00 or ACK[0] == 0xFF
def isACK(ACK):
    return ACK[0] == 0x00 

#divide request and response, remove ACK/SYN, and make some checks to simplify further processing.
def divideTelegram(tel):
    tel_m = None
    tel_s = None
    pay_m = None
    pay_s = None
    tel_valid = False
    if len(tel) >= 6:
        ZZ      = tel[1:2]              #dest
        NN_m    = tel[4:5]              #payload-len
        req_len = 5+NN_m[0]+1           #request-len
        if len(tel) >= req_len:
            tel_m   = tel[0:req_len]    #request
            CRC_m   = tel_m[-1:]         
            CRC_calc= ebusCRC(tel_m[0:-1])
            if CRC_m == CRC_calc:
                if isBroadcast(ZZ):
                    tel_valid = True
                else:
                    if isMaster(ZZ):                #master telegrams do not contain response data except ACK. we dont care about the ack, as we are interested in data, not bus debugging.
                        tel_valid = True
                    else:
                        tel_s = tel[req_len:]       #response-candidate
                        if len(tel_s) >= 1:
                            ACK   = tel_s[0:1]          #ACK - wether positive or negative
                            if isACK(ACK):              #NAKs are valid too but not followed by (immediate) data, and we don't treat retransmissions here, so we treat them as invalid 
                                if len(tel_s) >= 3:
                                    NN_s  = tel_s[1:2]
                                    res_len = 2+NN_s[0]+1
                                    if len(tel_s) >= res_len:
                                        tel_s = tel_s[0:res_len]
                                        CRC_s    = tel_s[-1:]
                                        CRC_calc = ebusCRC(tel_s[0:-1])
                                        if CRC_s == CRC_calc:
                                            del tel_s[0] #remove ACK - as master's ACK, SYN etc are also not returned.
                                            tel_valid = True;
                                            #a potential master ack and syn are not evaluated as not required for data extraction.
    if tel_valid: #redundant, but for caller's convenience...
        pay_m = tel_m[5:-1]
        if tel_s is not None:
            pay_s = tel_s[1:-1]
    return tel_valid, tel_m, tel_s, pay_m, pay_s
                

def h2b(hex_str):
   res = bytearray.fromhex(hex_str)
   return res

def b2u(byts): 
    #todo handle lower and upper bound values
    res  = int.from_bytes(byts,byteorder='little',signed=False)
    return res

def b2s(byts):
    #todo handle lower and upper bound values
    res  = int.from_bytes(byts,byteorder='little',signed=True)
    return res

def i2h(i):
    #impl for single byte only so far.
    res =  i.to_bytes(1,'big').hex()
    return res
    
def decodeTelegram(telegram):
    #print(telegram);
    tel = bytearray.fromhex(telegram)
    #tel = bytes(tel)
    decoded = {}
    
    tel_valid, tel_m, tel_s, pay_m, pay_s = divideTelegram(tel)
    
    if tel_valid and len(pay_m)>=1:
        ADDR = tel_m[0:2]               # QQ ZZ 
        REGA = tel_m[2:4]+pay_m[0:1]    # PB SB __ ID
        ADDRnREGA = ADDR+REGA           # QQ ZZ PB SB  ID
        ZZnREGA = ADDRnREGA[1:]         #    ZZ PB SB  ID
        pay_m_v = pay_m[1:]             # vaillant-payload is 1 less as first is always some index.
        
        #silent mode nr slots per weekday read
        #"31 15 B5 55 07 A4 00 04 FF FF FF FF 82 00 09 00 02 02 02 02 02 02 02 00 23 00 AA"
        if    ZZnREGA == h2b(   "15 B5 55 A4") and len(pay_s)*3 == len("00 02 02 02 02 02 02 02 00 "):
            if len(pay_m_v)*3 == len("00 04 00 01 FF FF ") and pay_m_v[0:2] == h2b('00 04') and pay_s[0:1] == h2b('00'):
                res = ""
                for i in range(7):
                    res = res + "{c};".format(c=b2u(pay_s[1+i:2+i]))
                decoded["SilentScheduleSlots"] = res
        
        #silent mode schedule read
        #"31 15 B5 55 07 A5 00 04 00 01 FF FF 1E 00 07 00 15 00 18 00 FF FF 8E 00 AA"
        if    ZZnREGA == h2b(   "15 B5 55 A5") and len(pay_s)*3 == len("00 15 00 18 00 FF FF "):
            if len(pay_m_v)*3 == len("00 04 00 01 FF FF ") and pay_m_v[0:2] == h2b('00 04') and pay_s[0:1] == h2b('00'):
                dow = b2u(pay_m_v[2:3])
                slot= b2u(pay_m_v[3:4])
                starthour = b2u(pay_s[1:2])
                startmin  = b2u(pay_s[2:3])
                endhour   = b2u(pay_s[3:4])
                endmin    = b2u(pay_s[4:5])
                decoded["SilentScheduleSlotR"] = "{d};{s};{s1};{s2};{e1};{e2}".format(d=dow,s=slot,s1=starthour,s2=startmin,e1=endhour,e2=endmin)
        
        #silent mode schedule write
        #"31 15 B5 55 0C A6 00 04 00 01 02 15 00 18 00 FF FF 64 00 01 00 9B 00 AA"
        if    ZZnREGA == h2b(   "15 B5 55 A6") and len(pay_s)*3 == len("00 "):
            if len(pay_m_v)*3 == len("00 04 00 01 02 15 00 18 00 FF FF ") and pay_m_v[0:2] == h2b('00 04') and pay_s[0:1] == h2b('00'):
                dow  = b2u(pay_m_v[2:3])
                slot = b2u(pay_m_v[3:4])
                slots= b2u(pay_m_v[4:5])
                starthour = b2u(pay_m_v[5:6])
                startmin  = b2u(pay_m_v[6:7])
                endhour   = b2u(pay_m_v[7:8])
                endmin    = b2u(pay_m_v[8:9])
                decoded["SilentScheduleSlotW"] = "{d};{s};{n};{s1};{s2};{e1};{e2}".format(d=dow,s=slot,n=slots,s1=starthour,s2=startmin,e1=endhour,e2=endmin)
                
        # ADDR    REG           MDAT     SDAT
        # # 71 08 | B5 14 xx 05  | XN 03 FF FF  |  XN 00 AA AA 
        if    ZZnREGA == h2b(   "08 B5 14 05") and len(pay_s)*3 == len("XN 00 32 00 "):
            if len(pay_m_v) == 4 and pay_m_v[1:4] == h2b('03 FF FF'):
                tregid = pay_s[0:1]
                testid = b2u(tregid)
                val16u = b2u(pay_s[2:4])
                val16s = b2s(pay_s[2:4])
                if False:
                    pass
                elif testid == 43: #tregid == h2b('2B'):
                    decoded["WFlowT[l/h]"] = val16u
                elif testid == 1: 
                    decoded["WPumpLvl[%]"] = val16u
                elif testid == 17: 
                    decoded["Fan1Lvl[%]"] = val16u
                elif testid == 19: 
                    decoded["CondHeat[on]"] = val16u
                elif testid == 20: 
                    decoded["4PortV[on]"] = val16u
                elif testid == 21: 
                    decoded["EEV[%]"] = val16u
                elif testid == 23: 
                    decoded["CompHeat[on]"] = val16u
                elif testid == 40: 
                    decoded["ForwTempT[C]"] = val16u/10
                elif testid == 41: 
                    decoded["RetnTempT[C]"] = val16u/10
                elif testid == 41: 
                    decoded["WPresT[bar]"] = val16u/10
                elif testid == 48: 
                    decoded["AirInTT[C]"] = val16s/10
                elif testid == 55: 
                    decoded["CompOutT[C]"] = val16u/10
                elif testid == 56: 
                    decoded["CompInT[C]"] = val16u/10
                elif testid == 57:
                    decoded["EEVOutT[C]"] = val16u/10
                elif testid == 59:
                    decoded["CondOutT[C]"] = val16u/10
                elif testid == 63:
                    decoded["HighPres[bar]"] = val16u/10
                elif testid == 64:
                    decoded["LowSPres[bar]"] = val16u/10
                elif testid == 67:
                    decoded["HighPresSw[ok]"] = val16u
                elif testid == 85:
                    decoded["EvapTemp[C]"] = val16u/10
                elif testid == 86:
                    decoded["CondTemp[C]"] = val16u/10
                elif testid == 87:
                    decoded["OverheatSet[K]"] = val16s/10
                elif testid == 88:
                    decoded["OverheatAct[K]"] = val16s/10
                elif testid == 89:
                    decoded["SubcoolSet[K]"] = val16s/10
                elif testid == 90:
                    decoded["SubcoolAct[K]"] = val16s/10
                elif testid == 93:
                    decoded["CompSpeed[rps]"] = val16u/10
                elif testid == 123:
                    decoded["CompOutTempSw[ok]"] = val16u
                elif testid == 46:
                    decoded["DigInS20[closed]"] = val16u
                elif testid == 72:
                    decoded["DigInS21[closed]"] = val16u
                elif testid == 119:
                    decoded["DigOutMA1[on]"] = val16u
                elif testid == 125:
                    decoded["DigInME[closed]"] = val16u
                elif testid == 126:
                    decoded["DigOutMA2[on]"] = val16u
        
        # 71 08 | B5 1A xx 05  | xx 32 PA |  xx 08 0E AA BB xx xx xx xx xx  AA BB depends in additional parameter PA!
        #if ADDRnREGA == h2b("71 08 B5 1A 05") and len(pay_s)*3 == len("xx 08 0E EI xx xx xx xx xx xx "):
        if    ZZnREGA == h2b(   "08 B5 1A 05") and len(pay_s)*3 == len("xx 08 0E EI xx xx xx xx xx xx "):
            if len(pay_m_v) == 3:
                if False:
                    pass
                elif pay_m_v[1:3] == h2b('32 1E'):
                    val = b2u(pay_s[3:5])
                    decoded["VV1E[?]"] = val
                elif pay_m_v[1:3] == h2b('32 1F'):
                    val = b2u(pay_s[3:5])
                    decoded["ForwSetT1F[C]"] = val/16
                elif pay_m_v[1:3] == h2b('32 20'):
                    val = b2u(pay_s[3:5])
                    decoded["ForwTemp20[C]"] = val/16
                elif pay_m_v[1:3] == h2b('32 21'):
                    val = b2s(pay_s[3:5])  #not sure if 1 or 2 byte
                    decoded["EnInt[Cmin]"] = val
                elif pay_m_v[1:3] == h2b('32 23'):
                    val = b2u(pay_s[3:4])
                    decoded["PEnv[kW]"] = val/10
                elif pay_m_v[1:3] == h2b('32 24'):
                    val = b2u(pay_s[3:4])
                    decoded["PEle[kW]"] = val/10
                elif pay_m_v[1:3] == h2b('32 25'):
                    val = b2u(pay_s[3:5])
                    decoded["CompMod[%]"] = val/16
                elif pay_m_v[1:3] == h2b('32 26'):
                    val = b2s(pay_s[3:5])
                    decoded["AirInT[C]"] = val/16
                elif pay_m_v[1:3] == h2b('32 3C'):
                    val = b2s(pay_s[3:5]) 
                    decoded["WFlow[l/h]"] = val
                elif pay_m_v[1:3] == h2b('32 3D'):
                    val = b2u(pay_s[3:5]) #not sure if 1 or 2 byte
                    decoded["VV3D[?]"] = val
                
        # 03 76 | B5 12 xx 13  | xx PR FL FL xx | xx xx                     PR=Pressure (bar/10), FL=Flow (l/h)
        elif ADDRnREGA == h2b("03 76 B5 12 13") and len(pay_m_v)*3 == len("xx PR FL FL xx ") and len(pay_s)*3 == len("xx xx "):
            val = b2u(pay_m_v[1:2])
            decoded['WPres[bar]']=val/10       
            val = b2u(pay_m_v[2:4])
            decoded['WFlow[l/h]']=val

        #71 08 | B5 11 xx 07  |    |  LS TE TE xx xx xx xx xx xx xx        LS=kind of power level (%), TE=day yield (?)
        elif  ADDRnREGA == h2b("71 08 B5 11 07") and len(pay_m_v)*3 == 0 and len(pay_s)*3 == len("LS TE TE xx xx xx xx xx xx xx "):
            val = b2u(pay_s[0:1])
            decoded['PwrLvl[%?]']=val           
            val = b2u(pay_s[1:3])
            decoded['EHeatDay[kWh]']=val/10
            
        # 10 76 | B5 11 xx 01  |    |  xx xx AT AT xx xx xx xx xx           AT=outdoor temp(deg/256).
        elif  ADDRnREGA == h2b("10 76 B5 11 01") and len(pay_m_v)*3 == 0 and len(pay_s)*3 == len("xx xx AT AT xx xx xx xx xx "):
            val = b2s(pay_s[2:4])
            decoded['OutdTemp[C]']=val/256
            
        # 10 08 | B5 11 xx 01  |    |  VL RL xx xx xx xx xx xx xx           VL=forward flow temp RL=return flow temp  (deg/2)
        elif  ADDRnREGA == h2b("10 08 B5 11 01") and len(pay_m_v)*3 == 0 and len(pay_s)*3 == len("VL RL xx xx xx xx xx xx xx "):
            val = b2s(pay_s[0:1])
            decoded['ForwTempL[C]']=val/2
            val = b2s(pay_s[1:2])
            decoded['RetnTempL[C]']=val/2
            
        # 10 08 | B5 11 xx 00  |    |  VL VL PR AA AA BB xx xx xx           VL=forw flow (deg/16). AA=analog?? BB=analog?/state?
        elif  ADDRnREGA == h2b("10 08 B5 11 00") and len(pay_m_v)*3 == 0 and len(pay_s)*3 == len("VL VL PR AA AA BB xx xx xx "):
            val = b2s(pay_s[0:2])
            decoded['ForwTemp[C]']=val/16
            val = b2u(pay_s[2:3])
            decoded['WPres[bar]']=val/10
            val = b2s(pay_s[3:5])
            decoded['AAAA[?]']=val
            val = b2s(pay_s[5:6])
            decoded['OpMode']=val
            
        # 10 08 | B5 10 xx 00  | xx VS xx xx xx xx xx xx      | xx        VS=VLset(deg/2) 
        # 10 76 | B5 10 xx 00  | xx 00 xx xx xx yy xx xx      | xx        example showing that content is ADDR-specific
        elif ADDRnREGA == h2b("10 08 B5 10 00") and len(pay_m_v)*3 == len("xx VS xx xx xx xx xx xx ") and len(pay_s)*3 == len("xx "):
            val = b2u(pay_m_v[1:2])
            decoded['ForwSetT[C]']=val/2
        
        # 10 08 | B5 07 xx 09  | VS |  LA LA                                VS=VLset(deg/2). Air out ?? (Grad/64)
        elif ADDRnREGA == h2b("10 08 B5 07 09") and len(pay_m_v)*3 == len("VS ") and len(pay_s)*3 == len("LA LA "):
            val = b2u(pay_m_v[0:1])
            decoded['ForwSetT[C]']=val/2
            val = b2s(pay_s[0:2])
            decoded['AirOutT?[C]']=val/64

        # 10 76 | B5 04 xx 00  |    |  QQ ss mm HH DD MM wd YY AT AT        DCF-Clock and Outdoortemp.
        elif  ADDRnREGA == h2b("10 76 B5 04 00") and len(pay_m_v)*3 == 0 and len(pay_s)*3 == len("QQ ss mm HH DD MM wd YY AT AT "):
            QQ = pay_s[0]
            if QQ == 3:
                val="{HH}:{mm}:{ss}".format(HH=pay_s[3:4].hex(),mm=pay_s[2:3].hex(),ss=pay_s[1:2].hex())
                decoded['DCFTime']=val
            
        # 10 FE | B5 08 xx 09  | QM                                         QM=Quiet Mode 
        elif  ADDRnREGA == h2b("10 FE B5 08 09") and len(pay_m_v)*3 == len("QM "):
            val = b2u(pay_m_v[0:1])
            decoded['QuietMode']=val
            
        #indoor/room temperature missing?
        #anyhow, a lot more sensors (which must be present according to the manual) are not decoded yet.

    return decoded
            
    
    
    
def publishRxd(linedict):
    global clientStrom,topicout
    clientStrom.publish(topicout, json.dumps(linedict))
    
def publishTx(telegramstr):
    global clientStrom,topictx
    txdict = { "telegram" : telegramstr }
    clientStrom.publish(topictx, json.dumps(txdict) )
    
    
def publishTelegramAddCrc(telegramStringWithoutCRC):
    requestTelegram = telegramStringWithoutCRC.strip() + " " 
    requestTelegram = requestTelegram + ebusCRC(h2b(requestTelegram)).hex()
    publishTx(requestTelegram)
    
def publishTestRequest(testOfInterest):
    requestTelegram = "31 08 B5 14 05 05 " + i2h(testOfInterest) + " 03 FF FF "
    publishTelegramAddCrc(requestTelegram)
    
    
def requestSilentScheduleSlotCounts():
    requestTelegram = "31 15 B5 55 07 A4 00 04 FF FF FF FF"
    publishTelegramAddCrc(requestTelegram)
    
def requestSilentScheduleSlot(dow, slot):
    requestTelegram = "31 15 B5 55 07 A5 00 04 " + i2h(dow)+" " +i2h(slot) + " FF FF"
    publishTelegramAddCrc(requestTelegram)
    
def writeSilentScheduleSlot(dow, slotindex, slotcount, hh1, mm1, hh2, mm2):
    requestTelegram = "31 15 B5 55 0C A6 00 04 " + i2h(dow)+" " +i2h(slotindex)+" " +i2h(slotcount)+" " +i2h(hh1)+" " +i2h(mm1)+" " +i2h(hh2)+" " +i2h(mm2)+ " FF FF"
    publishTelegramAddCrc(requestTelegram)
    
def writeSilentScheduleSlotsNight():
    for i in range(0,7):
        writeSilentScheduleSlot(i,0,2,0,0,4,30);
        time.sleep(0.6)
        writeSilentScheduleSlot(i,1,2,21,0,24,00);
        time.sleep(0.6)
        
def writeSilentScheduleSlotsNone():
    for i in range(0,7):
        writeSilentScheduleSlot(i,0,0,0,0,0,0);
        time.sleep(0.6)
        
def writeSilentScheduleSlotsAlways():
    for i in range(0,7):
        writeSilentScheduleSlot(i,0,1,0,0,24,0);
        time.sleep(0.6)
    

#MQTT-Callback.
def on_message(client, userdata, message):
    try:
        messagestr = str(message.payload.decode())
        #print(messagestr) 
        #{"telegram":"10 08 B5 11 01 01 89 00 09 3A 3A 00 80 FF FF 00 00 FF 49 00 AA"}
        data = json.loads(messagestr)
        if "telegram" in data:
            q.put(data)
    except:
        print("on_message failed to parse. ignore and continue.")
        

def startMqtt():
    global clientStrom, topicin
    logging.info('Starting mqtt...')
    clientStrom.on_message = on_message;
    clientStrom.connect(mqttIP, 1883, 60)
    clientStrom.loop_start()
    clientStrom.subscribe(topicin)
    
def stopMqtt():
    global clientStrom
    logging.info('Stopping mqtt...\n')
    clientStrom.loop_stop()
    



if __name__ == '__main__':
    
    # commented code is for development / reverse engineering.
    
    # # hand-chosen telegrams.
    # telegrams = ["71 08 B5 1A 04 05 F7 32 3D 32 00 0A F7 08 2C 4D 00 00 00 00 00 00 4C 00 AA",
    #               "71 08 B5 1A 04 05 11 32 3C DF 00 0A 11 08 0E B1 04 00 00 00 00 00 68 00 AA",
    #               "03 76 B5 12 06 13 00 0C 4D 03 00 C2 00 02 00 FF D3 00 AA",
    #               "71 08 B5 11 01 07 B4 00 0A 00 BA 00 E1 08 24 00 00 00 00 C9 00 AA",
    #               "10 76 B5 11 01 01 16 00 09 FF FF 70 0D FF FF 00 00 FF C5 00 AA",
    #               "10 08 B5 11 01 01 89 00 09 2C 2D 00 80 FF FF 00 00 FF CF 00 AA",
    #               "10 08 B5 11 01 00 88 00 09 65 01 0C 00 00 08 00 00 00 F9 00 AA"
    #               "10 76 B5 04 01 00 A2 00 0A 03 52 00 00 18 09 04 25 70 0D AE 00 AA",
    #               "10 76 B5 04 01 00 A2 00 0A 03 26 12 03 19 09 05 25 A0 0B 96 00 AA"
    #               "10 08 B5 07 02 09 2A 65 00 02 1B 03 32 00 AA",
    #               "10 08 B5 07 02 09 2A 65 00 02 1B 03 32 00 AA",
    #               "10 FE B5 08 02 09 01 A9 AA",
    #               "10 08 B5 10 09 00 00 2A FF FF FF 06 00 00 46 00 01 01 9A 00 AA",
    #               "71 08 B5 1A 04 05 95 32 25 B1 00 0A 95 08 38 CB 01 00 00 00 00 00 B1 00 AA",
    #               "31 15 B5 55 07 A5 00 04 00 01 FF FF 1E 00 07 00 15 00 18 00 FF FF 8E 00 AA",
    #               "31 15 B5 55 07 A5 00 04 00 00 00 00 8C 00 07 00 00 00 04 1E FF FF D7 00 AA",
    #               "31 15 B5 55 07 A4 00 04 FF FF FF FF 82 00 09 00 02 02 02 02 02 02 02 00 23 00 AA",
    #               "31 15 B5 55 0C A6 00 04 00 01 02 15 00 18 00 FF FF 64 00 01 00 9B 00 AA",
    #               ] 
    # for telegram in telegrams:
    #     dec = decodeTelegram(telegram);
    #     print(dec)
    
    
    # # a complete day of pre-recorded telegrams
    # #dfe  = pd.read_csv('./ebus_2025-09-19rt.csv',sep=';');
    # dfe  = pd.read_csv('/mnt/knecht/machines/143_stromlog/ebus/ebus_2025-09-26rt.csv',sep=';');
    # #file format:
    # # t;telegram;  
    # # 02:15:18;10 76 B5 10 09 00 00 00 FF FF FF 05 00 00 DD 00 01 01 9A 00 AA;
    
    # # #example for futher narrowing down / focussing on specific registers.
    # # df1 = dfe[dfe['telegram'].str.startswith('71 08 B5 1A 04 05')]
    # # df1['extreg'] = df1['telegram'].str[21:26]
    # # print(df1['extreg'].value_counts())
    # # df2 = df1[df1['extreg']=='32 26'] #airintemp?
   
    # #df1 = dfe[dfe['telegram'].str.startswith('10 FE B5 ')]
    
    
    # #decode und gerate a csv, which is more friendly for further processing/watching.
    # dictdec = {}
    # for i,row in dfe.iterrows():
    #     tim = row['t']
    #     tel = row['telegram']
    #     dec = decodeTelegram(tel)
    #     tHHmm = tim[:-3]
    #     if tHHmm not in dictdec:
    #         dictdec[tHHmm]={}
    #     for key,val in dec.items():
    #         dictdec[tHHmm][key] = val
    # dfdec = pd.DataFrame.from_dict(dictdec,orient='index')
    # dfdec = dfdec[sorted(dfdec.columns)]
    # dfdec.sort_index(inplace=True)
    # dfdec.to_csv('test.csv',sep=';')
    
    
    # #plot example - helpful for reverse engineering of unknown values, 
    # df = dfdec
    # df['Zeit'] = pd.to_datetime(df.index)
    # new_date = datetime(2025, 9, 19)
    # df['Zeit'] = df['Zeit'].apply(lambda dt: datetime.combine(new_date.date(), dt.time()))
    # plt.figure()
    # key = 'AirInT[C]'
    # plt.plot(df['Zeit'], df[key], marker='.', linestyle='-', label=key)
    # plt.legend()
    # plt.grid(True)
    # plt.xticks(rotation=45)
    # plt.tight_layout()
    # plt.show()
    
    
    
    
    # live translation. subscribe to ebus/ll/rxd to watch output.
    startMqtt()
    
    #Fluestermodus Tests
    #writeSilentScheduleSlotsNight()
    #writeSilentScheduleSlotsNone()
    #writeSilentScheduleSlotsAlways()
    
    tLastTrigger1 = 0
    tLastTrigger30 = 0
    tLastTrigger300 = 0
    # requestTelegram = "31 08 B5 14 05 05 40 03 FF FF AA"
    interest14 = [1, 17, 19, 20, 21, 23, 55, 56, 57, 59, 63, 64, 67, 85, 86, 87, 88, 89, 90, 93, 123, ]  #act like the test menu, request T.0.001 etc.
    interestRemaining = []
    while run:
        tNow = time.perf_counter()
        #Task1: translate received
        data = None
        try:
            data = q.get(False)
        except:
            pass
        if data:
            decoded = decodeTelegram(data["telegram"])
            if decoded:
                publishRxd(decoded)
            q.task_done()
        #Task2: request more.
        if tNow-tLastTrigger1 > 1:
            tLastTrigger1 = tNow
            if interestRemaining:
                testOfInterest = interestRemaining.pop(0)
                publishTestRequest(testOfInterest)
        elif tNow-tLastTrigger30 > 30:
            tLastTrigger30 = tNow
            #Energieintegral
            #ebusCRC(h2b("31 08 B5 1A 04 05 99 32 21")) 8E
            publishTx("31 08 B5 1A 04 05 99 32 21 8E")
            time.sleep(1.0)
            #Lufteinlasstemperatur
            testOfInterest = 48
            publishTestRequest(testOfInterest)
        elif tNow-tLastTrigger300 > 300:
            tLastTrigger300 = tNow
            interestRemaining = interest14.copy()
        time.sleep(0.1)
    stopMqtt()
    
    
