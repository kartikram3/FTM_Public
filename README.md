FTM
---
This code implements the scheme described in the paper :
"First Time Miss : Low Overhead Mitigation for Shared Memory
Cache Side Channels, Ramkrishnan et al, ICPP-49, Aug 2020.

Dependencies
------------
1. Ubuntu 12.04
2. External dependencies: `gcc >=4.6, pin, scons, libconfig, libhdf5, libelfg0, python`
3. Pin version used : pin-2.14-71313-gcc.4.4.7-linux

Install Steps
-------------

1. Install Ubuntu 12.04.4 on your computer, or a VM of your choice. Zsim may work
on newer versions, but I have not tested the code on newer versions, and had
trouble with version > 16.04 due to Zsim crashing during runtime.

2. Install the dependencies using the following
commands 
```shell
apt-get -y update
apt-get -y install build-essential g++ git scons python
apt-get -y install libelfg0-dev libhdf5-serial-dev libconfig++-dev
```
3. Next, clone this repository using the command 
```bash
https://github.com/kartikram3/FTM_Public
```
4. Extract Pin using the command 
```bash
tar -xvzf pin-2.14-71313-gcc.4.4.7-linux.tar.gz
```
Set the PINPATH environment variable to Pin's base directory. 

5. Navigate into the FTM_Public directory. Then, compile the code using the command :
```bash
scons -j4
```
6. Run the test case using the command
```bash
./build/opt/zsim ./scripts/test.cfg
```

Experiments
-----------

  Example configuration files used to run experiments are in the  ```experiments``` folder.

License & Copyright
-------------------

zsim is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation,
version 2.

zsim was originally written by Daniel Sanchez at Stanford University, and per
Stanford University policy, the copyright of this original code remains with
Stanford (specifically, the Board of Trustees of Leland Stanford Junior
University). Since then, zsim has been substantially modified and enhanced at
MIT by Daniel Sanchez, Nathan Beckmann, and Harshad Kasture. zsim also
incorporates contributions on main memory performance models from Krishna
Malladi, Makoto Takami, and Kenta Yasufuku.

zsim was also modified and enhanced while Daniel Sanchez was an intern at
Google. Google graciously agreed to share these modifications under a GPLv2
license. This code is (C) 2011 Google Inc. Files containing code developed at
Google have a different license header with the correct copyright attribution.

Additionally, if you use this software in your research, we request that you
reference the zsim paper ("ZSim: Fast and Accurate Microarchitectural
Simulation of Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June
2013) as the source of the simulator in any publications that use this
software, and that you send us a citation of your work.
