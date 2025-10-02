//Copyright (C) 2025 makischu

//ebusd-light: translates between adapter.ebusd.eu-hardware and ebus-telegrams via MQTT.
// translates between kind of a tcp-tunneled uart and whole telegrams, as hex strings.
// manages the link layer, so that others can focus on content.
// RX resembles a passive tap (just listening), and is fault-tolerant (e.g. does NOT filter out telegrams with bad checksums)
// TX is limited to [valid] master requests (responding as a slave is not implemented)

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// uses: paho mqtt https://github.com/eclipse/paho.mqtt.c
// parts copied from https://github.com/eclipse/paho.mqtt.c/blob/master/src/samples/MQTTClient_subscribe.c

// build: g++ ...c -lpaho-mqtt3c  

// requires an ebus adapter e.g. https://adapter.ebusd.eu/v5-c6/ 
// requires an mqtt broker[+client], e.g. mosquitto_sub -h localhost -p 1883 -t ebus/ll/rx   
// format:                                {"telegram":"10 08 B5 10 09 00 00 3D FF FF FF 06 00 00 26 00 01 01 9A 00 AA"}


#include <stdio.h> //printf
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <stdlib.h> //exit
#include <stdbool.h>
#include <stdint.h>
#include <string.h> //memset
#include <math.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <sys/uio.h>
#include <time.h>
#include <signal.h>
#include <iostream>
#include <cstring>
#include <unistd.h>      // for read(), write(), close()
#include <sys/socket.h>  // fpr socket(), connect()
#include <netinet/in.h>  // for sockaddr_in
#include <arpa/inet.h>   // for inet_addr()
#include <thread>        // for yield
#include <netinet/tcp.h>  // Defines TCP_NODELAY

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

volatile bool run = true;

void sig_handler(int signo)
{
  if (signo == SIGINT  || signo == SIGQUIT) 
    printf("received SIGINT\n");
  run=false;
}

#include "MQTTClient.h" //see above for installtion (clone local, make, sudo make install)

#define ADDRESS     "tcp://192.168.2.43:1883"  //"tcp://localhost:1883"
#define CLIENTID    "ebusd-light"
#define TOPIC_TX    "ebus/ll/tx"            // mqtt-messages to this topic are received and valid requests are sent on the ebus. format:  {"telegram":"AB CD ..."} 
#define TOPIC_RXD   "ebus/ll/rx"            // valid received ebus telegrams are sent to this mqtt topic. example: {"telegram":"10 FE B5 16 03 01 70 10 52 AA"}
#define QOS         0
#define TIMEOUT     2000L
#define USERTOKEN   "notused"

#define ADAPTER_ADDRESS "192.168.2.31"
#define ADAPTER_PORT    9999


// "the sequence of individual characters that a participant must use, when accessing the bus"
// may or may not be valid, may contain slave response too, may end with SYN or not, may be expanded or not, simple multi-purpose-struct.
// for RX: it is assumed that a telegram consists of all bytes between two SYN symbols. at least these are our candidates.
struct Telegram {
  uint8_t data[256]; //bytes (the actual data)
  int     len;       //nr of bytes
};


//program is implemented as a state machine
enum State {
START=0,
INIT1_MQTT, // MQTT connect
INIT2_ATCP, // TCP  connect to adapter
INIT3_AINI, // adapter initialized
INIT4_BUS,  // 
WORK,       // OPERATIONAL
RESTART,    //
DEIN4_BUS,  //
DEIN3_AINI, // 
DEIN2_ATCP, // TCP  close
DEIN1_MQTT, // MQTT disconnect
DEIN0_PAUS, // wait before retry.
};

char topic_to_publish[256]; 
char message_to_publish[256]; 
bool message_to_publish_valid = false;

uint8_t chars_to_send_bus[256];
int  chars_to_send_len;


Telegram telegramToSend;
Telegram telegramToSendExpanded;
Telegram telegramToSendExpandedEnhanced;
int telegramToSendExpandedEnhancedIndex;
//Sending is implemented as a sub-state-machine of WORK.
enum TelegramSendState {
    SENDIDLE,
    SENDSTART,
    ARBITRATION_INIT,
    ARBITRATION_AWAIT,
    SENDDATA,
    AWAITACK,
    AWAITRESPONSE,
    SENDACK,
    SENDSYN,
    FINISHED
};
TelegramSendState sendState;

void telegramExpand(Telegram* pIn, Telegram *pOut) {
    int i=0;
    uint8_t byte;
    pOut->len = 0;
    for(i=0;i<pIn->len;i++) {
        byte = pIn->data[i];
        if (pOut->len+2 >= sizeof(pOut->data)) {
            return; //avoid overflow, just in case, not expected to happen.
        }
        if (byte==0xAA) {
            pOut->data[pOut->len++] = 0xA9;
            pOut->data[pOut->len++] = 0x01;
        }
        else if (byte==0xA9) {
            pOut->data[pOut->len++] = 0xA9;
            pOut->data[pOut->len++] = 0x00;
        }
        else {
            pOut->data[pOut->len++] = byte;
        }
    }
}
void telegramExpandEnhanced(Telegram* pIn, Telegram *pOut) {
    int i=0;
    uint8_t byte;
    pOut->len = 0;
    for(i=0;i<pIn->len;i++) {
        byte = pIn->data[i];
        if (pOut->len+2 >= sizeof(pOut->data)) {
            return; //avoid overflow, just in case, not expected to happen.
        }
        // KISS. allows for easy separation for byte by byte sending. 1 char => 2 char.
        //if (byte & 0x80) {
            pOut->data[pOut->len++]  = 0xC0 | (0x01<<2) | ((byte&0xC0)>>6);
            pOut->data[pOut->len++]  = 0x80 | (byte&0x3F);
        // }
        // else {
        //     pOut->data[pOut->len++] = byte;
        // }
    }
}

bool json2struct(char* jsonstr, struct Telegram* pTelegram);

// received from MQTT = to send on bus
void handle_rxd(char* payload, int len) {
    char temp[256]; //ensure zero-termination.
    if (len>=sizeof(temp)) {
        printf("mqtt payload longer than expected. ignoring.");
        return;
    }
    memcpy(temp,payload,len);
    temp[len]=0;
    if (sendState != SENDIDLE) { // KISS. one telegram at a time only
        printf("telegram sending busy. ignored.");
    }
    else {
        if (json2struct(temp,&telegramToSend)) {
            sendState = SENDSTART;
        } else {
            printf("could not parse message to telegram: %s\n",temp);
        }
    }
    return;
}


// called when received a message via mqtt
int msgarrvd(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    //printf("Message arrived\n");
    //printf("     topic: %s\n", topicName);
    //printf("   message: %.*s\n", message->payloadlen, (char*)message->payload);

    if (strcmp(TOPIC_TX, topicName)==0) {
        handle_rxd((char*)message->payload, message->payloadlen);
    }

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

// called when ready to publish a message via mqtt
// buffers [must] retain valid until next call (must not be freed by caller as with msgarrvd).
// buffers [must] contain valid [=zero-terminated] strings (if return true)
// returns true if the returned message should be sent, false if not
bool msgPreparedMqtt(char** topic, char** payload) {
    if (message_to_publish_valid) {
        strcpy(topic_to_publish, TOPIC_RXD);
        *topic = topic_to_publish;
        *payload = message_to_publish;
        message_to_publish_valid = false;
        return true;
    }
    return false;
}



const uint8_t master_addresses[25] = {
     0x00, 0x10, 0x30, 0x70, 0xF0,
     0x01, 0x11, 0x31, 0x71, 0xF1,
     0x03, 0x13, 0x33, 0x73, 0xF3,
     0x07, 0x17, 0x37, 0x77, 0xF7,
     0x0F, 0x1F, 0x3F, 0x7F, 0xFF
 };
bool isMasterAddr(uint8_t addr) {
    for(int i=0;i<25;i++) {
        if (addr==master_addresses[i])
            return true;
    }
    return false;
}


Telegram telegramRxd;

/**
 * CRC8 lookup table for the polynom 0x9b = x^8 + x^7 + x^4 + x^3 + x^1 + 1.
 */
static const uint8_t CRC_LOOKUP_TABLE[] = {
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
};

uint8_t calcEbusCrc(uint8_t* pStart, int len) {
    uint8_t crc = 0x00;
    uint8_t byte;

    uint8_t inject=0xFF;
    for (int i=0;i<len;i++) {
        byte = pStart[i];
        // "the CRC is calculated over the EXPANDED byte transmission sequence"
        if (inject!=0xFF) {
            byte = inject;
            inject = 0xFF;
            i--;
        }
        if (byte == 0xAA) {
            byte = 0xA9; 
            inject = 0x01;
        }
        if (byte == 0xA9) {
            byte = 0xA9; 
            inject = 0x00;
        }
        crc = CRC_LOOKUP_TABLE[crc]^byte;
    }
    return crc;
}

bool struct2json(struct Telegram* pTelegram,char* jsonstr, int maxLen, int* pLen);

// received sth that looks like a valid telegram > report it.
// received on bus -> to sent via mqtt
void processBusTelegramChecked() {
    if (struct2json(&telegramRxd,message_to_publish,sizeof(message_to_publish),0)) {
        message_to_publish_valid = 1;
    }
}

int telegramCountBad=0;
int telegramCountOk=0;
void bytes2hexstr(uint8_t* arr, int n, char* pStr, int maxlen);


bool telegramCRCcheck(Telegram* telegram, bool slaveResponseNotMaster) {
    int minlen,offset,crcoffset;
    uint8_t NN,CRC,calcdCRC;
    bool result=false;
    // QQ ZZ XX XX NN    CRC
    offset = 0;
    minlen = 6;
    crcoffset = 5;
    NN = telegram->data[4]; 
    if (telegram->len >= minlen) {
        if (NN <= 16) {
            CRC      = telegram->data[crcoffset+NN];
            calcdCRC = calcEbusCrc(telegram->data+offset,crcoffset+NN-offset);
            if (CRC == calcdCRC) {
                result = true;
            }
        }
    }
    // ACK NN   CRC    - only if slave response
    if (result && slaveResponseNotMaster) {   
        result = false;
        offset = minlen+NN;
        minlen = 3;
        crcoffset = 2;
        NN = telegram->data[offset+1];
        if (NN <= 16) {
            CRC      = telegram->data[offset+crcoffset+NN];
            calcdCRC = calcEbusCrc(telegram->data+offset,crcoffset+NN);
            if (CRC == calcdCRC) {
                result = true;
            }
        }
    }
    return result;
}

bool telegramIsPlausibleTx(Telegram* telegram) {
    uint8_t QQ,NN;
    QQ = telegram->data[0];
    NN = telegram->data[4]; 
    if (isMasterAddr(QQ)) {
        if (telegramCRCcheck(telegram, false)) {
            return (telegram->len == 6+NN);
        }
    }
    return false;
}

bool telegramIsPlausibleRx(Telegram* telegram) {
    // this is a very minimalistic implementation, intentionally.
    // we do NO checks here to be able to deal with bugs at next layer.
    // exception: [auto-]SYNs with no content at all.
    return (telegram->len > 1);
}

// received on bus between two SYNs
void processBusTelegram() {
    if (telegramIsPlausibleRx(&telegramRxd)) {    
        processBusTelegramChecked();
        telegramCountOk++;
        return;
    }
    else if (telegramRxd.len>1) { 
        char tmp[256];
        bytes2hexstr(telegramRxd.data,telegramRxd.len, tmp,sizeof(tmp));
        printf("ingoring bad telegram %s \n",tmp);
        telegramCountBad++;
    }
}


Telegram telegramTxRxd; // echo of master request + slave response.
int arbitration_success;

// received on bus - like real uart
void processBusChar(uint8_t value) {
    static bool escaped=false;
    // handle escape character
    if (value==0xA9) {
        escaped = true;
        return;
    }
    else if (escaped) {
        if(value == 0x00)       value = 0xA9;
        else if (value==0x01)   value = 0xAA;
        escaped = false;
    }

    // store until SYN char, for receiving (RX)
    if(telegramRxd.len >= sizeof(telegramRxd.data)) {
        telegramRxd.len=0;
    }
    telegramRxd.data[telegramRxd.len++] = value;
    if (value == 0xAA) {
        processBusTelegram();
        telegramRxd.len = 0;
    }
    // if, store slave response to our master request (TX)
    if (sendState >= SENDDATA) {
        if (telegramTxRxd.len+1 < sizeof(telegramTxRxd.data)) {
            telegramTxRxd.data[telegramTxRxd.len++] = value;
        }
    }
}

// received on bus-enhanced-tcp.
void processEnhBusChar(uint8_t value) {
    static uint8_t enh1=0;
    uint8_t enh2; uint8_t cccc;

    if (value & 0x80) {   
        if ((value&0xC0) == 0xC0) {  // first byte of a two-byte-char
            enh1 = value;
            return;
        }
        if ((value&0xC0) == 0x80) {  // 2nc byte of a two-byte-char
            enh2 = value;
            value = ((enh1 & 0x3) << 6) | (enh2&0x3F);
            cccc  = (enh1 & 0x3C)>>2;
            //printf("%02x %02x %d\n", enh1, enh2, cccc);
            if (cccc == 2) {        // 2 = Arbitration Success 
                arbitration_success = 1;
            }
            if (cccc == 10) {        // 10 = Fail
                arbitration_success = 0;
            }
            if (cccc != 0x1 && cccc != 0x2 && cccc != 10) {       // 1 = Received, ... ignore others.
                printf("ignoring cccc %d\n",cccc);
                return;
            }
        }
    }
    processBusChar(value);
}

// returns true if some bytes are prepared for tcp send
bool charsPreparedTCP(char** payload, int* len) {
    bool chars_to_send_valid = false;
    uint8_t QQ,ZZ,NN,AK;
    static time_t tLastStateChange;
    time_t tnow;
    tnow = time(0);
    bool slaveCRCok=false;

    TelegramSendState nextState=sendState;
    switch(sendState) {
        case SENDIDLE:
            break;
        case SENDSTART: 
            //do some sanity checks, like for receiving.
            if(telegramIsPlausibleTx(&telegramToSend)) {
                telegramExpand(&telegramToSend,&telegramToSendExpanded);
                telegramExpandEnhanced(&telegramToSendExpanded,&telegramToSendExpandedEnhanced);
                telegramToSendExpandedEnhancedIndex=0;
                nextState = ARBITRATION_INIT;
            }
            else {
                printf("ignored, not a valid request telegram.\n");
                nextState = FINISHED;
            }
            break;
        case ARBITRATION_INIT:
            QQ = telegramToSend.data[0]; 
            chars_to_send_bus[0] = 0xC0 | (0x02<<2) | ((QQ&0xC0)>>6);
            chars_to_send_bus[1] = 0x80 | (QQ&0x3F);
            chars_to_send_len = 2;  // wireshark: c8b1 ok.
            chars_to_send_valid = true;
            arbitration_success = -1;
            memset(&telegramTxRxd,0,sizeof(telegramTxRxd));
            telegramTxRxd.data[telegramTxRxd.len++] = telegramToSendExpanded.data[0];
            nextState = ARBITRATION_AWAIT;
            break;
        case ARBITRATION_AWAIT:
            if (arbitration_success == 0) {
                printf("arbitration failed.\n");
                // I think according to ebus standard, we should retry, but this is not implemented here (yet).
                nextState = FINISHED;
            }else if (arbitration_success == 1) {
                telegramTxRxd.len = 1;
                nextState = SENDDATA;
            }else if (difftime(tnow,tLastStateChange)>1.0) {
                printf("arbitration adapter timeout?\n");
                nextState = FINISHED;
            }
            break;
        case SENDDATA:
            if (difftime(tnow,tLastStateChange)>1.0) {
                printf("send data loopback timeout?\n");
                nextState = FINISHED;
            } else 
            // ebus adapter (at least with Build 20250615) does only accept one character at a time.
            // we need to wait for receiving every single byte back before sending the next one. 
            // you would not call this efficient, at least not if tunneled over TCP, but it is ok for occasional usage.
            // memcpy(chars_to_send_bus, &telegramToSendExpandedEnhanced.data[1], telegramToSendExpandedEnhanced.len-1);
            // chars_to_send_len = telegramToSendExpandedEnhanced.len-1;
            if (telegramToSendExpandedEnhancedIndex == 0) {
                telegramToSendExpandedEnhancedIndex = 2;
            }else if(telegramToSendExpandedEnhancedIndex < telegramToSendExpandedEnhanced.len) {
                // only send once received the last one.
                if (telegramTxRxd.len*2 >= telegramToSendExpandedEnhancedIndex) {
                    chars_to_send_bus[0] = telegramToSendExpandedEnhanced.data[telegramToSendExpandedEnhancedIndex++];
                    chars_to_send_bus[1] = telegramToSendExpandedEnhanced.data[telegramToSendExpandedEnhancedIndex++];
                    chars_to_send_len = 2;
                    chars_to_send_valid = true;
                }
            }
            else {
                nextState = AWAITACK;
            }
            break;
        case AWAITACK:
            ZZ = telegramToSend.data[1];
            if (ZZ == 0xFE) { // no ack or repsonse on broadcasts
                if (difftime(tnow,tLastStateChange)>0.01) {
                    nextState = SENDSYN;
                }
            }
            else if (telegramTxRxd.len >= telegramToSend.len + 1) {
                AK = telegramTxRxd.data[telegramToSend.len];
                if (AK == 0x00) { //ACK
                    nextState = AWAITRESPONSE;
                } else if (AK == 0xFF) { //NAK
                    //retry not implemented.
                    nextState = SENDSYN;
                } else {
                    nextState = SENDSYN;
                }
            }
            else if (difftime(tnow,tLastStateChange)>1.0) {
                printf("ack timeout.\n");
                nextState = FINISHED;
            }
            break;
        case AWAITRESPONSE:
            ZZ = telegramToSend.data[1];
            NN = telegramTxRxd.data[telegramToSend.len+1];
            if (isMasterAddr(ZZ)) { // no content expected
                nextState = SENDSYN;
            } else if (telegramTxRxd.len >= telegramToSend.len+3 && NN <= 16 && telegramTxRxd.len >= telegramToSend.len+3+NN) {
                nextState = SENDACK;
            } else if (difftime(tnow,tLastStateChange)>1.0) {
                printf("response timeout.\n");
                nextState = FINISHED;
            }
            break;
        case SENDACK:
            slaveCRCok = telegramCRCcheck(&telegramTxRxd,true);
            if (slaveCRCok) {//00
                chars_to_send_bus[0] = 0xC4; 
                chars_to_send_bus[1] = 0x80;
                chars_to_send_len = 2;
            }else { //FF
                chars_to_send_bus[0] = 0xC7; 
                chars_to_send_bus[1] = 0xBF;
                chars_to_send_len = 2;
            }
            chars_to_send_valid = true;
            if (slaveCRCok) {
                nextState = SENDSYN;
            } else {
                // retry not implemented
                nextState = FINISHED;
            }
            break;
        case SENDSYN:
            //AA
            chars_to_send_bus[0] = 0xC6; 
            chars_to_send_bus[1] = 0xAA;
            chars_to_send_len = 2;
            chars_to_send_valid = true;
            nextState = FINISHED;
            break;
        case FINISHED:
            printf("sending telegram finished, successful or not.\n");
            nextState = SENDIDLE;
            break;
        default:
        break;
    }

    if (nextState != sendState) {
        sendState = nextState;
        tLastStateChange = tnow;
    }

    if (chars_to_send_valid) {
        *payload = (char*)chars_to_send_bus;
        *len     = chars_to_send_len;
    }
    return chars_to_send_valid;
}



int main(int argc, char *argv[]) {
    State state = START, nextState=START;
    time_t tLastStateChange, tnow;
    tLastStateChange = tnow = time(0);

    char* rcvdTopicName;
    int   rcvdTopicLen;
    MQTTClient_message* rcvdMessage;

    char* totxTopicName;
    char* totxPayload;
    int   totxLen;
    
    MQTTClient client;
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;
    bool totx;

    const char* server_ip = ADAPTER_ADDRESS;
    const int server_port = ADAPTER_PORT;  
    int sock=-1;
    uint8_t initresp_candidate[2] = {0,0};

    uint8_t initdata[] = { 0xC0, 0x81 };

    uint8_t recv_byte = 0; int res; int flags;
    
    if (signal(SIGINT, sig_handler) == SIG_ERR)
        printf("\ncan't catch SIGINT\n");
    if (signal(SIGQUIT, sig_handler) == SIG_ERR)
        printf("\ncan't catch SIGQUIT\n");

	//keep listening for data
	while(1)
	{
        tnow = time(0);

        switch(state) {
            case START:
                nextState=INIT1_MQTT;
                break;
            case INIT1_MQTT: 
                printf("MQTT Start\n");
                if ((rc = MQTTClient_create(&client, ADDRESS, CLIENTID,
                    MQTTCLIENT_PERSISTENCE_NONE, NULL)) != MQTTCLIENT_SUCCESS)
                {
                    printf("Failed to create client, return code %d\n", rc);
                    nextState=DEIN0_PAUS;
                }
                
                // // "If your application calls MQTTClient_setCallbacks(), this puts the client into asynchronous mode"
                // // "In asynchronous mode, the client application runs on several threads. "
                // if ((rc = MQTTClient_setCallbacks(client, NULL, NULL, msgarrvd, NULL)) != MQTTCLIENT_SUCCESS)
                // {
                //     printf("Failed to set callbacks, return code %d\n", rc);
                //     return EXIT_FAILURE;
                // }

                conn_opts.keepAliveInterval = 20;
                conn_opts.cleansession = 1;
                conn_opts.username = USERTOKEN;
                if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS)
                {
                    printf("Failed to connect, return code %d\n", rc);
                    nextState=DEIN0_PAUS;
                }

                //printf("Subscribing to topic %s\nfor client %s using QoS%d\n", TOPIC_TX, CLIENTID, QOS);
                if ((rc = MQTTClient_subscribe(client, TOPIC_TX, QOS)) != MQTTCLIENT_SUCCESS)
                {
                    printf("Failed to subscribe, return code %d\n", rc);
                    nextState=DEIN0_PAUS;
                }
                nextState=INIT2_ATCP;
                break;

            case INIT2_ATCP:

                printf("ETCP Start\n");
                sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) {
                    printf("Fehler beim Erstellen des Sockets\n");
                    nextState=DEIN2_ATCP;
                    break;
                }

                // Serveradresse konfigurieren
                sockaddr_in server_addr;
                memset(&server_addr,0,sizeof(sockaddr_in));
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(server_port);
                server_addr.sin_addr.s_addr = inet_addr(server_ip);

                // Verbindung herstellen
                res = connect(sock, (sockaddr*)&server_addr, sizeof(server_addr));
                if (res < 0) {
                    printf("TCP-Verbindung zum Adapter fehlgeschlagen\n");
                    nextState=DEIN2_ATCP;
                    break;
                }

                // Initialisierung triggern
                if (write(sock, &initdata, 2) != 2) {
                    printf("Fehler beim Senden\n");
                    nextState=DEIN2_ATCP;
                    break;
                }

                // non-blocking setzen
                flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                // disable nagle algo
                flags = 1;
                setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));
                
                nextState = INIT3_AINI;
                break;

            case INIT3_AINI:
                res = read(sock, &recv_byte, 1);
                if (res < 0) {
                    if (errno == EWOULDBLOCK) { //temporary
                        break;
                    }
                    printf("Fehler beim Empfangen\n");
                    nextState=DEIN3_AINI;
                    break;
                }
                if (res == 1) {
                    initresp_candidate[0] = initresp_candidate[1];
                    initresp_candidate[1] = recv_byte;

                    uint8_t initresp_expected[] = { 0xC0, 0x81 };
                    if (initresp_candidate[0] == initresp_expected[0] &&
                        initresp_candidate[1] == initresp_expected[1]) {
                        nextState=INIT4_BUS;
                        break;
                    }
                }
                // check for timeout.
                if (difftime(tnow,tLastStateChange)>2.0) {
                    printf("Timeout beim Empfangen der Initsequenz.\n");
                    nextState=DEIN3_AINI;
                }
                break;
            case INIT4_BUS:
                // here is a good place to anounce our master, scan the bus, or other bus management
                // e.g. 07 FE - inquiry of existence
                // not implemented

                printf("Init completed.\n");
                nextState = WORK;
                break;
            case WORK:
                if (!run) {
                    nextState = RESTART;
                    break;
                }

                if (sendState == SENDIDLE) { // save time if we could not handle a new request anyhow.
                    //try to receive some data, this is a blocking call (with timeout)
                    rcvdMessage = 0;
                    if ((rc = MQTTClient_receive(client, &rcvdTopicName, &rcvdTopicLen, &rcvdMessage, 10)) != MQTTCLIENT_SUCCESS)
                    {
                        if (rc == -1) {
                            printf("Failed to receive message, return code %d, ignoring\n", rc);
                        }
                        else {
                            printf("Failed to receive message, return code %d\n", rc);
                            nextState = RESTART;
                            break;
                        }
                    } 
                    if (rcvdMessage) {
                        msgarrvd(0, rcvdTopicName, rcvdTopicLen, rcvdMessage);
                    }
                }

                //if sth was generated, publish it
                totx = msgPreparedMqtt(&totxTopicName, &totxPayload);
                if (totx) {
                    printf("Publishing  %s\n", totxPayload);
                    pubmsg.payload = totxPayload;
                    pubmsg.payloadlen = (int)strlen(totxPayload);//+1; //include 0 termination? not required.
                    pubmsg.qos = QOS;
                    pubmsg.retained = 0;
                    if ((rc = MQTTClient_publishMessage(client, totxTopicName, &pubmsg, &token)) != MQTTCLIENT_SUCCESS)
                    {
                        printf("Failed to publish message, return code %d\n", rc);
                        //exit(EXIT_FAILURE);
                        // Failed to publish message, return code -1   already seen. 
                        nextState = RESTART;
                        break;
                    } 

                    // printf("Waiting for up to %d seconds for publication of %s\n"
                    //         "on topic %s for client with ClientID: %s\n",
                    //         (int)(TIMEOUT/1000), PAYLOAD, TOPIC, CLIENTID);
                    rc = MQTTClient_waitForCompletion(client, token, TIMEOUT);
                    if (rc != MQTTCLIENT_SUCCESS) {
                        printf("Failed to publish message wfc, return code %d\n", rc);
                        //exit(EXIT_FAILURE);
                        nextState = RESTART;
                        break;
                    }
                    //printf("Message with delivery token %d delivered\n", token);
                }


                // handling of tcp conn. loop due to dumb byte by byte impl.
                do {
                    res = read(sock, &recv_byte, 1);
                    if (res < 0) {
                        if (errno == EWOULDBLOCK) {
                            break;
                        }
                        printf("TCP read error\n");
                        nextState=RESTART;
                        break;
                    }
                    if (res == 1) {
                        //printf("read %02x\n",recv_byte);
                        processEnhBusChar(recv_byte);
                    }
                } while(res>0 && message_to_publish_valid==0); //stop on SYN, as our queue is implemented as length 1 and we need to mqtt-publish first.

                totx = charsPreparedTCP(&totxPayload,&totxLen);
                if (totx) {
                    if (write(sock, totxPayload, totxLen) != totxLen) {
                        printf("TCP write error\n");
                        nextState=RESTART;
                        break;
                    }
                }
                break;

            case RESTART:
                printf("statistics: received %d half-plausible and %d erronous telegrams\n",telegramCountOk, telegramCountBad);
                nextState = DEIN4_BUS;
                break;
            case DEIN4_BUS:
                nextState = DEIN3_AINI;
                break;
            case DEIN3_AINI:
                nextState = DEIN2_ATCP;
                break;
            case DEIN2_ATCP:
                // close TCP
                close(sock);
                sock = -1;
                nextState = DEIN1_MQTT;
                break;
            case DEIN1_MQTT:
                // close MQTT
                if ((rc = MQTTClient_disconnect(client, 10000)) != MQTTCLIENT_SUCCESS)
                    printf("Failed to disconnect mqtt, return code %d\n", rc);
                MQTTClient_destroy(&client);
                nextState = DEIN0_PAUS;
                break;
            case DEIN0_PAUS:
                // delay.
                if (difftime(tnow,tLastStateChange)>10.0) {
                    nextState=START;
                }
                break;
            default:
            break;
        }

        if (state != nextState) {
            //printf("state change %d > %d\n", state, nextState);
            state = nextState;
            tLastStateChange = tnow;
        }

        if (state == DEIN0_PAUS && !run) {
            // exit main loop
            break;
        }

        //if (state != WORK) {
            std::this_thread::yield(); // be cooperative resp scheduling at every state.
        //}
    }

    return 0;
} 



//////////////////////////
// JSON, Strings, byte arrays

// there are plenty of libraries but for our purpose we don't need a library at all.
// as we know our counterpart very well (same project...)
// and we are only using a very small subset of json (a dictionary (str->[str|int]), only space as whitespace, ...)
// so we can make things easy, even in c.
// requires a zeroterminated jsonstr and key as input (however pValue will *not* be zero-terminated)
bool json_lookup(char* jsonstr, const char* key, char** pValue, int* pLen) {
    char quotedkey[32];
    if (!jsonstr || !key || !pValue || !pLen) return false;
    snprintf(quotedkey, sizeof(quotedkey), "\"%s\"", key); quotedkey[sizeof(quotedkey)-1]=0;
    char* foundkey = strstr(jsonstr, quotedkey); // assumes that the object is flat and the key exists only once and may not be also part of a value...
    char* valuestart; char* valueend;
    if (foundkey) {
        valuestart = foundkey + strlen(quotedkey);
        while(*valuestart==':' || *valuestart==' ') valuestart++;
        valueend = valuestart + 1;
        if (*valuestart == '"') { //its a string -> end char is also a quote (and we assume there are none inside the string)
            valuestart++;
            while (*valueend && *valueend != '"') valueend++;
        } else {                  //its an integer or similar -> end char is ',' or '}'
            while (*valueend && *valueend != ',' && *valueend != '}') valueend++;
        }
        *pLen = valueend-valuestart;
        *pValue = valuestart;
        return true;
    }
    return false;
}

bool json_lookup_int(char* jsonstr, char* key, int* pValue) {
    char* val; int len;
    bool ok = json_lookup(jsonstr, key, &val, &len);
    int value = 0;
    if(ok && pValue) {
        //atoi(val)  
        value = strtol(val, 0, 10);
        *pValue = value;
        return true;
    }
    return false;
}


// [0 255] ==> "00 FF"
void bytes2hexstr(uint8_t* arr, int n, char* pStr, int maxlen) {
  int len = 0;
  for (int i=0; i<n && len+2<maxlen; i++) {
    len += sprintf(&pStr[len], "%02X ", arr[i]);
  }
  pStr[--len]=0; // replace last space by termination
}

// "00 FF" ==> [0 255] , return 2
int hexstr2bytes(const char* pStr, int len, uint8_t* arr, int maxn) {
  const char* pBuf=pStr; int i=0;
  char bytestr[3]; bytestr[2]=0;
  for (i=0; i<maxn && pBuf+2<=pStr+len && *pBuf!=0; i++) {
    bytestr[0] = *(pBuf++); bytestr[1] = *(pBuf++); 
    arr[i]=(uint8_t)strtoul(bytestr, 0, 16);
    if (!isxdigit(*pBuf)) pBuf++; //skip space
  }
  return i;
}

// {"telegram":"AA BB"} => struct
//example {"telegram":"10 FE B5 16 03 01 70 10 52 AA","sthelse"=0}
bool json2struct(char* jsonstr, struct Telegram* pTelegram) {
    bool ok = true;
    memset(pTelegram, 0, sizeof(*pTelegram));
    char*   val_data; int len_data;
    ok &= json_lookup(jsonstr, "telegram", &val_data, &len_data);
    if(ok) {
        pTelegram->len = hexstr2bytes(val_data, len_data, pTelegram->data, sizeof(pTelegram->data));
    }
    return ok;
}

// struct ==> {"telegram":"AA BB"}
bool struct2json(struct Telegram* pTelegram,char* jsonstr, int maxLen, int* pLen) {
    memset(jsonstr, 0, sizeof(maxLen));
    char* s=jsonstr;
    if (maxLen >= pTelegram->len*3 + strlen("{xtelegramx=xx}")) {
        strcpy(s, "{\"telegram\":\"");
        s+=strlen(s);
        bytes2hexstr(pTelegram->data,pTelegram->len,s,maxLen-strlen(jsonstr));
        s=jsonstr+strlen(jsonstr);
        strcpy(s, "\"}");
        if (pLen) *pLen=strlen(jsonstr);
        return true;
    }
    return false;
}

