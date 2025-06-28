import wayfire_socket as ws
import os
import json
import time
import random

addr = os.getenv('WAYFIRE_SOCKET')
sock = ws.WayfireSocket(addr)

for output in range(1, 30):
    create_query = ws.get_msg_template('stipc/create_wayland_output')
    response = sock.send_json(create_query)

    time.sleep(random.random() * 0.01)
    destroy_query = ws.get_msg_template('stipc/destroy_wayland_output')
    destroy_query['data']['output'] = ("WL-%d" % (output))
    print("Destroying output " + destroy_query['data']['output'])
    response = sock.send_json(destroy_query)

    time.sleep(0.5)
