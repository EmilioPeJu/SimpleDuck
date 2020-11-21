# SimpleDuck

SimpleDuck is a very simplified remake of [WiFiDuck](https://github.com/SpacehuhnTech/WiFiDuck) concept.
It injects keystrokes specified in a ducky script.

## Quickstart

1. Get an Arduino pro micro (3.3V version) and program it with the code in the folder "micro".
2. Get an ESP32 and use the esp-idf environment to configure (especially, the wifi parameters), build and program it with the code in the folder "esp".
3. Connect the serial port 2 in the ESP32 to the serial port 1 in the Arduino pro micro. Don't forget about 3V3 and GND.
4. Connect the USB port in the Arduino pro micro to a the target computer and run the example.

## Example

Load and run an example ducky script in the ESP32 with the ip 192.168.1.237
```bash
$ cat example
DEFAULT_DELAY 50
STRING echo SimpleDuck did it
ENTER

$ ./simpleduck.py --ip 192.168.1.237 -l example -r
Loading... OK
Running... OK
```
