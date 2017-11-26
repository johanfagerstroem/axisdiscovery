Axis Device Discovery Utility
=========================================
A small command-line utility that lists Axis devices on your network using
UPnP/SSDP.

Written in C for GNU/Linux using no external libraries.

Build:
------
Just strike a make command.
```sh
$ make
```
Put the output binary in your path if you like.

Run:
----
```sh
$ ./axisdiscovery 192.168.1.255
C1004-E          ACCC8E123456  http://192.168.1.119:80/
M1054            00408C123456  http://192.168.1.101:80/
P3224-V Mk II    ACCC8EABCDEF  http://192.168.1.100:80/
```
If no broadcast address is provided, the default UPnP address (239.255.255.250)
will be used.

Maybe future features
----------------------
* Command-line options to sort device list based on MAC or IP instead of model
  number.
* Microsoft Windows portability
* Travis-CI to build statically linked binaries for some different archs

Disclaimer:
-----------
This software has no affiliation with Axis Communications AB.
