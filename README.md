# ebusd-light-B5
A lightweight [ebusd](https://github.com/john30/ebusd) alternative for Vaillant heatpumps

## Goal

A data interface for a Vaillant heatpump.

More precisely: I wanted to monitor my heatpump's operation, especially the various analog sensors. Focus is on gaining the values and technical aspects (no nice displaying etc).

## Scope

The solution needs to work at least with the system of:

| Mfr | Device |  MON | DoM | Inst.ass. | FW-Vers. 
| -- | -- | -- | -- | -- | -- |
| Vaillant | **aroTHERM plus** VWL 75/6 A 230V S2 R1 | 0010021118 | 07/2023 | 09.02 |  0351.09.02
| Vaillant | **VWZ AI**, VWL x/6 MB1 | 0010031643 | | 02.02 |  0301.02.02
| Vaillant | sensoCOMFORT **VRC 720/3** | 0010045478 | | 07.09 |  0360.03.01

<!--- 
 Installationsassistent: 
 Systemtregler    07.09
 Wärmepumpe 1     09.02
 WP-Regelunsmodul 02.02
 an VWZ AI abgelesen:
 00          0301.02.02
 01          0351.09.02
 02          0360.03.01
 https://www.haustechnikdialog.de/Forum/t/272615/Platinenversion-und-Systemregler-VRC-720-und-720-3
 00= Platine Hydraulikstation
 01= Platine WP Außeneinheit
 02= Display
-->

It is very likely and desirable that such a solution will also work for similar devices. I am confident for the aroTHERM plus series, other series may [not be compatible](https://www.haustechnikdialog.de/Forum/t/254257/Eigenbau-Arotherm-plus-75-6?page=4).

Other vendors are definitely out of scope, as Vaillant uses a mostly proprietery command set (`"B5"`). However, the link layer may be reused.

Disclaimer: Vaillant and/or its device names are registered trademark of Vaillant GmbH, 42859 Remscheid. I am not affiliated to Vaillant, I am an end user. No more, no less. All my proposals are offered in the hope they may be useful. In doubt, refer to official sources only and do not modify anything.


## Approach

There is a promising wide-spread project: [ebusd](https://github.com/john30/ebusd), 685 stars on github. I gave it a try and failed.

It is not trivial to find a working combination of [hardware](https://adapter.ebusd.eu/)/firmware, software-daemon, and particularly its configuration files matching your setup. Despite some [automatic configuration](https://github.com/john30/ebusd/wiki/4.7.-Automatic-configuration), Vaillant users report some [effort](https://community.openenergymonitor.org/t/new-vaillant-arotherm-plus-7kw-install/26459/13) for preparing configuration files, which are shared as [alternative repos](https://github.com/cyberthom42/vaillant-arotherm-plus), [files](https://github.com/john30/ebusd/discussions/720#discussioncomment-5574781) to be changed or other [tweaking](https://github.com/john30/ebusd/discussions/720#discussioncomment-4129260). The required format is software [version dependant](https://github.com/john30/ebusd-configuration/issues/443), and in 2024 the project added an additional [TypeSpec](https://github.com/john30/ebusd-configuration?tab=readme-ov-file#usage-with-ebusd) layer and thus changed its configuration format completely, aimed to be "way easier to maintain and understand". I am not sure if *adding complexity* is a good way to achieve this. And asked myself how to build things *simpler*. I'd rather spend time in technical investigation of a constant setup, than managing artificial software compatibility issues.



## Architecture

Inspired by ebusd and using its hardware, we have a UART-like interface to the bus.

```mermaid
flowchart LR
a(heatpump system) <-->|ebus|b(adapter) <-->|UART over TCP/eth|c(ebusd)
```

My adapter is an [eBUS Adapter Shield C6](https://adapter.ebusd.eu/v5-c6/) with ethernet shield, running the firmware build 20250615. Ethernet allows me to place the server within arbitrary distance to the ebus [adapter]. Drawback is a big overhead, as ebusd was originally made for local UART and in general uses a whole TCP packet for every single bus byte. At the time of writing in Sep'2025, the adapter offers arbitration offloading, but no rx or tx buffer. However, sending whole telegrams ["is ](https://github.com/john30/ebusd/blob/255f176861ac9e41ba983183ffc90333ed2ad135/docs/enhanced_proto.md) [planned"](https://github.com/john30/ebusd/blob/master/docs/enhanced_proto.md).

Fun fact: As far as I can see, only ebusd's server component is open source - the adapter firmware seems not. Edit: In Nov'25 I realized a possible reason: the author introduced [micro-ebusd](https://token.ebusd.eu/) in Oct'25. It can ["send arbitrary messages from hex input" on "the adapter directly"](https://token.ebusd.eu/) and much more, if you "[purchase](https://token.ebusd.eu/) a token".

As a first step, I think of a passive tap, only listening on the bus. This reduces complexity a lot in contrast to mimicing a full ebus device. There are two main tasks to accomplish:
 - translating (a stream of) UART bytes to telegrams
 - translating a telegram to values

The first step is classical [Layer2 (Link Layer)](https://en.wikipedia.org/wiki/OSI_model#Layer_2:_Data_link_layer) task. Output of the second step may be a JSON-formatted list of key-value pairs.

As the heatpump system comprises of 3 participants, some data is regularly transferred on the bus and we can just read it, too. Low risk, transparent to other bus participants, not taking any bandwidth. Unfortunately some data must be requested, so we need to transmit ebus master requests:
 - phrasing our request as an ebus master telegram 
 - transmitting the ebus telegram on the bus

As ebus is a multi-master bus, the latter is quite effortful. It includes arbitration, waiting for the slave response, and releasing the line. See [introduction to ebus](https://web.archive.org/web/20211208101047/https://ebus-wiki.org/lib/exe/fetch.php/ebus/ebus_basics_de.pdf) p.7. [et.al.](https://web.archive.org/web/20211208101047/https://ebus-wiki.org/doku.php/ebus/ebusdoku).

As "full message sending" is not yet implemented in the adapter yet and I want to omit ebusd, I first built a link layer implementation with limited feature set, see `ebusd-light.cpp`:
 - RX: publishing telegram candidates (=bytes between two SYNs) to MQTT `ebus/ll/rx`
 - TX: send master requests from `ebus/ll/tx`

Wether a master request was successful, or for reading the response, the RX topic needs to be evaluated. These two MQTT topics are the only interface to higher level processing. This abstraction allows us to change the language to python for higher level things, including reverse engineering telegrams.

The higher-level program is quite staight-forward: a tree of if-statements decodes known values and re-publishes values to MQTT. While the result may resemble similar to ebusd, this solution is much less generic and thus has a very limited scope of application. On the other hand, it follows the [KISS principle](https://en.wikipedia.org/wiki/KISS_principle) and maybe 1k [loc](https://en.wikipedia.org/wiki/Source_lines_of_code) incl. config are easier to adapt for you than 22k [loc](https://en.wikipedia.org/wiki/Source_lines_of_code) excl. config.

```mermaid
flowchart LR
a(heatpump system) <-->|ebus|b(adapter) <-->|TCP|c(ebusd-light) <-->|MQTT|d(ebus-B5-decoder)
```

Currently the set of decodable values is limited and there is currently no seperate configuration at all, all hardcoded. Sufficient for demonstration or minimalistic purposes, but not production-ready.



<!--- 
## Exemplary Use: Check for a known firmware bug

Planned: I want to use data to proof the absence of [this](https://community.openenergymonitor.org/t/vaillant-arotherm-firmware-351-06-07-problems-energy-integral/26186) [bug](https://knx-user-forum.de/forum/%C3%B6ffentlicher-bereich/knx-eib-forum/2040543-bug-in-vaillant-arotherm-firmware-351-06-07).

## Vaillant vs ebus Standard

See if vaillant does escape AA in the data.
If not, the do not only use solely 5B, are not even compatibel on link layer.

-->


## How to use

At the moment this is just a demonstrator. You will need to modify it for your needs, at least change ip adresses and compile (instructions in the code).

Impression of what you can get:

```
mosquitto_sub -h 192.168.x.y -t ebus/ll/rx
{"telegram":"10 08 B5 11 01 01 89 00 09 3D 3E 00 80 FF FF 00 00 FF C2 00 AA"}
{"telegram":"10 76 B5 11 01 01 16 00 09 FF FF 20 0A FF FF 00 00 FF 15 00 AA"}
{"telegram":"10 08 B5 10 09 00 00 40 FF FF FF 06 00 00 BC 00 01 01 9A 00 AA"}
{"telegram":"03 76 B5 12 06 13 00 0C 4B 03 00 B6 00 02 00 FF D3 00 AA"}
{"telegram":"10 08 B5 11 01 00 88 00 09 EE 01 0C 00 00 08 00 00 00 D1 00 AA"}

mosquitto_sub -h 192.168.x.y -t ebus/ll/rxd
{"ForwTempL[C]": 30.5, "RetnTempL[C]": 31.0}
{"OutdTemp[C]": 10.125}
{"ForwSetT[C]": 32.0}
{"WPres[bar]": 1.2, "WFlow[l/h]": 843}
{"ForwTemp[C]": 30.875, "WPres[bar]": 1.2, "AAAA[?]": 0, "OpMode": 8}
```

