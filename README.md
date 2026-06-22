ESP32 PublicFireHose
=============================
Build Instructions
------------------

1.  Initialize the environment.
2.  Set your target chip: esp32c5, c6, s3, etc.
3.  Build the project.
4.  Flash and monitor.

``` bash
. ./esp-idf-v5.5.4/export.sh
idf.py set-target esp32c6
idf.py build
idf.py flash monitor
```
