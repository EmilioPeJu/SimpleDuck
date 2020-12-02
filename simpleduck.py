#!/usr/bin/env python3
#
#   This code is in the Public Domain (or CC0 licensed, at your option.)
#
#   Unless required by applicable law or agreed to in writing, this
#   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
#   CONDITIONS OF ANY KIND, either express or implied.

from argparse import ArgumentParser

import os
import socket
import struct
import sys
KEYS = {
    b"SPACE": 0x20,
    b"PRINTSCREEN": 0x6b,
    b"PRINT": 0x6b,
    b"CONTROL": 0x80,
    b"CTRL": 0x80,
    b"SHIFT": 0x81,
    b"ALT": 0x82,
    b"GUI": 0x83,
    b"ENTER": 0xb0,
    b"RETURN": 0xb0,
    b"ESC": 0xb1,
    b"ESCAPE": 0xb1,
    b"BACKSPACE": 0xb2,
    b"TAB": 0xb3,
    b"CAPSLOCK": 0xc1,
    b"F1": 0xc2,
    b"F2": 0xc3,
    b"F3": 0xc4,
    b"F4": 0xc5,
    b"F5": 0xc6,
    b"F6": 0xc7,
    b"F7": 0xc8,
    b"F8": 0xc9,
    b"F9": 0xca,
    b"F10": 0xcb,
    b"F11": 0xcc,
    b"F12": 0xcd,
    b"INSERT": 0xd1,
    b"HOME": 0xd2,
    b"PAGE_UP": 0xd3,
    b"DEL": 0xd4,
    b"END": 0xd5,
    b"PAGE_DOWN": 0xd6,
    b"DELETE": 0xd4,
    b"RIGHT": 0xd7,
    b"RIGHTARROW": 0xd7,
    b"LEFT": 0xd8,
    b"LEFTARROW": 0xd8,
    b"DOWN": 0xd9,
    b"DOWNARROW": 0xd9,
    b"UP": 0xda,
    b"UPARROW": 0xda,
}

DUCKY_TO_COMMANDS = {
    b"STRING ": b"s",
    b"DELAY ": b"d",
    b"DEFAULT_DELAY ": b"D",
    b"REPEAT ": b"R"
}

TERMINATOR = b"\n"


def convert_ducky_script(data):
    # convert ducky script to a format that the board parses
    result = bytearray()
    for line in data.split(b"\n"):
        if line.startswith(b"REM") or not line:
            # comments are ignored
            continue
        processed = False
        for dcommand, command in DUCKY_TO_COMMANDS.items():
            if line.startswith(dcommand):
                # it's a command
                processed = True
                result.extend(command + line[len(dcommand):] + TERMINATOR)
                break
        if not processed:
            # it's a key press combination
            press_seq = bytearray()
            rel_seq = bytearray()
            for key in line.split(b" "):
                key = key.strip()
                key_val = KEYS.get(key)
                if key_val is not None or len(key) == 1:
                    press_seq.extend(b"p")
                    rel_seq.extend(b"r")
                    press_seq.append(key_val or ord(key))
                    rel_seq.append(key_val or ord(key))
                else:
                    raise ValueError(f"Invalid token {key} in {line}")
            result.extend(press_seq)
            result.extend(rel_seq)
            result.extend(TERMINATOR)
    return bytes(result)


def swap_repeat(data):
    # put repeat command before the line to be repeated
    # this way it's easier for the board to parse
    result = bytearray()
    sdata = data.split(b"\n")
    for i in range(1, len(sdata)):
        if sdata[i].startswith(b"REPEAT "):
            sdata[i - 1], sdata[i] = sdata[i], sdata[i - 1]
    return b"\n".join(sdata)


def parse_args():
    parser = ArgumentParser()

    def valid_path(arg):
        if not os.access(arg, os.R_OK) or not os.path.isfile(arg):
            parser.error('Invalid file path')
        return arg

    parser.add_argument('--ip', required=True)
    parser.add_argument('--port', default=3333)
    parser.add_argument('-r', dest='run', action='store_true',
                        help="Run the last script loaded")
    parser.add_argument('-k', dest='kill', action='store_true',
                        help="Kill a running script")
    parser.add_argument('-l', dest='load', type=valid_path, default=None,
                        help="File to load as script")
    return parser.parse_args()


def main():
    args = parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((args.ip, args.port))
    if args.load:
        with open(args.load, 'rb') as fhandle:
            payload = convert_ducky_script(swap_repeat(fhandle.read()))
        pkt = b"b" + struct.pack("<H", len(payload)) + payload
        sock.sendall(pkt)
        print(f"Loading... {sock.recv(16).decode().strip()}")
    if args.run:
        sock.sendall(b'r\x00\x00')
        print(f"Running... {sock.recv(16).decode().strip()}")
    if args.kill:
        sock.sendall(b'k\x00\x00')
        print(f"Killing... {sock.recv(16).decode().strip()}")
    sock.close()


if __name__ == "__main__":
    main()
