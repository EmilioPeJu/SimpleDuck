/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <Keyboard.h>
#define BUFFER_SIZE (512)
#define TERMINATOR '\n'

void setup()
{
    Serial1.begin(9600);
    Keyboard.begin();
}

void loop()
{
    char buff[BUFFER_SIZE];
    unsigned int index = 0;
    if (Serial1.available()) {
        size_t len = Serial1.readBytesUntil(TERMINATOR, buff, BUFFER_SIZE - 1);
        buff[len] = 0;
        process_command(buff);
    }
}

void process_command(char *command)
{
    char *ptr;
    switch (command[0]) {
    // press/release command
    case 'p':
    case 'r':
        ptr = command;
        while (*ptr && *ptr != TERMINATOR) {
            if (*ptr == 'p')
                Keyboard.press(ptr[1]);
            else
                Keyboard.release(ptr[1]);
            ptr += 2;
        }
        break;
    // terminator
    case 't':
        Keyboard.print(TERMINATOR);
        break;
    // string command
    case 's':
        ptr = command + 1;
        while (*ptr && *ptr != TERMINATOR) {
            ptr++;
        }
        *ptr = 0;
        Keyboard.print(&command[1]);
        break;
    }
}
