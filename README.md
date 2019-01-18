# INTRODUCTION
*plget* is a tool used to measure latency packets spent in network stack, NIC
driver and on the wire, trace interpacket gap, based as on h/w as on sw
timestamping, as for RX as for TX path. It can be used to measure complete
latency from wire to an application and from an application to wire, round
trip time (RTT) and more. The plget tool uses socket interface and works
with UDP and PTP l2/l4 packets. Also an extension is being adding to work
with AF_XDP sockets.

Can be useful for developers to trace hw timestamps when packets were sent or
received in case of some shaping verification, like CBS Qdisc in both, offload
and not offload modes, observe interpacket gap, inverstigate interchannel
impact and so on. In case if h/w timestamping is not supported or partly
supported, the sw timestamps are used, but not for everything. All timestamps
are bind to the packet, so no need in any additional tracing tools and tracing
impact is moved to the minimum. Usually PHC counter is running on the same
timeline with system counter, but their synchronization can be done only once
at driver initialization. To avoid mistiming while stack latency measurements,
the phc2sys tool from linux ptp packet should be running:
"phc2sys -s CLOCK_REALTIME -c eth0 -m -O 0". It allows to keep in sync
two time counters in sub-microsecond level accuracy, for TI AM572x it was
~172ns in average, depends on discreteness of timers and their accuracy).

One of the possible test models:

![](https://github.com/ikhorn/plget/blob/master/example_latency_test_model.jpg)


# PURPOSE
This tool is used for measuring the following:
* packet latency for networking stack for RX and TX
* NIC driver latency (hw timestamping is required) for RX and TX
* round trip time (RTT), NIC->server->NIC (hw timestamping is advisable)
* tracing interpacket gap (hw timestamping is required) for RX and TX
* tracing time of sending (hw timestamping is required)
* figuring out bottlenecks for hi priority packets
* figuring out interclass impact for AVB
* speed based on h/w or s/w timestamps + packet generator
* ...

# COMPILATION
~~~
:~# make #for native
:~# export ARCH=arm; export CROSS_COMPILE=arm-linux-gnueabihf-; make #for cross
:~# export ARCH=arm; export CC=arm-linux-gnueabihf-gcc; make #for cross
~~~

# HELP
~~~
:~# plget -h
~~~

# PRINTOUTS
Printout can be tuned with -f option. "hwts" means print h/w timestamps (if they
are present ofc) normalized to first packet timestamp, as absolute time is not
relevant, "ipgap" means time between neighbor h/w timestamps and useful to see
how h/w shaping behaves in case of several streams etc. The "lat" is set by
default, but only if -f option is not used. All can be used in one line
"-f lat,hwts,ipgap". Also, if stack latency before entering packet scheduler is
needed, -f "sched" can be ued, relevant only for modes with TX, like echo-lat,
rtt, tx-lat.

# EXAMPLES

## RX LATENCY EXAMPLES
~~~
				examples scheme
  +-----------------+      +-------------------------------------------------+
  |  WORKSTATION    |      |                   TARGET BOARD                  |
  |-----------------|      |---------------------------+---------------------|
  |                 |      |        Kernel space       |     User space      |
  |                 |      |------------+--------------+---------------------|
  |                 |      | NIC driver | Net stack    | RT application      |
  |                 |      |---+        +---+          +---+                 |
  | client          |      |ts1|        |ts2|          |ts3| plget -m rx-lat |
  | pkt generator   |=====>|   |------->|   |--------->|   |            ...  |
  | plget -m tx-lat |      |PHC|        |SYS|          |SYS|                 |
  |            ...  |      |CLK|        |CLK|          |CLK|                 |
  |                 |      |---\        +---\          +---\                 |
  |                 |      |    \       |    \         |    \  phc2sys       |
  |                 |      |     \______|_____\________|_____\___            |
  |                 |      |            |              |       sync          |
  +-----------------+      +-------------------------------------------------+
~~~

For all examples run phc2sys, to keep PHC and system clock in sync, if h/w
timestamping is supported ofc:
~~~
:~# phc2sys -s CLOCK_REALTIME -c eth0 -m -O 0 > /dev/null &
~~~

Use "-f lat,hwts,ipgap" if more printouts are needed, see "plget -h", as for
the target board as for the server (if you need server latency too).

### Example 1: UDP RX latency
Measure RX latency for UDP packets, port 385.

On target board (192.168.3.16):
~~~
:~# plget -i eth0 -t udp -u 385 -m rx-lat -n 16
~~~

On client side generate appropriate packets:
~~~
:~# plget -i eth0 -t udp -u 385 -m tx-lat -n 16 -s 100 -l 512 -a 192.168.3.16
~~~

### Example 2: PTP l4 RX latency
Measure RX latency for PTP l4 packets (port 319), useful if the driver
supports h/w timestamping only for PTP kind packets.

On target board (192.168.3.16):
~~~
:~# plget -i eth0 -t udp -u 319 -m rx-lat -n 16
~~~
or
~~~
:~# plget -i eth0 -t ptpl4 -m rx-lat -n 16
~~~

On client side generate appropriate packets:
~~~
:~# plget -i eth0 -t udp -u 319 -m tx-lat -n 16 -s 100 -l 512 -a 192.168.3.16
~~~
or
~~~
:~# plget -i eth0 -t ptpl4 -m tx-lat -n 16 -s 100 -l 512 -a 192.168.3.16
~~~

Example 3: AVTP RX latency
----------
Measure RX latency for avtp packets (IEEE 1722)

On target board (c8:a0:30:b4:94:03):
~~~
:~# plget -i eth0 -t avtp -m rx-lat -n 16
~~~

On client side generate appropriate packets:
~~~
:~# plget -i eth0 -t avtp -m tx-lat -n 16 -s 100 -l 512 -a c8:a0:30:b4:94:03
~~~

Example 4: PTP l2 RX latency
----------
Measure RX latency for ptpl2 packets (IEEE 1588)

On target board (c8:a0:30:b4:94:03):
~~~
:~# plget -i eth0 -t ptpl2 -m rx-lat -n 1600
~~~

On client side generate appropriate packets:
~~~
:~# plget -i eth0 -t ptpl2 -m pkt-gen -n 1600 -l 512 -s 400 -a 74:da:ea:47:7d:9d
~~~

If address is not set for pkt-gen or tx-lat modes then default multicast address
is used: 01:1B:19:00:00:00 in this case rx-lat should set address explicitly.

## TX LATENCY EXAMPLES
~~~
		  examples scheme
+-------------------------------------------------+
|                   TARGET BOARD                  |
|---------------------+---------------------------|
|     User space      |        Kernel space       |
|---------------------+--------------+------------|
| RT application      | Net stack    | NIC driver |
|                 +---|          +---|        +---|
|                 |ts1|          |ts2|        |ts3|
| plget -m tx-lat |   |--------->|   |------->|   |====> Eth
|                 |SYS|          |SYS|        |PHC|
|                 |CLK|          |CLK|        |CLK|
|                 /---|          /---|        /---|
|       phc2sys  /    |         /    |       /    |
|       ________/_____|________/_____|______/     |
|       sync          |              |            |
+-------------------------------------------------+
~~~

No need in any additional network nodes to be involved.
For all examples run phc2sys, to keep PHC and system clock in sync, if h/w
timestamping is supported ofc:
~~~
:~# phc2sys -s CLOCK_REALTIME -c eth0 -m -O 0 > /dev/null &
~~~

Use "-f lat,hwts,ipgap" if more printouts are needed, see "plget -h".

Example 1: UDP TX latency
----------
Measure TX latency for UDP packets, port 385.

~~~
:~# plget -i eth0 -t udp -u 385 -m tx-lat -n 16 -s 100 -l 512 -a 192.168.2.1
~~~

Example 2: PTP l4 TX latency
----------
Measure TX latency for PTP l4 packets (port 319), useful if a driver
supports h/w timestamping only for PTP kind packets.

~~~
:~# plget -i eth0 -t udp -u 319 -m tx-lat -n 16 -s 100 -l 512
~~~

Example 3: AVTP TX latency
----------
Measure TX latency for avtp packets (IEEE 1722).

~~~
:~# plget -i eth0 -t avtp -m tx-lat -n 16 -s 10 -l 512 -a 74:da:ea:47:7d:9d
~~~

Example 4: PTP l2 TX latency
----------
Measure TX latency for ptpl2 packets (IEEE 1588)

~~~
:~# plget -i eth0 -t ptpl2 -m tx-lat -n 160 -l 512 -s 10 -a 74:da:ea:47:7d:9d
~~~

If address is not specified then 01:1B:19:00:00:00 is used.

Example 5:  PTP l2 TX latency and its packet scheduler part
---------
Measure TX latency for ptpl2 packet, but also get latency in
packet scheduler (IEEE 1588)

~~~
:~# plget -if=eth0 --type=ptpl2 --mode=tx-lat --pkt-num=16 --pkt-size=512 \
--format=sched,lat --pps=100
~~~

or if vlan is used (one more sched ts):

~~~
:~# plget -if=eth0.100 --type=ptpl2 --mode=tx-lat --pkt-num=16 --pkt-size=512 \
--format=sched,lat --pps=100 --dev-deep=2
~~~

## RTT and ECHO LATENCY EXAMPLES
~~~
		     examples scheme
+--------------------------------------------------+
|                    TARGET BOARD 1                |
|----------------------+---------------------------|
|     User space       |        Kernel space       |
|----------------------+--------------+------------|
| RT app               | Net stack    | NIC driver |
|                  +---|          +---|        +---|
|                  |ts1|          |ts2|        |ts3|
| plget -m rtt     |   |--------->|   |------->|   |
|                  |SYS|          |SYS|        |PHC|<===> Eth
|                  |CLK|<---------|CLK|<-------|CLK|
|                  |   |          |   |        |   |
|                  |ts6|          |ts5|        |ts4|
|                  /---|          /---|        /---|
|        phc2sys  /    |         /    |       /    |
|        ________/_____|________/_____|______/     |
|        sync          |              |            |
+--------------------------------------------------+


      +--------------------------------------------------+
      |            TARGET BOARD 2 (ECHO)                 |
      |---------------------------+----------------------|
      |        Kernel space       |     User space       |
      |------------+--------------+----------------------|
      | NIC driver | Net stack    | RT app               |
      |---+        +---+          +---+                  |
      |ts1|        |ts2|          |ts3| plget -m echo-lat|
 Eth  |   |------->|   |--------->|   |----+      ...    |
<====>|PHC|        |SYS|          |SYS|    |             |
      |CLK|<-------|CLK|<---------|CLK|<---+             |
      |   |        |   |          |   |                  |
      |ts6|        |ts5|          |ts4|                  |
      |---\        +---\          +---\                  |
      |    \       |    \         |    \  phc2sys        |
      |     \______|_____\________|_____\___             |
      |            |              |       sync           |
      +--------------------------------------------------+
~~~

In this case plget has to be running on workstation and on testing board.
It measures round trip time based on NICs h/w timestamps. And in the same time
the latencies on board itself are also measured, both TX and RX, as packet is
echoed with testing board. Packets are sent in order, the new packet is not sent
till last packet has not been received back. Can be useful if TX and RX
latencies should be measured for 2 different boards, twice faster.

For all examples run phc2sys, to keep PHC and system clock in sync, if h/w
timestamping is supported ofc:
~~~
:~# phc2sys -s CLOCK_REALTIME -c eth0 -m -O 0 > /dev/null &
~~~

Use "-f lat,hwts,ipgap" if more printouts are needed, see "plget -h".

Example 1: UDP TX, RX latencies and RTT
----------
Measure TX, RX latencies and rtt for UDP packets, port 385.

On board 1 (192.168.3.16):
~~~
:~# plget -i eth0 -t udp -u 385 -m rtt -n 16 -l 512 -a 192.168.3.20
~~~

On board 2 (192.168.3.20):
~~~
:~# plget -i eth0 -t udp -u 385 -m echo-lat -n 16 -l 512 -a 192.168.3.16
~~~

Example 2: PTP l4 TX, RX latencies and RTT
----------
Measure TX, RX latencies and RTT for PTP l4.

On workstation (client):
~~~
:~# plget -i eth0 -t udp -u 319 -m rtt -n 16 -l 512
~~~

On target board:
~~~
:~# plget -i eth0 -t udp -u 319 -m echo-lat -n 16 -l 512
~~~

By default multicast 224.0.1.129 address is used. In case the other is
needed, use smth like -a 224.0.1.130 as for taget board as for client.

Example 3: AVTP TX, RX latencies and RTT
----------
Measure TX, RX latencies and RTT for avtp packets (IEEE 1722)

On workstation (client 74:da:ea:47:7d:9d):
~~~
:~# plget -i eth0 -t avtp -m rtt -n 16 -l 512 -a c8:a0:30:b4:94:03
~~~

On target board (c8:a0:30:b4:94:03):
~~~
:~# plget -i eth0 -t avtp -m echo-lat -n 16 -l 512 -a 74:da:ea:47:7d:9d
~~~

Example 4: PTP l2 TX, RX latencies and RTT
----------
Measure TX, RX latencies and RTT for ptpl2 packets (IEEE 1588)

On workstation (client 74:da:ea:47:7d:9d):
~~~
:~# plget -i eth0 -t ptpl2 -m rtt -n 16 -l 512 -a c8:a0:30:b4:94:03
~~~

On target board (c8:a0:30:b4:94:03):
~~~
:~# plget -i eth0 -t ptpl2 -m echo-lat -n 16 -a 74:da:ea:47:7d:9d
~~~

Or use default multicast group and run appropriately:
~~~
:~# plget -i eth0 -t ptpl2 -m rtt -n 16 -l 512
:~# plget -i eth0 -t ptpl2 -m echo-lat -n 16
~~~

## RECEIVE RATE AND PACKET GEN MODES EXAMPLE
On one side run packet generator, on another plget tool in "rx-rate" mode

Example:
--------

On workstation, packet generator:
~~~
:~# plget -i eth0 -t udp -u 385 -m pkt-gen -n 1600 -l 512 -s 103 -a 192.168.3.16
~~~

On target board (192.168.3.16), measure periodically rate:
~~~
:~# plget -i eth0 -t udp -u 385 -m rx-rate
~~~

By default rate is measured once a second, but can be tuned with -s.
For measurements h/w timestamps are used if possible, otherwise software.

## "HWTS" or/and "IPGAP" EXAMPLE
For next examples, replace or add "ipgap" to -f command to get interpacket gap.

Example 1:
----------
Get plot, how packets are put on line:
~~~
:~# plget -i eth0 -t ptpl2 -m tx-lat -n 16 -s 100 -l 512 -f hwts
~~~

Packets are shown in relative to first packet ts time.

Example 2:
----------
To measure tx-lat or hwts with several applications in parallel, and print plots
for two applications on the same time line (not relative to first ts time) the
following script can be used:
~~~
#!/bin/sh
# get hardware timestamps for both streams to observe inter streams impact

# get common time base running plget w/o arguments, to present readable
# timestamps in one time line.

BASE_TIME=$(./plget)

# run stream 1 with prio 3 and stream id = 0 to get hwts to tss1 file
plget -i eth0 -t ptpl2 -m tx-lat -n 16 -s 100 -l 512 -f hwts -p 3 \
	-r $BASE_TIME -k 0 > tss1&

# run stream 2 with prio 3 and stream id = 1 to get hwts to tss2 file
plget -i eth0 -t ptpl2 -m tx-lat -n 16 -s 100 -l 512 -f hwts -p 2 \
	-r $BASE_TIME -k 1 > tss2
~~~
All this can be done getting in parallel "latency" and "ipgap"
