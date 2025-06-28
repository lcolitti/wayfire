import wayfire_socket as ws
import os
import sys
import json

addr = os.getenv('WAYFIRE_SOCKET')
sock = ws.WayfireSocket(addr)

query = ws.get_msg_template('stipc/destroy_wayland_output')
query['data']['output'] = sys.argv[1]
response = sock.send_json(query)
print(response)
