# wepoll

epoll for Windows (https://github.com/piscisaureus/wepoll, the `dist`
branch's single-file build), BSD-2-Clause (see LICENSE). The runtime's
per-thread pollers (runtime/async.c) use it on Windows, where it waits on
sockets through the AFD driver's polling, as epoll does on Linux. Built
only into the Windows runtime.

wepoll.c sha256: b89a91fa9b3b665c6d6cfaac807ed17831202187738284eb5a08514c2b4eaa3c
wepoll.h sha256: 20aa8653cea63caf6f234f278f5681288cd3c5c27aeab6961151b362bf5e47e1
