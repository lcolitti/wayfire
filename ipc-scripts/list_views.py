#!/usr/bin/python3

import os
import sys

from wayfire_socket import *

s = WayfireSocket("/tmp/wayfire-wayland-1.socket")

for v in s.list_views():
  print(v)
